#pragma once

#include "cdc_ui/IView.h"
#include <cstdint>

namespace cdc::ui {

/**
 * PinEntryView - Secure PIN code input
 *
 * Displays PIN entry with masked digits.
 * Supports configurable length and max attempts.
 * Errors are shown via MessageBox overlay.
 *
 * Keys:
 *   0-9 = Add digit
 *   N = Backspace (short) / Cancel (empty)
 *   Y = Confirm and verify PIN
 */
class PinEntryView : public ViewBase {
public:
    static constexpr uint8_t MAX_PIN_LENGTH = 8;

    /**
     * PIN complete callback
     * @param pin The entered PIN
     * @return true if PIN is correct, false otherwise
     */
    using VerifyCallback = bool(*)(const char* pin);

    /**
     * PIN success callback
     */
    using SuccessCallback = void(*)();

    /**
     * Cancel callback (called when N is pressed with empty PIN)
     */
    using CancelCallback = void(*)();

    /**
     * Failure callback (called after invalid PIN)
     * @param lockedOut True if device is now locked out
     */
    using FailureCallback = void(*)(bool lockedOut);

    /**
     * Initialize PIN entry view
     * @param title Title text
     * @param maxPinLength Maximum PIN length (default 6)
     * @param maxAttempts Maximum attempts before lockout (0 = unlimited)
     */
    void init(const char* title, uint8_t maxPinLength = 6, uint8_t maxAttempts = 3);

    /**
     * Set verify callback (called when Y is pressed)
     */
    void setOnVerify(VerifyCallback callback) { onVerify_ = callback; }

    /**
     * Set success callback (called after successful verify)
     */
    void setOnSuccess(SuccessCallback callback) { onSuccess_ = callback; }

    /**
     * Set cancel callback (optional)
     */
    void setOnCancel(CancelCallback callback) { onCancel_ = callback; }

    /**
     * Set failure callback (optional)
     */
    void setOnFailure(FailureCallback callback) { onFailure_ = callback; }

    /**
     * Suppress internal error messages (caller handles feedback)
     */
    void setShowMessages(bool enabled) { showMessages_ = enabled; }

    /**
     * Get current PIN
     */
    const char* getPin() const { return buffer_; }

    /**
     * Get current attempt count
     */
    uint8_t getAttempts() const { return attempts_; }

    /**
     * Check if locked out
     */
    bool isLockedOut() const { return lockedOut_; }

    /**
     * Clear PIN and reset state
     */
    void clear();

    /**
     * Reset attempt counter
     */
    void resetAttempts() { attempts_ = 0; lockedOut_ = false; }

    /**
     * Set minimum PIN length required (default 1)
     * Y will only work if length >= minLength
     */
    void setMinLength(uint8_t minLen) { minLength_ = minLen; }

    /**
     * Get remaining lockout time in seconds (0 if not locked)
     */
    uint32_t getLockoutRemaining() const;

    // IView implementation
    void onEnter(void* context) override;
    void onTick(uint32_t nowMs) override;
    void render(bool partial) override;
    InputResult onKey(char key) override;
    const char* getName() const override { return "PinEntryView"; }
    const char* getFooterHint() const override;

private:
    const char* title_ = nullptr;
    char buffer_[MAX_PIN_LENGTH + 1] = {};
    uint8_t length_ = 0;
    uint8_t maxLength_ = 6;
    uint8_t minLength_ = 4;
    uint8_t attempts_ = 0;
    uint8_t maxAttempts_ = 3;
    bool lockedOut_ = false;
    uint32_t lockoutStartMs_ = 0;

    VerifyCallback onVerify_ = nullptr;
    SuccessCallback onSuccess_ = nullptr;
    CancelCallback onCancel_ = nullptr;
    FailureCallback onFailure_ = nullptr;
    bool showMessages_ = true;

    void addDigit(char digit);
    void backspace();
    void verify();
};

// ============================================================================
// Convenience Functions
// ============================================================================

/**
 * Show a PIN entry view and push it to the ViewStack.
 * Simplest possible API for PIN input.
 *
 * @param title Title text
 * @param onVerify Verification callback (return true if PIN correct)
 * @param onSuccess Called after successful verification
 * @param maxLength Maximum PIN length (default 6)
 * @param minLength Minimum PIN length (default 4)
 * @param maxAttempts Maximum attempts (0 = unlimited, default 3)
 * @return Pointer to the PinEntryView
 *
 * Example (default PIN is "1234"):
 *   showPinEntry("Enter PIN",
 *                [](const char* pin) { return strcmp(pin, "1234") == 0; },
 *                []() { unlockDevice(); });
 */
PinEntryView* showPinEntry(const char* title,
                           PinEntryView::VerifyCallback onVerify,
                           PinEntryView::SuccessCallback onSuccess,
                           uint8_t maxLength = 6,
                           uint8_t minLength = 4,
                           uint8_t maxAttempts = 3);

} // namespace cdc::ui
