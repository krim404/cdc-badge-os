#pragma once

#include "cdc_hal/ISleepController.h"
#include <cstdint>

// Forward declarations
namespace cdc::ui {
class LockScreenView;
}
namespace cdc::hal {
class IPowerManager;
}

namespace cdc::ui {

// Light sleep timeout (seconds after which lock screen enters light sleep)
static constexpr uint32_t LIGHT_SLEEP_TIMEOUT_MS = 120 * 1000;

// Maximum number of sleep inhibitors
static constexpr uint8_t MAX_SLEEP_INHIBITORS = 8;

// Light sleep management for lock screen
class SleepManager {
public:
    static SleepManager& instance();

    // Initialize with dependencies
    void init(hal::ISleepController* sleep, hal::IPowerManager* power, LockScreenView* lockScreen);

    // Check if should enter sleep (call from ui_process when on lock screen)
    void checkLockScreenSleep(uint32_t nowMs);

    // Reset the sleep timer (on key press or activity)
    void resetTimer(uint32_t nowMs);

    // Reset timer to current time
    void resetTimer();

    // Check if currently in light sleep
    bool isInLightSleep() const { return inLightSleep_; }

    // === Sleep Inhibitor API ===

    /**
     * Add a sleep inhibitor (prevents sleep while active)
     * @param reason Identifier for the inhibitor (e.g., module name)
     * @return true if added, false if already exists or full
     */
    bool addSleepInhibitor(const char* reason);

    /**
     * Remove a sleep inhibitor
     * @param reason Identifier to remove
     * @return true if removed, false if not found
     */
    bool removeSleepInhibitor(const char* reason);

    /**
     * Check if sleep is currently inhibited
     */
    bool isSleepInhibited() const { return inhibitorCount_ > 0; }

    /**
     * Get count of active inhibitors
     */
    uint8_t getInhibitorCount() const { return inhibitorCount_; }

private:
    SleepManager() = default;

    void enterLockScreenSleep();
    bool handleWakeup();
    void updateCaffeinatedIcon();

    hal::ISleepController* sleep_ = nullptr;
    hal::IPowerManager* power_ = nullptr;
    LockScreenView* lockScreen_ = nullptr;

    uint32_t lockScreenEnteredMs_ = 0;
    bool inLightSleep_ = false;

    // Last clock string rendered during the light-sleep loop ("HH:MM"); used to
    // skip e-paper writes on wakeups that do not change the displayed minute.
    char lastRenderedHHMM_[6] = {0};

    // Sleep inhibitors
    const char* inhibitors_[MAX_SLEEP_INHIBITORS] = {};
    uint8_t inhibitorCount_ = 0;
};

} // namespace cdc::ui
