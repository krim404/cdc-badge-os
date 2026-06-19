/**
 * USB CDC Serial Interface
 *
 * Provides serial console via TinyUSB CDC.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize USB subsystem with CDC serial
 * Behavior depends on CONFIG_USB_EARLY_DEBUG:
 * - If enabled: Starts USB immediately (for debug logs during boot)
 * - If disabled: Only prepares USB, call usb_cdc_start() after modules loaded
 * @return true if successful
 */
bool usb_cdc_init(void);

/**
 * Start USB stack (only needed when CONFIG_USB_EARLY_DEBUG is disabled)
 * Call this after all modules have registered their USB interfaces.
 * @return true if successful
 */
bool usb_cdc_start(void);

/**
 * Force a USB re-enumeration (disconnect then reconnect the bus pull-up).
 * Use to make the host re-attach after the device was unresponsive, e.g. when
 * USB is plugged in while the badge is in light sleep. No-op if USB is not
 * running.
 */
void usb_cdc_reenumerate(void);

/**
 * Check if USB CDC is ready for I/O
 * @return true if connected and ready
 */
bool usb_cdc_ready(void);

/**
 * Write data to USB CDC
 * @param data Data to write
 * @param len Data length
 * @return Number of bytes written
 */
size_t usb_cdc_write(const uint8_t* data, size_t len);

/**
 * Write string to USB CDC
 * @param str Null-terminated string
 * @return Number of bytes written
 */
size_t usb_cdc_print(const char* str);

/**
 * Read data from USB CDC (non-blocking)
 * @param data Buffer for received data
 * @param len Maximum bytes to read
 * @return Number of bytes read (0 if no data)
 */
size_t usb_cdc_read(uint8_t* data, size_t len);

/**
 * Get single character from USB CDC (non-blocking)
 * @return Character or -1 if no data
 */
int usb_cdc_getchar(void);

/**
 * Check if data is available to read
 * @return Number of bytes available
 */
size_t usb_cdc_available(void);

/**
 * Flush TX buffer (blocking)
 */
void usb_cdc_flush(void);

#ifdef __cplusplus
}
#endif
