#pragma once

#include "cdc_core/IService.h"
#include <cstdint>

namespace cdc::hal {

/**
 * Sleep mode types
 */
enum class SleepMode : uint8_t {
    NONE,           // Not sleeping
    LIGHT_SLEEP,    // CPU paused, peripherals active, fast wake
    DEEP_SLEEP      // Full power down, causes reset on wake
};

/**
 * Wakeup source
 */
enum class WakeupSource : uint8_t {
    UNKNOWN,
    TIMER,          // Timer wakeup
    GPIO,           // GPIO wakeup (keypad)
    TOUCHPAD,       // Touch wakeup
    EXT0,           // EXT0 wakeup
    EXT1            // EXT1 wakeup
};

/**
 * Callback types for sleep/wakeup events
 */
using SleepCallback = void (*)(void* context);

/**
 * Sleep callback registration entry
 */
struct SleepCallbackEntry {
    const char* moduleName;     // For debugging/unregister
    SleepCallback callback;
    void* context;
    uint8_t priority;           // Lower = called first (0-255)
};

/**
 * Sleep Controller Interface
 *
 * Manages light sleep and deep sleep modes for the ESP32-S3.
 *
 * Light Sleep: CPU paused, RTC running, fast wake (<10ms), GPIO wakeup
 * Deep Sleep: Full power down, causes reset on wake, survives in RTC memory
 *
 * Reference: ~/GIT/cdc-badge-os-legacy/main/app_power.cpp
 */
class ISleepController : public core::IService {
public:
    virtual ~ISleepController() = default;

    /**
     * Enter light sleep mode
     *
     * CPU is paused, but RTC continues. Wakes on:
     * - Timer (configurable interval)
     * - GPIO (keypad interrupt)
     *
     * Returns immediately after wakeup.
     */
    virtual void enterLightSleep() = 0;

    /**
     * Enter deep sleep mode
     *
     * Full power down. Causes RESET on wakeup.
     * Only GPIO wakeup is configured (no timer).
     *
     * Before calling:
     * - Turn off backlight
     * - Update display (show sleep icon)
     * - Wait for display refresh to complete
     *
     * This function does NOT return.
     */
    [[noreturn]] virtual void enterDeepSleep() = 0;

    /**
     * Get last wakeup source
     */
    virtual WakeupSource getWakeupSource() const = 0;

    /**
     * Whether the last light-sleep wakeup was caused by the keypad.
     *
     * Both the keypad and the charger interrupt report WakeupSource::GPIO. The
     * keypad's I/O expander holds its IRQ line low after a key event until the
     * inputs are read, so this distinguishes a key press from a charger-only
     * interrupt (e.g. USB plug or charger watchdog).
     */
    virtual bool wasKeypadWakeup() const = 0;

    /**
     * Check if device just woke from deep sleep
     * (persistent flag in RTC memory)
     */
    virtual bool wasInDeepSleep() const = 0;

    /**
     * Clear deep sleep flag (call after handling deep sleep wake)
     */
    virtual void clearDeepSleepFlag() = 0;

    /**
     * Set light sleep timer interval in seconds
     * @param seconds Interval between timer wakeups (0 = no timer wakeup)
     */
    virtual void setLightSleepInterval(uint32_t seconds) = 0;

    /**
     * Get light sleep timer interval
     */
    virtual uint32_t getLightSleepInterval() const = 0;

    /**
     * Prepare GPIO for sleep (disable interrupts to avoid conflicts)
     */
    virtual void prepareGpioForSleep() = 0;

    /**
     * Stabilize GPIO after wakeup (restore edge trigger)
     */
    virtual void stabilizeGpioAfterWakeup() = 0;

    // === Callback Registration ===

    /**
     * Register a callback to be called BEFORE entering sleep
     * Use to prepare module state (stop timers, save data, etc.)
     * @param entry Callback entry with module name and priority
     * @return true if registered successfully
     */
    virtual bool registerPreSleepCallback(const SleepCallbackEntry& entry) = 0;

    /**
     * Register a callback to be called AFTER waking from light sleep
     * Use to resume module operations, refresh data, etc.
     * NOTE: Not called after deep sleep (that's a reset)
     * @param entry Callback entry with module name and priority
     * @return true if registered successfully
     */
    virtual bool registerWakeupCallback(const SleepCallbackEntry& entry) = 0;

    /**
     * Unregister all callbacks for a module
     * @param moduleName Module name used in registration
     */
    virtual void unregisterCallbacks(const char* moduleName) = 0;
};

// Factory function
ISleepController* getSleepControllerInstance();

} // namespace cdc::hal
