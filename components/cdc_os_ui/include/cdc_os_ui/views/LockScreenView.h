#pragma once

#include "cdc_ui/IView.h"
#include <cstdint>

namespace cdc {
namespace hal {
class ISleepController;
}
}

namespace cdc::ui {

// Deep sleep hold threshold (5 seconds)
static constexpr uint32_t DEEP_SLEEP_HOLD_MS = 5000;

/**
 * Status icons for lock screen
 */
enum class StatusIcon : uint16_t {
    NONE         = 0,
    LOCK         = (1 << 0),   // Padlock
    DEEP_SLEEP   = (1 << 1),   // zzZ
    LIGHT_SLEEP  = (1 << 2),   // z
    BACKLIGHT    = (1 << 3),   // Sun
    USB          = (1 << 4),   // USB connected
    BLE          = (1 << 5),   // Bluetooth
    WIFI         = (1 << 6),   // WiFi connected
    SAO          = (1 << 7),   // SAO detected
    CHARGING     = (1 << 8),   // Battery charging
    NO_BATTERY   = (1 << 9),   // No battery connected
    CAFFEINATED  = (1 << 10),  // Sleep inhibited (coffee cup)
    BACKGROUND   = (1 << 11),  // Background plugin running
};

// Allow bitwise operations
inline StatusIcon operator|(StatusIcon a, StatusIcon b) {
    return static_cast<StatusIcon>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
inline StatusIcon operator&(StatusIcon a, StatusIcon b) {
    return static_cast<StatusIcon>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
inline StatusIcon& operator|=(StatusIcon& a, StatusIcon b) {
    return a = a | b;
}

/**
 * LockScreenView - Main lock screen display
 *
 * Shows:
 * - Clock and date (top)
 * - Status icons (top right)
 * - Three text lines (center)
 * - Battery indicator
 *
 * Press any key to unlock (triggers callback).
 */
class LockScreenView : public ViewBase {
public:
    static constexpr uint8_t MAX_TEXT_LEN = 64;

    /**
     * Unlock callback
     */
    using UnlockCallback = void(*)();

    /**
     * Initialize lock screen
     */
    void init();

    /**
     * Set unlock callback
     */
    void setOnUnlock(UnlockCallback callback) { onUnlock_ = callback; }

    /**
     * Set display texts
     */
    void setDisplayName(const char* name);
    void setInfo(const char* info);
    void setInfo2(const char* info2);

    const char* getDisplayName() const { return name_; }
    const char* getInfo() const { return info_; }
    const char* getInfo2() const { return info2_; }

    /**
     * Set clock display ("HH:MM" or "--:--")
     */
    void setClock(const char* clock);

    /**
     * Set date display ("DD.MM" or empty)
     */
    void setDate(const char* date);

    /**
     * Set battery level (0-100)
     */
    void setBatteryPercent(uint8_t percent);

    /**
     * Set status icons
     */
    void setStatusIcons(StatusIcon icons);
    void addStatusIcon(StatusIcon icon);
    void removeStatusIcon(StatusIcon icon);
    StatusIcon getStatusIcons() const { return statusIcons_; }

    // IView implementation
    void render(bool partial) override;
    InputResult onKey(char key) override;
    void onTick(uint32_t nowMs) override;
    void onEnter(void* context) override;
    void onResume() override;
    const char* getName() const override { return "LockScreenView"; }
    const char* getFooterHint() const override;
    // Clock/icon updates are tiny; keep them pure partials (no forced full).
    bool prefersLightRefresh() const override { return true; }

    /**
     * Toggle backlight from context menu
     */
    void toggleBacklight();

    /**
     * Callback invoked at the start of every render so the host can sync the
     * lock-screen state (battery percent, charge/USB icons) with current
     * hardware readings. No automatic polling happens outside this hook.
     */
    using PreRenderCallback = void (*)();
    void setPreRenderCallback(PreRenderCallback callback) { preRenderCb_ = callback; }

private:
    char name_[MAX_TEXT_LEN] = {};
    char info_[MAX_TEXT_LEN] = {};
    char info2_[MAX_TEXT_LEN] = {};
    char clock_[8] = "--:--";
    char date_[12] = {};
    uint8_t batteryPercent_ = 0;
    StatusIcon statusIcons_ = StatusIcon::NONE;
    UnlockCallback onUnlock_ = nullptr;
    PreRenderCallback preRenderCb_ = nullptr;

    // Long-press N for deep sleep (flight mode)
    uint32_t nPressStartMs_ = 0;
    bool deepSleepMode_ = false;

    void renderStatusIcons(void* gfx, int x, int y);
    void renderBattery(void* gfx, int x, int y);
    void renderDeepSleepScreen();
    void checkDeepSleepTrigger(uint32_t nowMs);
};

} // namespace cdc::ui
