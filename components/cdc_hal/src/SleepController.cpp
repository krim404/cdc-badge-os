/**
 * ESP32-S3 Sleep Controller Implementation
 *
 * Manages light sleep and deep sleep modes.
 *
 * Light Sleep: CPU paused, fast wake, preserves state
 * Deep Sleep: Full power down, causes reset, state in RTC memory only
 *
 * Based on: ~/GIT/cdc-badge-os-legacy/main/app_power.cpp
 */

#include "cdc_hal/ISleepController.h"
#include "cdc_hal/IKeypad.h"
#include "cdc_hal/IPowerManager.h"
#include "cdc_hal/hw_config.h"
#include "cdc_log.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char* TAG = "SLEEP";

namespace cdc::hal {

/** \brief Default light-sleep timer interval in seconds. */
static constexpr uint32_t DEFAULT_LIGHT_SLEEP_INTERVAL_S = 60;

/** \brief NVS namespace/key for persisted sleep interval. */
static constexpr const char* NVS_NAMESPACE = "sleep";
static constexpr const char* NVS_KEY_INTERVAL = "interval";

/** \brief Maximum number of registered callbacks per callback list. */
static constexpr size_t MAX_CALLBACKS = 8;

/** \brief RTC-retained flag indicating previous deep-sleep state. */
RTC_DATA_ATTR static bool g_was_in_deep_sleep = false;

// RTC-retained diagnostic counters. Survive deep-sleep wake and external
// (EN) reset; cleared only on true power loss. Used to distinguish a
// spurious-wake/reset loop from a never-firing wake source after long sleeps.
RTC_DATA_ATTR static uint32_t g_diag_boot_count = 0;
RTC_DATA_ATTR static uint32_t g_diag_deep_sleep_count = 0;

class Esp32SleepController : public ISleepController {
public:
    Esp32SleepController() = default;

    // IService implementation
    bool init() override;
    bool start() override { state_ = core::ServiceState::STARTED; return true; }
    void stop() override { state_ = core::ServiceState::STOPPED; }
    core::ServiceState getState() const override { return state_; }
    const char* getName() const override { return "sleep"; }

    // ISleepController implementation
    void enterLightSleep() override;
    [[noreturn]] void enterDeepSleep() override;
    WakeupSource getWakeupSource() const override;
    bool wasKeypadWakeup() const override { return lastKeypadWake_; }
    bool wasInDeepSleep() const override { return g_was_in_deep_sleep; }
    void clearDeepSleepFlag() override { g_was_in_deep_sleep = false; }
    void setLightSleepInterval(uint32_t seconds) override;
    uint32_t getLightSleepInterval() const override { return lightSleepIntervalS_; }
    void prepareGpioForSleep() override;
    void stabilizeGpioAfterWakeup() override;

    // Callback registration
    bool registerPreSleepCallback(const SleepCallbackEntry& entry) override;
    bool registerWakeupCallback(const SleepCallbackEntry& entry) override;
    void unregisterCallbacks(const char* moduleName) override;

private:
    void invokeCallbacks(SleepCallbackEntry* callbacks, size_t count);
    bool registerCallback(SleepCallbackEntry* callbacks, size_t* count,
                          const SleepCallbackEntry& entry, const char* logLabel);
    void loadFromNvs();
    void saveToNvs();

    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;
    bool lightSleepConfigured_ = false;
    bool lastKeypadWake_ = false;
    uint32_t lightSleepIntervalS_ = DEFAULT_LIGHT_SLEEP_INTERVAL_S;

    // Callback storage
    SleepCallbackEntry preSleepCallbacks_[MAX_CALLBACKS] = {};
    SleepCallbackEntry wakeupCallbacks_[MAX_CALLBACKS] = {};
    size_t preSleepCount_ = 0;
    size_t wakeupCount_ = 0;
};

/**
 * \brief Initializes sleep controller configuration and wake-state tracking.
 * \return `true` on successful initialization.
 */
bool Esp32SleepController::init() {
    if (state_ != core::ServiceState::UNINITIALIZED) {
        return state_ == core::ServiceState::INITIALIZED ||
               state_ == core::ServiceState::STARTED;
    }

    LOG_I(TAG, "Initializing sleep controller");

    // Load saved interval from NVS
    loadFromNvs();

    // Check if we woke from deep sleep
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    // Single-line boot diagnostic (no per-loop logging). reset/wake are the
    // ESP-IDF enum values; boot/deep counters are RTC-retained across wake and
    // EN reset, so a value jump after an unattended sleep reveals a
    // spurious-wake/reset loop versus a wake source that never fired.
    ++g_diag_boot_count;
    LOG_W(TAG, "DeepSleep-Diag: boot#%lu reset=%d wake=%d deepEntries=%lu wasDeep=%d",
          (unsigned long)g_diag_boot_count, (int)esp_reset_reason(), (int)cause,
          (unsigned long)g_diag_deep_sleep_count, (int)g_was_in_deep_sleep);

    if (cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1) {
        // g_was_in_deep_sleep is already set in RTC memory
    } else if (g_was_in_deep_sleep) {
        // Reset occurred but not from deep sleep wakeup
        g_was_in_deep_sleep = false;
    }

    LOG_I(TAG, "Light sleep interval: %lu seconds", (unsigned long)lightSleepIntervalS_);

    state_ = core::ServiceState::INITIALIZED;
    return true;
}

/**
 * \brief Enters light sleep with configured wake sources.
 */
void Esp32SleepController::enterLightSleep() {
    if (!lightSleepConfigured_) {
        // Configure timer wakeup
        if (lightSleepIntervalS_ > 0) {
            esp_sleep_enable_timer_wakeup(lightSleepIntervalS_ * 1000000ULL);
        }

        // Configure GPIO wakeup (keypad + charger interrupt) - level triggered
        gpio_wakeup_enable(EXP_IRQ_PIN, GPIO_INTR_LOW_LEVEL);
        gpio_wakeup_enable(CHG_IRQ_PIN, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();

        lightSleepConfigured_ = true;
        LOG_I(TAG, "Light sleep configured (GPIO%d+%d + %lus timer)",
                 EXP_IRQ_PIN, CHG_IRQ_PIN, (unsigned long)lightSleepIntervalS_);
    }

    // Invoke pre-sleep callbacks (modules can prepare for sleep)
    invokeCallbacks(preSleepCallbacks_, preSleepCount_);

    // Prepare GPIO before sleep
    prepareGpioForSleep();

    LOG_D(TAG, "Entering light sleep...");

    // Enter light sleep
    esp_light_sleep_start();

    // Capture the keypad IRQ level before recovery reads/clears the expander.
    // The TCA9535 holds its IRQ line low after a key event until the inputs are
    // read, so a low level here reliably identifies a keypad wakeup versus a
    // timer or charger-only interrupt.
    lastKeypadWake_ = (gpio_get_level(EXP_IRQ_PIN) == 0);

    // Reset the task watchdog as soon as we resume: the sleep window can
    // be longer than the TWDT timeout, and any subscribed task (notably the
    // idle task we sit in via WDT_CHECK_IDLE_TASK_CPU0) would otherwise fire
    // the WDT in the middle of coex cleanup, which can land in the panic
    // handler with a spinlock already held.
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        esp_task_wdt_reset();
    }

    // Log wakeup cause
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        LOG_D(TAG, "GPIO wakeup");
    } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        LOG_D(TAG, "Timer wakeup");
    }

    // Stabilize GPIO after wakeup
    stabilizeGpioAfterWakeup();

    // Invoke wakeup callbacks (modules can resume operations)
    invokeCallbacks(wakeupCallbacks_, wakeupCount_);
}

/**
 * \brief Enters deep sleep mode and never returns.
 */
[[noreturn]] void Esp32SleepController::enterDeepSleep() {
    LOG_I(TAG, "Entering deep sleep mode...");

    // Mark that we're in deep sleep mode (survives reset)
    g_was_in_deep_sleep = true;
    ++g_diag_deep_sleep_count;

    // Configure GPIO wakeup only (no timer)
    // Use EXT1 instead of EXT0 - less RTC GPIO issues on ESP32-S3
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext1_wakeup_io(1ULL << EXP_IRQ_PIN, ESP_EXT1_WAKEUP_ANY_LOW);

    // Enter deep sleep (causes reset on wake)
    esp_deep_sleep_start();

    // Never reached, but satisfies [[noreturn]]
    while (true) { }
}

/**
 * \brief Returns last wakeup source reported by ESP-IDF.
 * \return Wakeup source enum value.
 */
WakeupSource Esp32SleepController::getWakeupSource() const {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    switch (cause) {
        case ESP_SLEEP_WAKEUP_TIMER:
            return WakeupSource::TIMER;
        case ESP_SLEEP_WAKEUP_GPIO:
            return WakeupSource::GPIO;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            return WakeupSource::TOUCHPAD;
        case ESP_SLEEP_WAKEUP_EXT0:
            return WakeupSource::EXT0;
        case ESP_SLEEP_WAKEUP_EXT1:
            return WakeupSource::EXT1;
        default:
            return WakeupSource::UNKNOWN;
    }
}

/**
 * \brief Updates light-sleep interval and persists it.
 * \param seconds New interval in seconds.
 */
void Esp32SleepController::setLightSleepInterval(uint32_t seconds) {
    if (lightSleepIntervalS_ == seconds) return;

    lightSleepIntervalS_ = seconds;
    lightSleepConfigured_ = false;  // Force reconfiguration on next sleep

    // Save to NVS
    saveToNvs();

    LOG_I(TAG, "Light sleep interval set to %lus", (unsigned long)seconds);
}

/**
 * \brief Prepares GPIO/keypad state for entering sleep.
 */
void Esp32SleepController::prepareGpioForSleep() {
    LOG_D(TAG, "Preparing GPIO for sleep...");

    // Use keypad's comprehensive sleep preparation
    auto* keypad = getKeypadInstance();
    if (keypad) {
        keypad->prepareForSleep();
    }

    // Arm the charger IRQ line for the level-triggered USB-plug wakeup
    auto* power = getPowerManagerInstance();
    if (power) {
        power->prepareForSleep();
    }
}

/**
 * \brief Restores/stabilizes GPIO/keypad state after wake.
 */
void Esp32SleepController::stabilizeGpioAfterWakeup() {
    LOG_D(TAG, "Stabilizing GPIO after wakeup...");

    // Use keypad's comprehensive recovery (waits for keys, clears buffer, etc.)
    auto* keypad = getKeypadInstance();
    if (keypad) {
        keypad->recoverFromSleep();
    }

    // Refresh charger status (USB presence) and re-arm the charger IRQ
    auto* power = getPowerManagerInstance();
    if (power) {
        power->recoverFromSleep();
    }
}

/**
 * \brief Inserts a callback entry into the array, sorted by priority.
 * \param callbacks Backing storage for the callback list.
 * \param count In/out pointer to the current callback count.
 * \param entry Callback registration descriptor.
 * \param logLabel Human-readable label used in log messages.
 * \return `true` on successful registration, `false` when the limit is reached.
 */
bool Esp32SleepController::registerCallback(SleepCallbackEntry* callbacks, size_t* count,
                                            const SleepCallbackEntry& entry,
                                            const char* logLabel) {
    if (*count >= MAX_CALLBACKS) {
        LOG_W(TAG, "%s callback limit reached", logLabel);
        return false;
    }

    // Insert sorted by priority (lower priority = earlier in array)
    size_t insertPos = *count;
    for (size_t i = 0; i < *count; i++) {
        if (entry.priority < callbacks[i].priority) {
            insertPos = i;
            break;
        }
    }

    // Shift existing entries
    for (size_t i = *count; i > insertPos; i--) {
        callbacks[i] = callbacks[i - 1];
    }

    callbacks[insertPos] = entry;
    (*count)++;

    LOG_I(TAG, "Registered %s callback: %s (priority %d)",
             logLabel, entry.moduleName, entry.priority);
    return true;
}

/**
 * \brief Registers callback invoked before sleep transition.
 * \param entry Callback registration descriptor.
 * \return `true` on successful registration.
 */
bool Esp32SleepController::registerPreSleepCallback(const SleepCallbackEntry& entry) {
    return registerCallback(preSleepCallbacks_, &preSleepCount_, entry, "pre-sleep");
}

/**
 * \brief Registers callback invoked after wakeup.
 * \param entry Callback registration descriptor.
 * \return `true` on successful registration.
 */
bool Esp32SleepController::registerWakeupCallback(const SleepCallbackEntry& entry) {
    return registerCallback(wakeupCallbacks_, &wakeupCount_, entry, "wakeup");
}

/**
 * \brief Removes all callbacks belonging to one module.
 * \param moduleName Module name key.
 */
void Esp32SleepController::unregisterCallbacks(const char* moduleName) {
    if (!moduleName) return;

    // Remove from pre-sleep callbacks
    for (size_t i = 0; i < preSleepCount_; ) {
        if (preSleepCallbacks_[i].moduleName &&
            strcmp(preSleepCallbacks_[i].moduleName, moduleName) == 0) {
            // Shift remaining entries
            for (size_t j = i; j < preSleepCount_ - 1; j++) {
                preSleepCallbacks_[j] = preSleepCallbacks_[j + 1];
            }
            preSleepCount_--;
        } else {
            i++;
        }
    }

    // Remove from wakeup callbacks
    for (size_t i = 0; i < wakeupCount_; ) {
        if (wakeupCallbacks_[i].moduleName &&
            strcmp(wakeupCallbacks_[i].moduleName, moduleName) == 0) {
            for (size_t j = i; j < wakeupCount_ - 1; j++) {
                wakeupCallbacks_[j] = wakeupCallbacks_[j + 1];
            }
            wakeupCount_--;
        } else {
            i++;
        }
    }

    LOG_I(TAG, "Unregistered callbacks for: %s", moduleName);
}

/**
 * \brief Invokes callback list in current stored order.
 * \param callbacks Callback array.
 * \param count Number of active callbacks.
 */
void Esp32SleepController::invokeCallbacks(SleepCallbackEntry* callbacks, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (callbacks[i].callback) {
            LOG_D(TAG, "Invoking callback: %s", callbacks[i].moduleName);
            callbacks[i].callback(callbacks[i].context);
        }
    }
}

/**
 * \brief Loads persisted light-sleep interval from NVS.
 */
void Esp32SleepController::loadFromNvs() {
    static constexpr uint32_t MIN_INTERVAL_S = 10;
    static constexpr uint32_t MAX_INTERVAL_S = 86400;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint32_t interval = 0;
        if (nvs_get_u32(handle, NVS_KEY_INTERVAL, &interval) == ESP_OK) {
            if (interval < MIN_INTERVAL_S) {
                interval = MIN_INTERVAL_S;
            } else if (interval > MAX_INTERVAL_S) {
                interval = MAX_INTERVAL_S;
            }
            lightSleepIntervalS_ = interval;
        }
        nvs_close(handle);
    }
}

/**
 * \brief Persists current light-sleep interval to NVS.
 */
void Esp32SleepController::saveToNvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_set_u32(handle, NVS_KEY_INTERVAL, lightSleepIntervalS_);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

/** \brief Singleton sleep-controller instance. */
static Esp32SleepController g_sleepController;

/**
 * \brief Returns the singleton sleep controller service instance.
 * \return Pointer to the global `ISleepController` implementation.
 */
ISleepController* getSleepControllerInstance() {
    return &g_sleepController;
}

} // namespace cdc::hal
