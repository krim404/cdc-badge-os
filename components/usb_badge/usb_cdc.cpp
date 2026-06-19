/**
 * USB CDC Serial Implementation
 *
 * Uses TinyUSB for USB CDC serial console.
 * Based on cdc-badge-os-legacy/components/usb_badge/usb_hid.cpp
 */

#include "usb_badge/usb_cdc.h"
#include "usb_badge/usb_hid.h"
#include "cdc_core/feature_flags.h"

#include "cdc_log.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_rom_sys.h"
#include "esp_system.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0) && defined(CONFIG_SOC_USB_OTG_SUPPORTED) && CONFIG_SOC_USB_OTG_SUPPORTED
#include "esp_private/usb_phy.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "tusb.h"
#include "class/cdc/cdc.h"
}

#include <cstring>

static const char* TAG = "USB";

/**
 * \brief Internal USB CDC startup and task state.
 */

static bool g_usb_prepared = false;    // PHY and descriptors ready
static bool g_usb_started = false;     // TinyUSB running
static TaskHandle_t g_usb_task = nullptr;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0) && defined(CONFIG_SOC_USB_OTG_SUPPORTED) && CONFIG_SOC_USB_OTG_SUPPORTED
static usb_phy_handle_t g_usb_phy = nullptr;

/**
 * \brief Initializes USB PHY once on supported platforms.
 * \return `true` if PHY is ready.
 */
static bool usb_phy_init_once(void) {
    if (g_usb_phy) return true;

    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
        .ext_io_conf = nullptr,
        .otg_io_conf = nullptr,
    };

    esp_err_t err = usb_new_phy(&phy_conf, &g_usb_phy);
    if (err != ESP_OK) {
        LOG_E(TAG, "USB PHY init failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
#endif

/**
 * \brief TinyUSB device task.
 */

/**
 * \brief TinyUSB device task loop.
 * \param arg Task parameter (unused).
 */
static void usb_device_task(void* arg) {
    (void)arg;
    while (1) {
        tud_task();
        vTaskDelay(1);
    }
}

/**
 * \brief Public USB CDC API implementation.
 */

/**
 * \brief Starts the TinyUSB stack and creates the USB device task.
 * \return `true` if USB stack startup succeeded, otherwise `false`.
 */
static void usb_shutdown_handler(void) {
    if (tud_inited()) {
        tud_disconnect();
        esp_rom_delay_us(50000);
    }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0) && defined(CONFIG_SOC_USB_OTG_SUPPORTED) && CONFIG_SOC_USB_OTG_SUPPORTED
    if (g_usb_phy) {
        usb_del_phy(g_usb_phy);
        g_usb_phy = nullptr;
    }
#endif
    esp_rom_delay_us(50000);
}

static bool usb_start_stack(void) {
    if (g_usb_started) return true;

    if (!tusb_init()) {
        LOG_E(TAG, "TinyUSB init failed");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    // Stack sized for worst case mbedTLS ECP P-256 keypair generation
    // and ECDH point multiplication invoked from CCID APDU handlers.
    xTaskCreate(usb_device_task, "usbd", 8192, nullptr,
                configMAX_PRIORITIES - 1, &g_usb_task);

    esp_register_shutdown_handler(usb_shutdown_handler);

    g_usb_started = true;
    return true;
}

bool usb_cdc_init(void) {
    if (g_usb_prepared) return true;

    // Prepare USB descriptors and PHY
    if (!usb_hid_init()) {
        LOG_E(TAG, "USB HID init failed");
        return false;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0) && defined(CONFIG_SOC_USB_OTG_SUPPORTED) && CONFIG_SOC_USB_OTG_SUPPORTED
    if (!usb_phy_init_once()) {
        LOG_E(TAG, "USB PHY init failed");
        return false;
    }
#endif

    g_usb_prepared = true;

#ifdef CONFIG_USB_EARLY_DEBUG
    // Early debug mode: Start USB immediately
    LOG_I(TAG, "USB CDC init (early debug mode)");
    if (!usb_start_stack()) {
        return false;
    }
    LOG_I(TAG, "USB CDC ready (will re-enumerate after modules load)");
#else
    // Production mode: Defer USB start until usb_cdc_start() is called
    // Note: No logging here as USB is not yet available!
#endif

    return true;
}

/**
 * \brief Starts USB CDC runtime (or triggers re-enumeration in early-debug mode).
 * \return `true` if USB is running after call.
 */
bool usb_cdc_start(void) {
    if (!g_usb_prepared) {
        // Not initialized yet
        return false;
    }

    if (g_usb_started) {
#ifdef CONFIG_USB_EARLY_DEBUG
        // In early debug mode, we need to re-enumerate after modules loaded
        LOG_I(TAG, "USB re-enumerating with final configuration...");
        usb_cdc_reenumerate();
#endif
        return true;
    }

    // Start USB stack for the first time (production mode)
    if (!usb_start_stack()) {
        return false;
    }

    LOG_I(TAG, "USB CDC started");
    return true;
}

/**
 * \brief Forces a USB re-enumeration by toggling the bus connection.
 */
void usb_cdc_reenumerate(void) {
    if (!g_usb_started || !tud_inited()) return;
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    tud_connect();
}

/**
 * \brief Returns whether USB CDC is connected and ready.
 * \return `true` if host CDC connection is active.
 */
bool usb_cdc_ready(void) {
    return g_usb_started && tud_cdc_connected();
}

/**
 * \brief Writes byte buffer to USB CDC endpoint.
 * \param data Data buffer.
 * \param len Number of bytes to write.
 * \return Number of bytes written.
 */
size_t usb_cdc_write(const uint8_t* data, size_t len) {
    if (!g_usb_started || !data || len == 0) return 0;

    size_t written = 0;
    while (written < len) {
        size_t avail = tud_cdc_write_available();
        if (avail == 0) {
            tud_cdc_write_flush();
            vTaskDelay(1);
            continue;
        }

        size_t chunk = (len - written > avail) ? avail : (len - written);
        size_t sent = tud_cdc_write(data + written, chunk);
        written += sent;

        if (sent == 0) break;
    }

    tud_cdc_write_flush();
    return written;
}

/**
 * \brief Writes null-terminated string to USB CDC.
 * \param str String to write.
 * \return Number of bytes written.
 */
size_t usb_cdc_print(const char* str) {
    if (!str) return 0;
    return usb_cdc_write((const uint8_t*)str, strlen(str));
}

/**
 * \brief Reads bytes from USB CDC endpoint.
 * \param data Output buffer.
 * \param len Maximum bytes to read.
 * \return Number of bytes read.
 */
size_t usb_cdc_read(uint8_t* data, size_t len) {
    if (!g_usb_started || !data || len == 0) return 0;
    return tud_cdc_read(data, len);
}

/**
 * \brief Reads one character from USB CDC stream.
 * \return Character value or `-1` if unavailable.
 */
int usb_cdc_getchar(void) {
    if (!g_usb_started) return -1;

    uint8_t c;
    if (tud_cdc_read(&c, 1) == 1) {
        return c;
    }
    return -1;
}

/**
 * \brief Returns number of bytes available for read.
 * \return Pending byte count.
 */
size_t usb_cdc_available(void) {
    if (!g_usb_started) return 0;
    return tud_cdc_available();
}

/**
 * \brief Flushes pending USB CDC writes.
 */
void usb_cdc_flush(void) {
    if (!g_usb_started) return;
    tud_cdc_write_flush();
    vTaskDelay(pdMS_TO_TICKS(10));
}
