#pragma once

#include "cdc_core/IService.h"
#include <cstdint>

namespace cdc::hal {

/**
 * Key codes for the 12-button keypad
 * Physical layout:
 *   [1] [2] [3]
 *   [4] [5] [6]
 *   [7] [8] [9]
 *   [N] [0] [Y]   (N=Cancel, Y=OK)
 */
enum class Key : char {
    KEY_1 = '1', KEY_2 = '2', KEY_3 = '3',
    KEY_4 = '4', KEY_5 = '5', KEY_6 = '6',
    KEY_7 = '7', KEY_8 = '8', KEY_9 = '9',
    KEY_NO = 'N', KEY_0 = '0', KEY_YES = 'Y',
    KEY_NONE = 0
};

/**
 * Keypad event callback
 */
using KeyCallback = void(*)(Key key, bool pressed);

/**
 * Keypad interface for button input
 */
class IKeypad : public core::IService {
public:
    virtual ~IKeypad() = default;

    /**
     * Poll for key state changes
     * Should be called periodically from main loop
     */
    virtual void poll() = 0;

    /**
     * Check if a specific key is currently pressed
     */
    virtual bool isKeyPressed(Key key) const = 0;

    /**
     * Get next key from buffer (consumes the key)
     * @return Key code or KEY_NONE if buffer empty
     */
    virtual Key getNextKey() = 0;

    /**
     * Check if there are keys in the buffer
     */
    virtual bool hasKey() const = 0;

    /**
     * Check if any key is currently pressed
     */
    virtual bool anyKeyDown() const = 0;

    /**
     * Set callback for key events
     * @param callback Function to call on key press/release
     */
    virtual void setCallback(KeyCallback callback) = 0;

    /**
     * Enable/disable long-press detection
     * @param enabled Enable long-press
     * @param thresholdMs Time in ms to trigger long-press
     */
    virtual void setLongPressEnabled(bool enabled, uint32_t thresholdMs = 800) = 0;

    /**
     * Set callback for long-press events
     */
    using LongPressCallback = void(*)(Key key);
    virtual void setLongPressCallback(LongPressCallback callback) = 0;

    /**
     * Set callback for the reserved rescue chord (N + Y held together).
     * Fired once per chord hold; used for the anti-block instant lock.
     */
    using PanicChordCallback = void(*)();
    virtual void setPanicChordCallback(PanicChordCallback callback) = 0;

    /** Independent requesters of deferred short-press mode (OR-combined). */
    static constexpr uint32_t DEFER_SRC_VIEW  = 1u << 0;  ///< active view (e.g. canvas long-press)
    static constexpr uint32_t DEFER_SRC_EVENT = 1u << 1;  ///< plugin KEY_LONG_PRESS subscription

    /**
     * Defer the short-press event until key release and suppress it when a
     * long-press already fired. When no source requests it (default) the short
     * press is emitted on key-down. Each source is tracked independently and
     * OR-combined, so a view and an event subscription cannot clobber each
     * other.
     * @param source  One of the DEFER_SRC_* bits identifying the requester.
     * @param enabled Whether that source requests deferred short-press.
     */
    virtual void setDeferShortPress(uint32_t source, bool enabled) {
        (void)source;
        (void)enabled;
    }

    /**
     * Re-emit a held key on a repeat schedule, for every view. While a single
     * key is held, the key is re-buffered every period_ms after an initial
     * delay. Mutually exclusive with long-press: a non-zero period suppresses
     * the long-press for held keys. Pass 0/0 to disable (default).
     * @param initial_ms Delay before the first repeat.
     * @param period_ms  Interval between subsequent repeats; 0 disables.
     */
    virtual void setKeyRepeat(uint16_t initial_ms, uint16_t period_ms) {
        (void)initial_ms;
        (void)period_ms;
    }

    /**
     * Prepare keypad for sleep mode
     * Disables ISR and interrupt to prevent spurious wakeups
     */
    virtual void prepareForSleep() = 0;

    /**
     * Recover keypad after sleep wakeup
     * Waits for keys to be released, clears buffer, re-enables ISR
     */
    virtual void recoverFromSleep() = 0;

    /**
     * Clear key buffer (consume all pending keys)
     */
    virtual void clearBuffer() = 0;
};

// Factory function to get keypad instance
IKeypad* getKeypadInstance();

} // namespace cdc::hal
