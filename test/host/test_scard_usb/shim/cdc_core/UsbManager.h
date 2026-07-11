// Host-test shim for cdc_core/UsbManager.h: records register/unregister
// calls so the scard_usb refcount logic and the UsbServiceManager toggle
// logic can be tested without the real USB stack.
#pragma once
#include <cstdint>

#include "usb_badge/usb_endpoint_budget.h"

namespace cdc::core {

enum class UsbHidInterface : uint8_t {
    Fido = 0,
    Keyboard = 1,
    Ccid = 2,
};

enum class UsbInterfaceClass : uint8_t {
    Hid = 0,
    Ccid = 1,
};

struct UsbHidCallbacks {
    uint16_t (*onGetReport)(uint8_t, uint8_t, uint8_t*, uint16_t) = nullptr;
    void (*onSetReport)(uint8_t, uint8_t, uint8_t const*, uint16_t) = nullptr;
    void (*onReportComplete)(uint8_t const*, uint16_t) = nullptr;
};

struct UsbInterfaceSpec {
    UsbInterfaceClass cls = UsbInterfaceClass::Hid;
    const char* name = nullptr;
    const uint8_t* reportDesc = nullptr;
    uint16_t reportDescLen = 0;
    uint8_t protocol = 0;
    bool hasOut = false;
    uint16_t epInSize = 64;
    uint16_t epOutSize = 64;
    UsbHidCallbacks callbacks = {};
};

class UsbManager {
public:
    static UsbManager& instance() {
        static UsbManager mgr;
        return mgr;
    }

    bool registerInterface(UsbHidInterface, const char*, const UsbInterfaceSpec&) {
        registerCalls++;
        return registerResult;
    }

    void unregisterInterface(UsbHidInterface, const char*, bool = true) {
        unregisterCalls++;
    }

    bool setCdcEnabled(bool on) {
        setCdcCalls++;
        if (!setCdcResult) return false;
        cdcEnabled_ = on;
        return true;
    }

    bool cdcEnabled() const { return cdcEnabled_; }

    bool isInterfaceSuspended(UsbHidInterface) const { return suspendedResult; }

    usb_ep_usage_t endpointUsage() const { return usage; }

    // Test knobs / counters
    int registerCalls = 0;
    int unregisterCalls = 0;
    int setCdcCalls = 0;
    bool registerResult = true;
    bool setCdcResult = true;
    bool suspendedResult = false;
    bool cdcEnabled_ = true;
    usb_ep_usage_t usage = {0, 0};

    void reset() {
        registerCalls = 0;
        unregisterCalls = 0;
        setCdcCalls = 0;
        registerResult = true;
        setCdcResult = true;
        suspendedResult = false;
        cdcEnabled_ = true;
        usage = {0, 0};
    }
};

} // namespace cdc::core
