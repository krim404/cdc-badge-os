#pragma once

#include "cdc_core/IModule.h"

namespace cdc::mod_blehid {

/**
 * BLE HID Keyboard Module
 *
 * Provides Bluetooth HID keyboard functionality for auto-type features.
 * Registers as IKeyboardProvider service for use by other modules (TOTP, Password).
 */
class BleHidModule : public core::IModule {
public:
    const char* getName() const override { return "mod_blehid"; }
    core::ServiceState getState() const override { return state_; }
    bool init() override;
    bool start() override;
    void stop() override;

    const char* getVersion() const override { return "1.0"; }
    uint8_t getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) override;

    static BleHidModule& instance();

private:
    BleHidModule() = default;
    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;
};

} // namespace cdc::mod_blehid

extern "C" void mod_blehid_register();
