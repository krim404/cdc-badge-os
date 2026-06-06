#include "cdc_os_ui/SleepManager.h"
#include "cdc_os_ui/views/LockScreenView.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_hal/ISleepController.h"
#include "cdc_hal/IPowerManager.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_core/EventBus.h"
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

    // Enter light sleep (blocking call, returns after wakeup)
    sleep_->enterLightSleep();

    // Handle wakeup
    handleWakeup();
}

/**
 * \brief Handles wakeup behavior after light sleep.
 * \return void
 */
void SleepManager::handleWakeup() {
    if (!sleep_ || !lockScreen_) return;

    // Immediately ensure backlight is off after wakeup to prevent glitches
    // (LEDC may briefly show wrong state after light sleep)
    auto* display = hal::getDisplayInstance();
    if (display && !display->isBacklightOn()) {
        display->backlightOff();
    }

    hal::WakeupSource source = sleep_->getWakeupSource();

    if (source == hal::WakeupSource::GPIO) {
        // Key press wakeup - user interaction
        lockScreen_->removeStatusIcon(StatusIcon::LIGHT_SLEEP);
        lockScreenEnteredMs_ = esp_timer_get_time() / 1000;  // Reset timer
        inLightSleep_ = false;

        // Update status icons (battery, USB, etc.)
        updatePowerStatusIcons();

        // Force display refresh
        lockScreen_->markDirty();

    } else if (source == hal::WakeupSource::TIMER) {
        // Timer wakeup - just update clock, keep icon, go back to sleep
        time_t now = time(nullptr);
        struct tm* tm = localtime(&now);
        if (tm) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
            lockScreen_->setClock(buf);
            snprintf(buf, sizeof(buf), "%02d.%02d.%04d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);
            lockScreen_->setDate(buf);
        }

        // Update power icons
        updatePowerStatusIcons();

        // Render clock update synchronously so the panel update completes
        // before we re-enter light sleep below. The lock screen declares
        // prefersLightRefresh(), so this stays a pure partial (never promoted
        // to a flickering full refresh).
        ViewStack::instance().render(true);

        // Check if USB was connected during sleep
        if (power_ && power_->isUsbConnected()) {
            // USB connected - exit light sleep mode
            lockScreen_->removeStatusIcon(StatusIcon::LIGHT_SLEEP);
            lockScreenEnteredMs_ = esp_timer_get_time() / 1000;
            inLightSleep_ = false;
        } else {
            // Go back to sleep immediately
            sleep_->enterLightSleep();
            handleWakeup();  // Recursive call to handle next wakeup
        }
    } else {
        // Unknown wakeup - treat like GPIO
        lockScreen_->removeStatusIcon(StatusIcon::LIGHT_SLEEP);
        lockScreenEnteredMs_ = esp_timer_get_time() / 1000;
        inLightSleep_ = false;
    }
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
