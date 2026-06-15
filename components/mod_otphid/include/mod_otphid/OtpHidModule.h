#pragma once

#include "cdc_core/IModule.h"

namespace cdc::mod_otphid {

/**
 * \brief USB OTP HID (YubiKey/OnlyKey emulation) module.
 *
 * Default-disabled, transport-only module (no TROPIC01 slots of its own). On
 * start() it acquires the UsbManager Keyboard interface slot with the Yubico-OTP
 * HID report descriptor and presents OnlyKey's KeePassXC-whitelisted VID/PID, so
 * the host recognizes the badge as a slot-configured OTP token. Competes for the
 * single Keyboard HID slot with mod_usbhid and, via the USB HID budget
 * (MAX_ACTIVE_HID), is exclusive with CCID/GPG; start() fails cleanly when the
 * slot is taken. Enumerates and answers the status feature report, performs the
 * VID/PID swap, and runs the HMAC-SHA1 challenge-response frame protocol.
 */
class OtpHidModule : public core::IModule {
public:
    const char* getName() const override { return "mod_otphid"; }
    core::ServiceState getState() const override { return state_; }
    bool init() override;
    bool start() override;
    void stop() override;
    void onTick(uint32_t nowMs) override;

    const char* getVersion() const override { return "1.0"; }
    uint8_t getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) override;

    static OtpHidModule& instance();

private:
    OtpHidModule() = default;
    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;
};

} // namespace cdc::mod_otphid

extern "C" void mod_otphid_register();
