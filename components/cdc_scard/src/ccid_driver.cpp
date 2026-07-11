#include "cdc_scard/ccid.h"
#include "cdc_scard/applet.h"
#include "cdc_log.h"

extern "C" {
#include "tusb.h"
#include "device/usbd_pvt.h"
}

#include <esp_attr.h>
#include <string.h>
#include <stdio.h>

static const char* TAG = "CCID";

#define CCID_LOG(tag, fmt, ...) LOG_I(tag, fmt, ##__VA_ARGS__)
#define CCID_LOG_E(tag, fmt, ...) LOG_E(tag, fmt, ##__VA_ARGS__)
#define CCID_LOG_W(tag, fmt, ...) LOG_W(tag, fmt, ##__VA_ARGS__)

#define CCID_RX_BUF_SIZE (CCID_MAX_MSG_SIZE + CCID_HEADER_SIZE)
#define CCID_TX_BUF_SIZE (CCID_MAX_MSG_SIZE + CCID_HEADER_SIZE)

// Must live in internal SRAM: ESP32-S3 USB OTG DMA cannot access PSRAM.
static uint8_t ccid_rx_buf[CCID_RX_BUF_SIZE];
static uint8_t ccid_tx_buf[CCID_TX_BUF_SIZE];

static struct {
    bool initialized;
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t rhport;

    uint16_t rx_len;
    bool rx_pending;

    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t error_count;
} ccid_state;

static void ccid_driver_init(void) {
    CCID_LOG(TAG, "CCID driver init");
    memset(&ccid_state, 0, sizeof(ccid_state));
}

static void ccid_driver_reset(uint8_t rhport) {
    (void)rhport;
    // USB bus reset acts like a card power cycle for the applets.
    scard_reset();
    ccid_state.rx_len = 0;
    ccid_state.rx_pending = false;
    ccid_state.initialized = false;
    ccid_state.rx_count = 0;
    ccid_state.tx_count = 0;
    ccid_state.error_count = 0;
}

static uint16_t ccid_driver_open(uint8_t rhport, tusb_desc_interface_t const* desc_itf, uint16_t max_len) {
    if (desc_itf->bInterfaceClass != TUSB_CLASS_SMART_CARD) {
        return 0;
    }

    ccid_state.itf_num = desc_itf->bInterfaceNumber;
    ccid_state.rhport = rhport;

    uint16_t drv_len = sizeof(tusb_desc_interface_t);
    uint8_t const* p_desc = (uint8_t const*)desc_itf + drv_len;

    while (drv_len < max_len) {
        uint8_t desc_len = p_desc[0];
        uint8_t desc_type = p_desc[1];
        if (desc_type == 0x21) {
            drv_len += desc_len;
            p_desc += desc_len;
        } else {
            break;
        }
    }

    uint8_t ep_count = 0;
    while (drv_len < max_len && ep_count < desc_itf->bNumEndpoints) {
        uint8_t desc_len = p_desc[0];
        uint8_t desc_type = p_desc[1];

        if (desc_type == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const* ep_desc = (tusb_desc_endpoint_t const*)p_desc;
            uint8_t ep_addr = ep_desc->bEndpointAddress;

            if (usbd_edpt_open(rhport, ep_desc)) {
                if (tu_edpt_dir(ep_addr) == TUSB_DIR_IN) {
                    ccid_state.ep_in = ep_addr;
                } else {
                    ccid_state.ep_out = ep_addr;
                }
                ep_count++;
            }
        }
        drv_len += desc_len;
        p_desc += desc_len;
    }

    if (ccid_state.ep_out) {
        bool ok = usbd_edpt_xfer(rhport, ccid_state.ep_out, ccid_rx_buf, sizeof(ccid_rx_buf));
        ccid_state.rx_pending = ok;
    }

    ccid_state.initialized = true;
    return drv_len;
}

static bool ccid_driver_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS &&
        request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE &&
        request->wIndex == ccid_state.itf_num) {

        // CCID 1.10 §5.3.1 — CCID_ABORT.
        // Host clears a stalled XFR_BLOCK; we just acknowledge the status
        // stage, drop any pending response remainder, and let the bulk EP
        // reset naturally on the next request.
        if (request->bRequest == 0x01) {
            ccid_state.rx_pending = false;
            ccid_state.rx_len = 0;
            return tud_control_status(rhport, request);
        }

        // CCID 1.10 §5.3.2 — GET_CLOCK_FREQUENCIES.
        // Returns the supported clock frequencies as little-endian uint32
        // values in kHz. We advertise 4000 kHz to match dwDefaultClock /
        // dwMaximumClock from the functional descriptor.
        if (request->bRequest == 0x02) {
            static const uint8_t clocks[] = {0xA0, 0x0F, 0x00, 0x00};  // 4000 kHz
            return tud_control_xfer(rhport, request,
                                    const_cast<uint8_t*>(clocks),
                                    sizeof(clocks));
        }

        // CCID 1.10 §5.3.3 — GET_DATA_RATES.
        // Returns the supported asynchronous data rates as little-endian
        // uint32 values in bits/s. We mirror dwDataRate / dwMaxDataRate
        // (1200 bps) so libccid keeps using our descriptor defaults.
        if (request->bRequest == 0x03) {
            static const uint8_t rates[] = {0xB0, 0x04, 0x00, 0x00};   // 1200 bps
            return tud_control_xfer(rhport, request,
                                    const_cast<uint8_t*>(rates),
                                    sizeof(rates));
        }
        return false;
    }

    return false;
}

static bool ccid_driver_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    if (result != XFER_RESULT_SUCCESS) {
        ccid_state.error_count++;
        return true;
    }

    if (ep_addr == ccid_state.ep_out) {
        ccid_state.rx_count++;
        ccid_state.rx_len = xferred_bytes;
        ccid_state.rx_pending = false;

        if (xferred_bytes >= CCID_HEADER_SIZE) {
            uint32_t data_len = (uint32_t)ccid_rx_buf[1] |
                                ((uint32_t)ccid_rx_buf[2] << 8) |
                                ((uint32_t)ccid_rx_buf[3] << 16) |
                                ((uint32_t)ccid_rx_buf[4] << 24);

            // Reject host-controlled lengths that would overflow the rx buffer.
            // Subtractive form avoids integer wraparound on attacker-chosen sizes.
            if (data_len > sizeof(ccid_rx_buf) - CCID_HEADER_SIZE) {
                ccid_state.error_count++;
            } else {
                uint32_t total_len = CCID_HEADER_SIZE + data_len;

                if ((uint32_t)ccid_state.rx_len >= total_len) {
                    int resp_len = ccid_process_message(ccid_rx_buf, total_len,
                                                        ccid_tx_buf, sizeof(ccid_tx_buf));
                    if (resp_len > 0) {
                        ccid_state.tx_count++;
                        bool ok = usbd_edpt_xfer(rhport, ccid_state.ep_in, ccid_tx_buf, resp_len);
                        if (!ok) {
                            ccid_state.error_count++;
                        }
                    } else if (resp_len < 0) {
                        ccid_state.error_count++;
                    }
                }
            }
        }

        bool rx_ok = usbd_edpt_xfer(rhport, ccid_state.ep_out, ccid_rx_buf, sizeof(ccid_rx_buf));
        if (!rx_ok) {
            ccid_state.error_count++;
        }
        ccid_state.rx_pending = rx_ok;
        return true;
    }

    if (ep_addr == ccid_state.ep_in) {
        return true;
    }

    return true;
}

static usbd_class_driver_t const ccid_driver = {
    .name             = "CCID",
    .init             = ccid_driver_init,
    .deinit           = NULL,
    .reset            = ccid_driver_reset,
    .open             = ccid_driver_open,
    .control_xfer_cb  = ccid_driver_control_xfer_cb,
    .xfer_cb          = ccid_driver_xfer_cb,
    .xfer_isr         = NULL,
    .sof              = NULL
};

// Symbol referenced from ccid.cpp so the linker pulls this translation
// unit into the final image. Without an external reference, the only
// strong symbol here would be usbd_app_driver_get_cb — which tinyusb
// already provides as a weak default, leading the linker to drop our
// strong override and silently disable the entire CCID class driver.
extern "C" void ccid_driver_link_anchor(void) {}

extern "C" usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count) {
    *driver_count = 1;
    return &ccid_driver;
}
