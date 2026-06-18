/**
 * \brief USB composite descriptor/runtime builder for CDC plus optional HID/CCID interfaces.
 */

#include "usb_badge/usb_hid.h"
#include "usb_descriptors.h"
#include "cdc_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"

extern "C" {
#include "tusb.h"
#include "class/cdc/cdc.h"
#include "class/hid/hid_device.h"
}

#include <string.h>
#include <stdio.h>

static const char* TAG = "USB_HID";

/**
 * \brief Global descriptor and interface registration state.
 */

static bool s_hid_initialized = false;

static constexpr size_t MAX_INTERFACES = 3;
static UsbInterfaceDef s_defs[MAX_INTERFACES] = {};
static size_t s_def_count = 0;

static int8_t s_hid_map[MAX_INTERFACES] = {-1, -1, -1};
static size_t s_hid_count = 0;

static uint8_t s_config_descriptor[256];
static uint16_t s_config_descriptor_len = 0;

// When true, an MSC (mass-storage) interface is appended after the HID/CCID
// interfaces. Toggled by usb_hid_set_msc(); the backing LUN lives in usb_msc.cpp.
static bool s_msc_active = false;

/**
 * \brief Dynamic module interface names generated during configuration apply.
 */
static const char* s_dynamic_strings[MAX_INTERFACES] = {};
static size_t s_dynamic_string_count = 0;

/**
 * \brief Descriptor storage used for runtime-generated TinyUSB configuration descriptors.
 */

static tusb_desc_device_t device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID_ESPRESSIF,
    .idProduct          = USB_PID_BADGE,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STR_MANUFACTURER,
    .iProduct           = STR_PRODUCT,
    .iSerialNumber      = STR_SERIAL,
    .bNumConfigurations = 1
};

/**
 * \brief Rebuilds the composite USB configuration descriptor from registered interfaces.
 */
static void build_config_descriptor(void) {
    uint8_t* p = s_config_descriptor;
    s_config_descriptor_len = 0;

    uint8_t itf_count = 2;
    uint16_t total_len = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN;
    bool has_ccid = false;
    uint16_t pref_vid = 0;
    uint16_t pref_pid = 0;

    // Reset dynamic strings and HID mapping
    s_hid_count = 0;
    s_dynamic_string_count = 0;

    for (size_t i = 0; i < s_def_count; i++) {
        const auto& def = s_defs[i];
        if (def.preferredVid != 0 && def.preferredPid != 0) {
            pref_vid = def.preferredVid;
            pref_pid = def.preferredPid;
        }
        if (def.cls == UsbInterfaceClass::Hid) {
            itf_count++;
            total_len += def.hasOut ? TUD_HID_INOUT_DESC_LEN : TUD_HID_DESC_LEN;
            s_hid_map[s_hid_count++] = static_cast<int8_t>(i);
        } else if (def.cls == UsbInterfaceClass::Ccid) {
            itf_count++;
            total_len += TUD_CCID_TOTAL_LEN;
            has_ccid = true;
        }
    }

    if (s_msc_active) {
        itf_count++;
        total_len += TUD_MSC_DESC_LEN;
    }

    // A per-interface VID/PID hint (e.g. mod_otphid's OnlyKey IDs) overrides the
    // default product identity; otherwise CCID selects Gemalto, else Espressif.
    if (pref_vid != 0 && pref_pid != 0) {
        device_descriptor.idVendor = pref_vid;
        device_descriptor.idProduct = pref_pid;
    } else {
        device_descriptor.idVendor = has_ccid ? USB_VID_GEMALTO : USB_VID_ESPRESSIF;
        device_descriptor.idProduct = has_ccid ? USB_PID_GEMALTO : USB_PID_BADGE;
    }

    uint8_t cfg_desc[] = {
        TUD_CONFIG_DESCRIPTOR(1, itf_count, 0, total_len, 0, 100),
    };
    memcpy(p, cfg_desc, sizeof(cfg_desc));
    p += sizeof(cfg_desc);

    uint8_t cdc_desc[] = {
        TUD_CDC_DESCRIPTOR(0, STR_CDC, EP_CDC_NOTIF, EP_CDC_NOTIF_SIZE,
                           EP_CDC_OUT, EP_CDC_IN, EP_CDC_SIZE),
    };
    memcpy(p, cdc_desc, sizeof(cdc_desc));
    p += sizeof(cdc_desc);

    uint8_t next_itf = 2;
    uint8_t next_out = 0x03;
    uint8_t next_in = 0x83;

    for (size_t i = 0; i < s_def_count; i++) {
        const auto& def = s_defs[i];

        // Register interface name as dynamic string
        const uint8_t str_idx = static_cast<uint8_t>(STR_DYNAMIC_BASE + s_dynamic_string_count);
        if (def.name && s_dynamic_string_count < MAX_INTERFACES) {
            s_dynamic_strings[s_dynamic_string_count++] = def.name;
        }

        if (def.cls == UsbInterfaceClass::Hid) {
            if (def.reportDesc == nullptr || def.reportDescLen == 0) {
                LOG_W(TAG, "HID interface missing report descriptor");
                continue;
            }

            const uint16_t ep_size = (def.epOutSize > def.epInSize) ? def.epOutSize : def.epInSize;
            if (def.hasOut) {
                uint8_t desc[] = {
                    TUD_HID_INOUT_DESCRIPTOR(next_itf, str_idx, def.protocol,
                                             def.reportDescLen,
                                             next_out, next_in, ep_size, 5),
                };
                memcpy(p, desc, sizeof(desc));
                p += sizeof(desc);
                next_out++;
                next_in++;
            } else {
                uint8_t desc[] = {
                    TUD_HID_DESCRIPTOR(next_itf, str_idx, def.protocol,
                                       def.reportDescLen, next_in, def.epInSize, 10),
                };
                memcpy(p, desc, sizeof(desc));
                p += sizeof(desc);
                next_in++;
            }
            next_itf++;
        } else if (def.cls == UsbInterfaceClass::Ccid) {
            uint8_t desc[] = {
                TUD_CCID_DESCRIPTOR(next_itf, str_idx, next_out, next_in, EP_CCID_SIZE),
            };
            memcpy(p, desc, sizeof(desc));
            p += sizeof(desc);
            next_out++;
            next_in++;
            next_itf++;
        }
    }

    if (s_msc_active) {
        uint8_t desc[] = {
            TUD_MSC_DESCRIPTOR(next_itf, STR_MSC, next_out, next_in, 64),
        };
        memcpy(p, desc, sizeof(desc));
        p += sizeof(desc);
        next_out++;
        next_in++;
        next_itf++;
    }

    s_config_descriptor_len = static_cast<uint16_t>(p - s_config_descriptor);
}

/**
 * \brief USB serial number derived from ESP32 MAC address.
 */
static char s_serial_number[13] = "000000000000";

/**
 * \brief Initializes the serial-number string once from efuse MAC.
 */
static void init_serial_number() {
    static bool inited = false;
    if (inited) return;
    inited = true;

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_serial_number, sizeof(s_serial_number),
                 "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

/**
 * \brief Fixed USB string descriptors; dynamic interface strings are appended at runtime.
 */
static const char* s_fixed_strings[] = {
    (const char[]){0x09, 0x04},  // 0: Language (English US)
    "CDC",                        // 1: Manufacturer
    "BadgeV1",                    // 2: Product
    s_serial_number,              // 3: Serial (derived from MAC)
    "CDC Serial",                 // 4: CDC Interface
    "vFAT Storage",               // 5: MSC Interface
};
static constexpr size_t FIXED_STRING_COUNT = sizeof(s_fixed_strings) / sizeof(s_fixed_strings[0]);

/**
 * \brief TinyUSB descriptor and HID report callbacks.
 */

extern "C" {

/**
 * \brief Returns the device descriptor used by TinyUSB.
 * \return Pointer to static device descriptor.
 */
uint8_t const* tud_descriptor_device_cb(void) {
    return (uint8_t const*)&device_descriptor;
}

/**
 * \brief Returns the active configuration descriptor.
 * \param index Configuration index (unused; single configuration only).
 * \return Pointer to descriptor buffer, or `nullptr` if not built.
 */
uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return s_config_descriptor_len ? s_config_descriptor : nullptr;
}

/**
 * \brief Returns UTF-16 string descriptors for fixed and dynamic strings.
 * \param index String descriptor index.
 * \param langid Requested language ID (unused).
 * \return Pointer to UTF-16 descriptor buffer, or `nullptr` if index is invalid.
 */
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t str_desc[32];

    // Determine which string to use
    const char* str = nullptr;

    if (index < FIXED_STRING_COUNT) {
        str = s_fixed_strings[index];
    } else if (index >= STR_DYNAMIC_BASE &&
               index < STR_DYNAMIC_BASE + s_dynamic_string_count) {
        str = s_dynamic_strings[index - STR_DYNAMIC_BASE];
    }

    if (!str) return nullptr;

    // Language descriptor (index 0)
    if (index == 0) {
        str_desc[0] = (TUSB_DESC_STRING << 8) | 4;
        str_desc[1] = 0x0409;
        return str_desc;
    }

    // Convert ASCII to UTF-16
    uint8_t len = strlen(str);
    if (len > 31) len = 31;
    str_desc[0] = (TUSB_DESC_STRING << 8) | (2 + 2 * len);
    for (uint8_t i = 0; i < len; i++) {
        str_desc[1 + i] = str[i];
    }
    return str_desc;
}

/**
 * \brief Returns the HID report descriptor for a HID instance.
 * \param instance HID instance index.
 * \return Pointer to report descriptor, or `nullptr` if instance is invalid.
 */
uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    if (instance >= s_hid_count) return nullptr;
    int8_t def_idx = s_hid_map[instance];
    if (def_idx < 0) return nullptr;
    const auto& def = s_defs[static_cast<size_t>(def_idx)];
    return def.reportDesc;
}

/**
 * \brief Handles HID GET_REPORT requests by delegating to interface callbacks.
 * \param instance HID instance index.
 * \param report_id Requested report ID.
 * \param report_type Requested report type.
 * \param buffer Destination buffer.
 * \param reqlen Requested report length.
 * \return Number of bytes written to `buffer`.
 */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t* buffer, uint16_t reqlen) {
    if (instance >= s_hid_count) return 0;
    int8_t def_idx = s_hid_map[instance];
    if (def_idx < 0) return 0;
    const auto& def = s_defs[static_cast<size_t>(def_idx)];
    if (!def.callbacks.onGetReport) return 0;
    return def.callbacks.onGetReport(report_id, static_cast<uint8_t>(report_type), buffer, reqlen);
}

/**
 * \brief Handles HID SET_REPORT requests by delegating to interface callbacks.
 * \param instance HID instance index.
 * \param report_id Report ID.
 * \param report_type Report type.
 * \param buffer Report payload.
 * \param bufsize Payload length in bytes.
 */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const* buffer, uint16_t bufsize) {
    if (instance >= s_hid_count) return;
    int8_t def_idx = s_hid_map[instance];
    if (def_idx < 0) return;
    const auto& def = s_defs[static_cast<size_t>(def_idx)];
    if (!def.callbacks.onSetReport) return;
    def.callbacks.onSetReport(report_id, static_cast<uint8_t>(report_type), buffer, bufsize);
}

/**
 * \brief Notifies interface callbacks that a HID report transfer completed.
 * \param instance HID instance index.
 * \param report Transmitted report payload.
 * \param len Report length in bytes.
 */
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len) {
    if (instance >= s_hid_count) return;
    int8_t def_idx = s_hid_map[instance];
    if (def_idx < 0) return;
    const auto& def = s_defs[static_cast<size_t>(def_idx)];
    if (!def.callbacks.onReportComplete) return;
    def.callbacks.onReportComplete(report, len);
}

} // extern "C"

/**
 * \brief Public USB HID/composite configuration API implementation.
 */

/**
 * \brief Initializes HID descriptor state and builds default descriptors.
 * \return `true` if initialization is complete.
 */
extern "C" bool usb_hid_init(void) {
    if (s_hid_initialized) return true;
    init_serial_number();
    build_config_descriptor();
    s_hid_initialized = true;
    return true;
}

/**
 * \brief Applies the runtime interface configuration and optionally triggers re-enumeration.
 * \param defs Interface definition array.
 * \param count Number of interface definitions.
 * \param needs_replug Optional out-flag indicating host re-enumeration request.
 * \return `true` on successful configuration apply.
 */
extern "C" bool usb_hid_apply_config(const UsbInterfaceDef* defs, size_t count, bool* needs_replug) {
    if (needs_replug) *needs_replug = false;
    if (!defs && count > 0) return false;
    if (count > MAX_INTERFACES) return false;

    for (size_t i = 0; i < count; i++) {
        s_defs[i] = defs[i];
    }
    s_def_count = count;

    build_config_descriptor();

    if (tud_inited()) {
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(20));
        tud_connect();
        if (needs_replug) *needs_replug = true;
    }

    return true;
}

/**
 * \brief Adds or removes the MSC interface and re-enumerates the device.
 * \param active `true` to expose the mass-storage interface, `false` to remove it.
 */
extern "C" void usb_hid_set_msc(bool active) {
    if (s_msc_active == active) return;
    s_msc_active = active;

    build_config_descriptor();

    if (tud_inited()) {
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(20));
        tud_connect();
    }
}

/**
 * \brief Returns whether TinyUSB stack is ready.
 * \return `true` when USB device stack is ready.
 */
extern "C" bool usb_hid_ready(void) {
    return tud_ready();
}

/**
 * \brief Returns whether a specific HID instance endpoint is ready.
 * \param instance HID instance index.
 * \return `true` if that instance is ready for reports.
 */
extern "C" bool usb_hid_instance_ready(uint8_t instance) {
    return tud_hid_n_ready(instance);
}

/**
 * \brief Sends one HID report on the selected interface instance.
 * \param instance HID instance index.
 * \param report_id Report ID.
 * \param data Report payload.
 * \param len Payload length in bytes.
 * \return `true` if TinyUSB accepted the report for transmission.
 */
extern "C" bool usb_hid_send_report(uint8_t instance, uint8_t report_id, const uint8_t* data, uint16_t len) {
    if (!data || len == 0) return false;
    return tud_hid_n_report(instance, report_id, data, len);
}
