#pragma once

#include "cdc_keyboard/KeyboardProviderBase.h"
#include <cstdint>
#include <cstddef>

namespace cdc::mod_usbhid {

/**
 * USB HID Keyboard Implementation
 *
 * USB HID (boot keyboard) transport for the shared keyboard::KeyboardProviderBase.
 * The base owns the KeyboardEngine, the IKeyboardProvider typing surface and the
 * UnicodeMethod NVS persistence; this class implements only the USB-specific
 * report delivery (IKeyReportSink::sendKeyReport via the TinyUSB HID interrupt IN
 * endpoint) and connection state. The shared keyboard::getHidReportMap() report
 * descriptor is registered with UsbManager's Keyboard interface slot.
 */
class UsbHidKeyboard : public keyboard::KeyboardProviderBase {
public:
    static UsbHidKeyboard& instance();

    /**
     * \brief Registers the USB HID keyboard interface and loads settings.
     * \return true if the Keyboard USB interface slot was acquired.
     */
    bool registerUsb();

    /**
     * \brief Unregisters the USB HID keyboard interface.
     */
    void unregisterUsb();

    /**
     * \brief Reports whether the USB HID interface slot is currently held.
     * \return true while the Keyboard interface is registered.
     */
    bool isRegistered() const { return registered_; }

    // IKeyboardProvider connection surface (transport-specific)
    bool isConnected() const override;
    const char* getStatusText() const override;

    // IKeyReportSink implementation (engine -> USB HID delivery)
    bool sendKeyReport(uint8_t modifier, const uint8_t* keycodes,
                       uint8_t numKeys) override;

private:
    UsbHidKeyboard() : keyboard::KeyboardProviderBase("mod_usbhid") {}

    /**
     * \brief Resolves the TinyUSB HID instance index of the keyboard interface.
     *
     * HID instances are numbered in active-interface order (Fido, Keyboard,
     * Ccid). The keyboard is instance 1 when the FIDO interface is also active,
     * otherwise instance 0.
     * \return HID instance index for usb_hid_send_report()/usb_hid_instance_ready().
     */
    uint8_t hidInstance() const;

    bool registered_ = false;
};

} // namespace cdc::mod_usbhid
