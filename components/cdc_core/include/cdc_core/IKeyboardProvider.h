#pragma once

#include <cstdint>

namespace cdc::core {

/**
 * Keyboard Provider Interface
 *
 * Allows modules (TOTP, Password, etc.) to type text without knowing
 * the implementation details (BLE HID, USB HID, etc.).
 *
 * Usage pattern:
 *   // Consumer (e.g., TOTP module):
 *   auto* kb = ServiceRegistry::instance().request<IKeyboardProvider>(ServiceType::KEYBOARD);
 *   if (kb && kb->isConnected()) {
 *       kb->typeString(totpCode);
 *   }
 *
 *   // Provider (e.g. a USB or BLE HID module):
 *   ServiceRegistry::instance().provide<IKeyboardProvider>(ServiceType::KEYBOARD, this);
 */
class IKeyboardProvider {
public:
    virtual ~IKeyboardProvider() = default;

    /**
     * Check if keyboard is connected and ready to type
     * \return true if connected and ready
     */
    virtual bool isConnected() const = 0;

    /**
     * Type a string (UTF-8 encoded)
     * \param text Text to type (null-terminated)
     * \param delayMs Delay between keystrokes in milliseconds (default 50ms)
     * \return true if typing started successfully
     */
    virtual bool typeString(const char* text, uint16_t delayMs = 50) = 0;

    /**
     * Type a single character
     * \param c Character to type
     * \return true if successful
     */
    virtual bool typeChar(char c) = 0;

    /**
     * Check if typing operation is in progress
     * \return true if busy typing
     */
    virtual bool isBusy() const = 0;

    /**
     * Cancel current typing operation
     */
    virtual void cancel() = 0;

    /**
     * Get human-readable connection status for UI
     * \return Status string (e.g., "Connected to MacBook Pro")
     */
    virtual const char* getStatusText() const { return isConnected() ? "Connected" : "Disconnected"; }

    /**
     * Enter or leave a discoverable/pairable state so a host can bond.
     * For BLE providers this enables the stack and starts advertising the
     * input-device profile; for USB providers it is a no-op.
     * \param on true to become discoverable, false to stop
     * \return true if the requested state was applied
     */
    virtual bool setDiscoverable(bool on) { (void)on; return false; }
};

/**
 * Convenience function to get keyboard provider
 * \return Pointer to keyboard provider or nullptr if none registered
 */
IKeyboardProvider* getKeyboard();

} // namespace cdc::core
