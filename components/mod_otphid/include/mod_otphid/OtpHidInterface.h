#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::mod_otphid {

/**
 * \brief Yubico-OTP HID transport on the UsbManager Keyboard slot.
 *
 * Registers a vendor-defined OTP HID interface (its own report descriptor, not a
 * typing keyboard) that presents OnlyKey's KeePassXC-whitelisted VID/PID. Host
 * tools (ykinfo / KeePassXC) probe the device by issuing a HID GET_FEATURE
 * request; this class answers it with a valid YubiKey status structure so the
 * badge is recognized as a present, slot-2-configured OTP token.
 *
 * The HMAC-SHA1 challenge-response frame protocol (SET_FEATURE writes, CRC16,
 * delegation to IChallengeResponder) lives in \ref OtpHidCr; this class routes
 * the GET/SET feature-report callbacks to it.
 */
class OtpHidInterface {
public:
    static OtpHidInterface& instance();

    /**
     * \brief Registers the OTP HID interface on the UsbManager Keyboard slot.
     * \return true if the Keyboard USB interface slot was acquired.
     */
    bool registerUsb();

    /**
     * \brief Unregisters the OTP HID interface, releasing the Keyboard slot.
     */
    void unregisterUsb();

    /**
     * \brief Reports whether the OTP HID interface slot is currently held.
     * \return true while the interface is registered.
     */
    bool isRegistered() const { return registered_; }

    /**
     * \brief Reports whether the host has configured the OTP HID endpoint.
     * \return true once the USB stack and this interface are ready.
     */
    bool isConnected() const;

private:
    OtpHidInterface() = default;

    bool registered_ = false;
};

} // namespace cdc::mod_otphid
