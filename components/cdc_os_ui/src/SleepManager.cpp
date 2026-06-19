#include "cdc_os_ui/SleepManager.h"
#include "cdc_os_ui/views/LockScreenView.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_hal/ISleepController.h"
#include "cdc_hal/IPowerManager.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_core/EventBus.h"
#include "usb_badge/usb_cdc.h"
#include "cdc_log.h"
#include "esp_timer.h"
#include <ctime>
#include <cstdio>
#include <cstring>

static const char* TAG = "SleepMgr";

namespace cdc::ui {

/**
 * \brief Forward declaration for lock-screen power status icon refresh.
 */
void updatePowerStatusIcons();

/**
 * \brief Returns singleton sleep manager instance.
 * \return Reference to global `SleepManager` instance.
 */
SleepManager& SleepManager::instance() {
    static SleepManager s_instance;
    return s_instance;
}

/**
 * \brief Initializes sleep-manager dependencies and state.
 * \param sleep Sleep controller dependency.
 * \param power Power manager dependency.
 * \param lockScreen Lock-screen view dependency.
 * \return void
 */
void SleepManager::init(hal::ISleepController* sleep, hal::IPowerManager* power, LockScreenView* lockScreen) {
    sleep_ = sleep;
    power_ = power;
    lockScreen_ = lockScreen;
    lockScreenEnteredMs_ = 0;
    inLightSleep_ = false;
}

/**
 * \brief Resets lock-screen sleep timer using explicit timestamp.
 * \param nowMs Current monotonic time in milliseconds.
 * \return void
 */
void SleepManager::resetTimer(uint32_t nowMs) {
    lockScreenEnteredMs_ = nowMs;
}

/**
 * \brief Resets lock-screen sleep timer using current system tick.
 * \return void
 */
void SleepManager::resetTimer() {
    lockScreenEnteredMs_ = esp_timer_get_time() / 1000;
}

/**
 * \brief Evaluates whether lock-screen light sleep should be entered.
 * \param nowMs Current monotonic time in milliseconds.
 * \return void
 */
void SleepManager::checkLockScreenSleep(uint32_t nowMs) {
    // Only on lock screen (depth == 1)
    if (ViewStack::instance().depth() != 1) return;
    if (!lockScreen_) return;

    // Initialize timer if needed
    if (lockScreenEnteredMs_ == 0) {
        lockScreenEnteredMs_ = nowMs;
        return;
    }

    // Skip if USB connected (keep device responsive)
    if (power_ && power_->isUsbConnected()) {
        lockScreenEnteredMs_ = nowMs;  // Reset timer
        // Remove light sleep icon if shown
        if ((lockScreen_->getStatusIcons() & StatusIcon::LIGHT_SLEEP) != StatusIcon::NONE) {
            lockScreen_->removeStatusIcon(StatusIcon::LIGHT_SLEEP);
        }
        return;
    }

    // Skip if sleep is inhibited (caffeinated mode)
    if (isSleepInhibited()) {
        lockScreenEnteredMs_ = nowMs;  // Reset timer
        return;
    }

    // Check timeout
    uint32_t elapsed = nowMs - lockScreenEnteredMs_;
    if (elapsed >= LIGHT_SLEEP_TIMEOUT_MS) {
        enterLockScreenSleep();
    }
}

/**
 * \brief Enters lock-screen light sleep flow and handles wakeup.
 * \return void
 */
void SleepManager::enterLockScreenSleep() {
    if (!lockScreen_ || !sleep_) return;

    auto& bus = cdc::core::EventBus::instance();
    bus.publish(cdc::core::EventType::SYSTEM_SLEEP_INCOMING);
    bus.process();

    lockScreen_->addStatusIcon(StatusIcon::LIGHT_SLEEP);
    inLightSleep_ = true;

    // Render the icon synchronously so the full SPI command/data stream reaches
    // the panel before light sleep halts the chip; otherwise an in-flight
    // refresh is frozen mid-transfer and the panel is left half-rendered.
    ViewStack::instance().render(true);

    // Light-sleep loop: timer wakeups refresh the clock and re-enter sleep at
    // constant stack depth; a key/USB/unknown wakeup exits the loop.
    do {
        sleep_->enterLightSleep();
    } while (handleWakeup());

    // We only sleep while USB is absent; if USB is present after the loop it was
    // plugged in during sleep, so force a re-enumeration to make the host attach
    // without a manual re-plug. Covers every exit path, including a key-press
    // wakeup that happens to discover USB.
    if (power_ && power_->isUsbConnected()) {
        usb_cdc_reenumerate();
    }
}

/**
 * \brief Handles wakeup behavior after light sleep.
 * \return `true` if light sleep should be re-entered (timer wakeup without
 *         USB), `false` to exit the lock-screen sleep loop.
 */
bool SleepManager::handleWakeup() {
    if (!sleep_ || !lockScreen_) return false;

    // Immediately ensure backlight is off after wakeup to prevent glitches
    // (LEDC may briefly show wrong state after light sleep)
    auto* display = hal::getDisplayInstance();
    if (display && !display->isBacklightOn()) {
        display->backlightOff();
    }

    // The charger wakeup callback refreshed the cached status during recovery,
    // so USB presence is current here even though the main loop did not run.
    bool usb = power_ && power_->isUsbConnected();

    // Decide whether this wakeup fully wakes the device (exit the loop) or is a
    // periodic refresh (timer, or a charger interrupt without USB such as the
    // BQ25895 watchdog) that only updates the clock and re-enters sleep.
    bool userWake;
    if (usb) {
        userWake = true;  // USB plugged in during sleep -> wake fully
    } else {
        hal::WakeupSource source = sleep_->getWakeupSource();
        if (source == hal::WakeupSource::TIMER) {
            userWake = false;
        } else if (source == hal::WakeupSource::GPIO) {
            // Keypad and charger interrupts both report GPIO; only a key press
            // wakes. A charger-only interrupt without USB re-enters sleep.
            userWake = sleep_->wasKeypadWakeup();
        } else {
            userWake = true;  // unknown source -> treat as user interaction
        }
    }

    if (userWake) {
        lockScreen_->removeStatusIcon(StatusIcon::LIGHT_SLEEP);
        lockScreenEnteredMs_ = esp_timer_get_time() / 1000;  // Reset timer
        inLightSleep_ = false;
        updatePowerStatusIcons();
        lockScreen_->markDirty();
        return false;
    }

    // Refresh-only wakeup: update the clock, keep the sleep icon, re-enter sleep.
    char hhmm[6] = {0};
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    if (tm) {
        char buf[40];
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tm->tm_hour, tm->tm_min);
        lockScreen_->setClock(hhmm);
        snprintf(buf, sizeof(buf), "%02d.%02d.%04d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);
        lockScreen_->setDate(buf);
    }

    updatePowerStatusIcons();

    // Repaint only when the displayed minute changed, so the extra charger
    // wakeups add no e-paper writes. The lock screen declares
    // prefersLightRefresh(), so this stays a pure partial refresh.
    if (strcmp(hhmm, lastRenderedHHMM_) != 0) {
        strncpy(lastRenderedHHMM_, hhmm, sizeof(lastRenderedHHMM_) - 1);
        lastRenderedHHMM_[sizeof(lastRenderedHHMM_) - 1] = '\0';
        ViewStack::instance().render(true);
    }
    return true;
}

/**
 * \brief Sleep inhibitor API implementation.
 */

/**
 * \brief Adds a sleep inhibitor reason.
 * \param reason Inhibitor identifier string.
 * \return `true` if added, otherwise `false`.
 */
bool SleepManager::addSleepInhibitor(const char* reason) {
    if (!reason) return false;

    // Check if already exists
    for (uint8_t i = 0; i < inhibitorCount_; i++) {
        if (inhibitors_[i] && strcmp(inhibitors_[i], reason) == 0) {
            return false;  // Already exists
        }
    }

    // Check if full
    if (inhibitorCount_ >= MAX_SLEEP_INHIBITORS) {
        LOG_W(TAG, "Sleep inhibitor list full, cannot add: %s", reason);
        return false;
    }

    // Add inhibitor
    inhibitors_[inhibitorCount_++] = reason;
    LOG_I(TAG, "Sleep inhibitor added: %s (count=%d)", reason, inhibitorCount_);

    // Update icon
    updateCaffeinatedIcon();
    return true;
}

/**
 * \brief Removes a sleep inhibitor reason.
 * \param reason Inhibitor identifier string.
 * \return `true` if removed, otherwise `false`.
 */
bool SleepManager::removeSleepInhibitor(const char* reason) {
    if (!reason) return false;

    // Find and remove
    for (uint8_t i = 0; i < inhibitorCount_; i++) {
        if (inhibitors_[i] && strcmp(inhibitors_[i], reason) == 0) {
            // Shift remaining entries
            for (uint8_t j = i; j < inhibitorCount_ - 1; j++) {
                inhibitors_[j] = inhibitors_[j + 1];
            }
            inhibitors_[--inhibitorCount_] = nullptr;

            LOG_I(TAG, "Sleep inhibitor removed: %s (count=%d)", reason, inhibitorCount_);

            // Update icon
            updateCaffeinatedIcon();
            return true;
        }
    }

    return false;  // Not found
}

/**
 * \brief Updates caffeinated icon visibility on lock screen.
 * \return void
 */
void SleepManager::updateCaffeinatedIcon() {
    if (!lockScreen_) return;

    if (inhibitorCount_ > 0) {
        lockScreen_->addStatusIcon(StatusIcon::CAFFEINATED);
    } else {
        lockScreen_->removeStatusIcon(StatusIcon::CAFFEINATED);
    }
}

} // namespace cdc::ui
