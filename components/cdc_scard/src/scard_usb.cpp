#include "cdc_scard/scard_usb.h"
#include "cdc_scard/ccid.h"
#include "cdc_core/UsbManager.h"
#include "cdc_log.h"

static const char* TAG = "scard_usb";

// Canonical owner name for the shared CCID interface. UsbManager treats
// register/unregister as idempotent per owner string, so all applet modules
// funnel through this single identity.
static const char* kOwnerName = "cdc_scard";

static uint8_t s_refcount = 0;

// Service gate managed by the USB service manager; false keeps the CCID
// interface out of the descriptor while applet modules stay registered.
static bool s_enabled = true;

// Whether the CCID interface is currently part of the USB descriptor.
static bool s_registered = false;

static bool register_interface(void) {
    using namespace cdc::core;

    UsbInterfaceSpec spec = {};
    spec.cls = UsbInterfaceClass::Ccid;
    spec.name = "CDC SmartCard";
    spec.epInSize = 64;
    spec.epOutSize = 64;
    if (!UsbManager::instance().registerInterface(UsbHidInterface::Ccid, kOwnerName, spec)) {
        LOG_W(TAG, "CCID interface registration failed (USB unavailable?)");
        return false;
    }
    s_registered = true;
    return true;
}

static void unregister_interface(void) {
    if (!s_registered) return;
    cdc::core::UsbManager::instance().unregisterInterface(
        cdc::core::UsbHidInterface::Ccid, kOwnerName);
    s_registered = false;
}

bool scard_usb_acquire(void) {
    if (s_refcount > 0) {
        s_refcount++;
        return true;
    }

    // ccid_init() is the only external reference into ccid.cpp /
    // ccid_driver.cpp. Without it the linker drops the entire CCID
    // translation unit (including the strong usbd_app_driver_get_cb
    // override), leaving tinyusb's weak default in place and the
    // smart-card interface unenumerated.
    if (!ccid_init()) {
        LOG_W(TAG, "CCID init failed");
        return false;
    }

    if (s_enabled && !register_interface()) {
        return false;
    }

    s_refcount = 1;
    return true;
}

void scard_usb_release(void) {
    if (s_refcount == 0) return;
    if (--s_refcount == 0) {
        unregister_interface();
    }
}

bool scard_usb_set_enabled(bool on) {
    if (s_enabled == on) return true;
    if (on) {
        // References are already held: the interface must enter the
        // descriptor now or the enable fails (endpoint budget).
        if (s_refcount > 0 && !register_interface()) {
            return false;
        }
        s_enabled = true;
    } else {
        s_enabled = false;
        unregister_interface();
    }
    return true;
}

bool scard_usb_enabled(void) {
    return s_enabled;
}

bool scard_usb_in_use(void) {
    return s_refcount > 0;
}
