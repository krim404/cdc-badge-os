/**
 * TinyUSB Configuration
 *
 * Minimal configuration for CDC Serial only.
 */

#pragma once

#include "tusb_option.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Kconfig defaults (if not defined via sdkconfig)
// ============================================================================

#ifndef CONFIG_TINYUSB_CDC_ENABLED
#define CONFIG_TINYUSB_CDC_ENABLED 0
#endif

#ifndef CONFIG_TINYUSB_MSC_ENABLED
#define CONFIG_TINYUSB_MSC_ENABLED 0
#endif

#ifndef CONFIG_TINYUSB_HID_ENABLED
#define CONFIG_TINYUSB_HID_ENABLED 0
#endif

#ifndef CONFIG_TINYUSB_MIDI_ENABLED
#define CONFIG_TINYUSB_MIDI_ENABLED 0
#endif

#ifndef CONFIG_TINYUSB_CUSTOM_CLASS_ENABLED
#define CONFIG_TINYUSB_CUSTOM_CLASS_ENABLED 0
#endif

#ifndef CONFIG_TINYUSB_CDC_RX_BUFSIZE
#define CONFIG_TINYUSB_CDC_RX_BUFSIZE 64
#endif

#ifndef CONFIG_TINYUSB_CDC_TX_BUFSIZE
#define CONFIG_TINYUSB_CDC_TX_BUFSIZE 64
#endif

#ifndef CONFIG_TINYUSB_MSC_BUFSIZE
#define CONFIG_TINYUSB_MSC_BUFSIZE 512
#endif

#ifndef CONFIG_TINYUSB_HID_BUFSIZE
#define CONFIG_TINYUSB_HID_BUFSIZE 64
#endif

// ============================================================================
// TinyUSB Core Configuration
// ============================================================================

#define CFG_TUSB_RHPORT0_MODE       OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED
#define CFG_TUSB_OS                 OPT_OS_FREERTOS

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          TU_ATTR_ALIGNED(4)
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE      64
#endif

// Maximum number of endpoints (excluding EP0)
// CDC needs 3 (notif, out, in), HID needs 2, CCID needs 2
#ifndef CFG_TUD_ENDPOINT_MAX
#define CFG_TUD_ENDPOINT_MAX        8
#endif

// ============================================================================
// Buffer Sizes
// ============================================================================

#define CFG_TUD_CDC_RX_BUFSIZE      CONFIG_TINYUSB_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE      CONFIG_TINYUSB_CDC_TX_BUFSIZE
#define CFG_TUD_MSC_BUFSIZE         CONFIG_TINYUSB_MSC_BUFSIZE
#define CFG_TUD_MSC_EP_BUFSIZE      4096   // one vfat WL sector per read10/write10 (matches FATFS sector size)
#define CFG_TUD_HID_BUFSIZE         CONFIG_TINYUSB_HID_BUFSIZE

// ============================================================================
// Enabled Device Class Drivers
// ============================================================================

#define CFG_TUD_CDC                 1   // CDC enabled for serial console
#define CFG_TUD_MSC                 1   // MSC: optional vfat mass-storage (mod_msc), dormant until a LUN is registered
#define CFG_TUD_HID                 2   // HID instances; must cover MAX_ACTIVE_HID concurrent HID-class interfaces (e.g. FIDO2 + keyboard)
#define CFG_TUD_MIDI                0   // MIDI not used
#define CFG_TUD_VENDOR              0   // Vendor not used
#define CFG_TUD_CUSTOM_CLASS        1   // Custom class used for CCID

#ifdef __cplusplus
}
#endif
