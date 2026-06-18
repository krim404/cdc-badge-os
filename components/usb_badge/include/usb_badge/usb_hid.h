// USB HID Module - Composite Device
// CDC + optional HID/CCID interfaces registered by modules
//
// Interface descriptor types (UsbInterfaceClass, UsbHidCallbacks, UsbInterfaceDef)
// are defined once in cdc_core/UsbManager.h and re-exported here as global aliases
// for the legacy C-style apply API.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include "cdc_core/UsbManager.h"

// Re-export the canonical types from cdc::core into the global namespace so that
// the runtime apply API can keep using unqualified type names without enforcing
// the namespace on existing call sites.
using UsbInterfaceClass = cdc::core::UsbInterfaceClass;
using UsbHidCallbacks = cdc::core::UsbHidCallbacks;
using UsbInterfaceDef = cdc::core::UsbInterfaceSpec;

extern "C" {
#endif

// Initialize HID-related resources (if any).
// TinyUSB init is performed by usb_cdc_init().
bool usb_hid_init(void);

// Apply active interface list (ordered). Attempts soft reconnect; sets needs_replug
// if host may require replug.
bool usb_hid_apply_config(const UsbInterfaceDef* defs, size_t count, bool* needs_replug);

// Add (true) or remove (false) the MSC mass-storage interface and re-enumerate.
void usb_hid_set_msc(bool active);

// Check if USB is ready (CDC or HID).
bool usb_hid_ready(void);

// Per-instance HID helpers (instance is 0..n-1 in registration order)
bool usb_hid_instance_ready(uint8_t instance);
bool usb_hid_send_report(uint8_t instance, uint8_t report_id, const uint8_t* data, uint16_t len);

#ifdef __cplusplus
}
#endif
