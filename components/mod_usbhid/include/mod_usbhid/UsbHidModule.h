#pragma once

#include "cdc_core/IModule.h"
#include "cdc_core/IKeyboardProvider.h"

namespace cdc::mod_usbhid {

/**
 * USB HID Keyboard Module
 *
 * Default-disabled module that types over the USB cable. On start() it acquires
 * the UsbManager Keyboard interface slot and registers itself as the
 * core::IKeyboardProvider, taking over auto-type (TOTP/password) from the BLE
 * provider for as long as it runs; on stop() it restores the previously
 * registered provider. Competes for the single Keyboard HID slot with mod_otphid
 * and, via the USB HID budget (MAX_ACTIVE_HID), is exclusive with CCID/GPG.
 */
class UsbHidModule : public core::IModule {
public:
    const char* getName() const override { return "mod_usbhid"; }
    core::ServiceState getState() const override { return state_; }
    bool init() override;
    bool start() override;
    void stop() override;

    const char* getVersion() const override { return "1.0"; }
    bool isDefaultEnabled() const override { return false; }
    uint8_t getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) override;

    static UsbHidModule& instance();

private:
    UsbHidModule() = default;
    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;

    // Keyboard provider registered before this module took over; restored on stop.
    core::IKeyboardProvider* prevKeyboard_ = nullptr;
};

} // namespace cdc::mod_usbhid

extern "C" void mod_usbhid_register();
