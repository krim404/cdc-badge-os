// USB Descriptors for CDC Badge
// Composite Device: CDC + optional FIDO HID + optional Keyboard HID + optional CCID

#pragma once

#include <stdint.h>
extern "C" {
#include "tusb.h"
}

// USB Device IDs
// When CCID is enabled, use Gemalto VID/PID for libccid whitelist compatibility
// (Same approach as pico-openpgp, Rugart, and other DIY OpenPGP cards)
#define USB_VID_GEMALTO      0x08E6  // Gemalto
#define USB_PID_GEMALTO      0x4433  // GemPC433 (libccid whitelisted)
#define USB_VID_ESPRESSIF    0x303A  // Espressif Systems
#define USB_PID_BADGE        0xBADE  // CDC Badge custom PID
// OnlyKey OTP IDs, whitelisted by KeePassXC for HMAC-SHA1 challenge-response
// (keepassxreboot/keepassxc commit e4326fb: yk_open_key_vid_pid(0x1d50, {0x60fc})).
// mod_otphid presents these so KeePassXC/ykinfo recognize the OTP HID interface.
#define USB_VID_ONLYKEY      0x1D50  // OpenMoko / Great Scott Gadgets (OnlyKey)
#define USB_PID_ONLYKEY      0x60FC  // OnlyKey
#define USB_BCD   0x0200

// String Descriptor Indices (fixed strings only, module strings are dynamic)
enum {
    STR_LANGID = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CDC,
    STR_MSC,          // Mass-storage interface name (when MSC active)
    STR_DYNAMIC_BASE  // Module interface names start here
};

// Endpoint Numbers
// CDC always uses: 0x81 (notif), 0x02 (out), 0x82 (in)
#define EP_CDC_NOTIF      0x81    // CDC Notification IN
#define EP_CDC_OUT        0x02    // CDC Data OUT
#define EP_CDC_IN         0x82    // CDC Data IN

// Endpoint Sizes
#define EP_CDC_NOTIF_SIZE   8
#define EP_CDC_SIZE         64
#define EP_FIDO_SIZE        64    // CTAPHID packets are 64 bytes
#define EP_KEYBOARD_SIZE    8     // Keyboard reports are 8 bytes
#define EP_CCID_SIZE        64    // CCID SmartCard packets

// HID Report ID
#define REPORT_ID_KEYBOARD  1     // Keyboard uses Report ID 1

// CCID SmartCard Descriptor (54 bytes class descriptor + endpoints)
// CCID Class Descriptor Macro (from legacy)
#define TUD_CCID_DESC_LEN  54
#define TUD_CCID_TOTAL_LEN (9 + TUD_CCID_DESC_LEN + 7 + 7)  // Interface + Class + 2x Endpoint

#define TUD_CCID_DESCRIPTOR(_itfnum, _stridx, _epout, _epin, _epsize) \
    /* Interface */ \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 2, TUSB_CLASS_SMART_CARD, 0, 0, _stridx, \
    /* CCID Class Descriptor (54 bytes) */ \
    TUD_CCID_DESC_LEN, 0x21, /* bDescriptorType: CCID Functional */ \
    0x10, 0x01,              /* bcdCCID: CCID 1.1 */ \
    0x00,                    /* bMaxSlotIndex: 1 slot */ \
    0x07,                    /* bVoltageSupport: 5V, 3V, 1.8V */ \
    0x02, 0x00, 0x00, 0x00,  /* dwProtocols: T=1 */ \
    0xA0, 0x0F, 0x00, 0x00,  /* dwDefaultClock: 4000 kHz */ \
    0xA0, 0x0F, 0x00, 0x00,  /* dwMaximumClock: 4000 kHz */ \
    0x00,                    /* bNumClockSupported */ \
    0x00, 0x2A, 0x00, 0x00,  /* dwDataRate: 10752 bps */ \
    0x00, 0x2A, 0x00, 0x00,  /* dwMaxDataRate: 10752 bps */ \
    0x00,                    /* bNumDataRatesSupported */ \
    0xFE, 0x00, 0x00, 0x00,  /* dwMaxIFSD: 254 */ \
    0x00, 0x00, 0x00, 0x00,  /* dwSynchProtocols: none */ \
    0x00, 0x00, 0x00, 0x00,  /* dwMechanical: none */ \
    0xFE, 0x00, 0x04, 0x00,  /* dwFeatures: auto config/activation/voltage/clock/baud/negotiation/PPS, extended APDU */ \
    0x0F, 0x01, 0x00, 0x00,  /* dwMaxCCIDMessageLength: 271 */ \
    0xFF,                    /* bClassGetResponse */ \
    0xFF,                    /* bClassEnvelope */ \
    0x00, 0x00,              /* wLcdLayout: none */ \
    0x00,                    /* bPINSupport: none */ \
    0x01,                    /* bMaxCCIDBusySlots: 1 */ \
    /* Bulk OUT Endpoint */ \
    7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0, \
    /* Bulk IN Endpoint */ \
    7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0
