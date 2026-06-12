#pragma once

#include "cdc_keyboard/KeyboardProviderBase.h"
#include <cstdint>
#include <cstddef>

namespace cdc::mod_blehid {

/**
 * BLE HID Keyboard Implementation
 *
 * BLE HID (HOGP) transport for the shared keyboard::KeyboardProviderBase.
 * The base owns the KeyboardEngine, the IKeyboardProvider typing surface and
 * the UnicodeMethod NVS persistence; this class implements only the BLE-specific
 * report delivery (IKeyReportSink::sendKeyReport via GATT notifications),
 * connection state and advertising, using the IBluetoothController API.
 */
class BleHidKeyboard : public keyboard::KeyboardProviderBase {
public:
    static BleHidKeyboard& instance();

    // Lifecycle
    bool init();
    void deinit();

    // IKeyboardProvider connection surface (transport-specific)
    bool isConnected() const override;
    const char* getStatusText() const override;
    bool setDiscoverable(bool on) override;

    // IKeyReportSink implementation (engine -> BLE delivery)
    bool sendKeyReport(uint8_t modifier, const uint8_t* keycodes,
                       uint8_t numKeys) override;

    // BLE HID control
    bool startAdvertising();
    void stopAdvertising();
    bool isAdvertising() const;

    // Connection callbacks (called from BluetoothController)
    void onConnect(uint16_t connHandle);
    void onDisconnect(uint16_t connHandle, int reason);
    void onHostSuspend();
    void onHostResume();

private:
    BleHidKeyboard() : keyboard::KeyboardProviderBase("mod_blehid") {}

    bool initialized_ = false;
    bool advertising_ = false;
    char statusText_[48] = "Disconnected";
};

} // namespace cdc::mod_blehid
