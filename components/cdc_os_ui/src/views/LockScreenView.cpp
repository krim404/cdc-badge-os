/**
 * LockScreenView Implementation
 *
 * Main lock screen with clock, icons, and user info.
 */

#include "cdc_os_ui/views/LockScreenView.h"
#include "cdc_os_ui/WifiHandlers.h"
#include "cdc_ui/I18n.h"
#include "cdc_views/ContextMenuView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_hal/IKeypad.h"
#include "cdc_hal/ISleepController.h"
#include "cdc_core/ModuleRegistry.h"
#include "plugin_manager/PluginManager.h"
#include "cdc_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <goodisplay/gdey029T94.h>
#include "cdc_views/Fonts.h"
#include "cdc_views/RenderHelpers.h"
#include <cstring>

static const char* TAG = "LockScreen";

/**
 * \brief Display layout constants.
 */
static constexpr int CLOCK_Y = 5;
static constexpr int DATE_Y = 22;
static constexpr int ICONS_Y = 5;
static constexpr int NAME_Y = 60;       // Name position (FreeFont baseline)
static constexpr int INFO_Y = 80;       // Info line 1 (small text)
static constexpr int INFO2_Y = 96;      // Info line 2 (small text)
static constexpr int BATTERY_X = 260;
static constexpr int BATTERY_Y = 5;
static constexpr int DISPLAY_WIDTH = 296;


/**
 * \brief Battery icon dimensions.
 */
static constexpr int BAT_WIDTH = 28;
static constexpr int BAT_HEIGHT = 12;
static constexpr int BAT_TIP_WIDTH = 3;
static constexpr int BAT_TIP_HEIGHT = 6;

namespace cdc::ui {

/**
 * \brief Initializes lock-screen state fields to defaults.
 */
void LockScreenView::init() {
    memset(name_, 0, sizeof(name_));
    memset(info_, 0, sizeof(info_));
    memset(info2_, 0, sizeof(info2_));
    strcpy(clock_, "--:--");
    memset(date_, 0, sizeof(date_));
    batteryPercent_ = 0;
    statusIcons_ = StatusIcon::NONE;
    nPressStartMs_ = 0;
    dirty_ = true;
}

/**
 * \brief Sets primary display name shown on lock screen.
 * \param name Name text (nullable).
 */
void LockScreenView::setDisplayName(const char* name) {
    const char* incoming = name ? name : "";
    if (strncmp(name_, incoming, MAX_TEXT_LEN) == 0) return;
    if (name) {
        strncpy(name_, name, MAX_TEXT_LEN - 1);
        name_[MAX_TEXT_LEN - 1] = '\0';
    } else {
        name_[0] = '\0';
    }
    dirty_ = true;
}

/**
 * \brief Sets first informational line.
 * \param info Info text (nullable).
 */
void LockScreenView::setInfo(const char* info) {
    const char* incoming = info ? info : "";
    if (strncmp(info_, incoming, MAX_TEXT_LEN) == 0) return;
    if (info) {
        strncpy(info_, info, MAX_TEXT_LEN - 1);
        info_[MAX_TEXT_LEN - 1] = '\0';
    } else {
        info_[0] = '\0';
    }
    dirty_ = true;
}

/**
 * \brief Sets second informational line.
 * \param info2 Secondary info text (nullable).
 */
void LockScreenView::setInfo2(const char* info2) {
    const char* incoming = info2 ? info2 : "";
    if (strncmp(info2_, incoming, MAX_TEXT_LEN) == 0) return;
    if (info2) {
        strncpy(info2_, info2, MAX_TEXT_LEN - 1);
        info2_[MAX_TEXT_LEN - 1] = '\0';
    } else {
        info2_[0] = '\0';
    }
    dirty_ = true;
}

/**
 * \brief Sets clock text shown in the header.
 * \param clock Clock text (nullable).
 */
void LockScreenView::setClock(const char* clock) {
    const char* incoming = clock ? clock : "--:--";
    if (strncmp(clock_, incoming, sizeof(clock_)) == 0) return;
    if (clock) {
        strncpy(clock_, clock, sizeof(clock_) - 1);
        clock_[sizeof(clock_) - 1] = '\0';
    } else {
        strcpy(clock_, "--:--");
    }
    dirty_ = true;
}

/**
 * \brief Sets date text shown below clock.
 * \param date Date text (nullable).
 */
void LockScreenView::setDate(const char* date) {
    const char* incoming = date ? date : "";
    if (strncmp(date_, incoming, sizeof(date_)) == 0) return;
    if (date) {
        strncpy(date_, date, sizeof(date_) - 1);
        date_[sizeof(date_) - 1] = '\0';
    } else {
        date_[0] = '\0';
    }
    dirty_ = true;
}

/**
 * \brief Updates battery percentage indicator.
 * \param percent Battery percentage (clamped to 0..100).
 */
void LockScreenView::setBatteryPercent(uint8_t percent) {
    if (percent > 100) percent = 100;
    if (batteryPercent_ != percent) {
        batteryPercent_ = percent;
        dirty_ = true;
    }
}

/**
 * \brief Replaces full status-icon bitmask.
 * \param icons New status icon mask.
 */
void LockScreenView::setStatusIcons(StatusIcon icons) {
    if (statusIcons_ != icons) {
        statusIcons_ = icons;
        dirty_ = true;
    }
}

/**
 * \brief Adds one status icon flag.
 * \param icon Icon bit to set.
 */
void LockScreenView::addStatusIcon(StatusIcon icon) {
    setStatusIcons(statusIcons_ | icon);
}

/**
 * \brief Removes one status icon flag.
 * \param icon Icon bit to clear.
 */
void LockScreenView::removeStatusIcon(StatusIcon icon) {
    setStatusIcons(static_cast<StatusIcon>(
        static_cast<uint16_t>(statusIcons_) & ~static_cast<uint16_t>(icon)
    ));
}

/**
 * \brief Static lock-screen instance pointer for C-style callbacks.
 */
static LockScreenView* s_lockScreenInstance = nullptr;

/**
 * \brief Handles entering lock screen and updates backlight behavior.
 * \param context Optional enter context (unused).
 */
void LockScreenView::onEnter(void* context) {
    (void)context;
    s_lockScreenInstance = this;
    nPressStartMs_ = 0;  // Reset deep sleep trigger

    // Turn off backlight when entering lock screen (unless persistent light is on)
    if ((statusIcons_ & StatusIcon::BACKLIGHT) == StatusIcon::NONE) {
        hal::IDisplay* display = hal::getDisplayInstance();
        if (display) {
            display->backlightOff();
        }
    }
    dirty_ = true;
}

/**
 * \brief Handles returning to lock screen and reapplies backlight policy.
 */
void LockScreenView::onResume() {
    // Called when returning to lock screen (e.g., via N from main menu)
    nPressStartMs_ = 0;  // Reset deep sleep trigger

    // Turn off backlight (unless persistent light is on)
    if ((statusIcons_ & StatusIcon::BACKLIGHT) == StatusIcon::NONE) {
        hal::IDisplay* display = hal::getDisplayInstance();
        if (display) {
            display->backlightOff();
        }
    }
    dirty_ = true;
}

/**
 * \brief Toggles display backlight and corresponding status icon.
 */
void LockScreenView::toggleBacklight() {
    hal::IDisplay* display = hal::getDisplayInstance();
    if (!display) return;

    if (display->isBacklightOn()) {
        display->backlightOff();
        removeStatusIcon(StatusIcon::BACKLIGHT);
    } else {
        display->backlightOn();
        addStatusIcon(StatusIcon::BACKLIGHT);
    }
    dirty_ = true;
}

/**
 * \brief Context-menu callback toggling lock-screen backlight mode.
 */
static void onLightMenuCallback() {
    if (s_lockScreenInstance) {
        s_lockScreenInstance->toggleBacklight();
    }
    hideContextMenu();
}

/**
 * \brief Context-menu callback toggling WiFi on/off via the user intent flag.
 */
static void onWifiToggleCallback() {
    hideContextMenu();
    auto& wifi = WifiHandlers::instance();
    wifi.setUserEnabled(!wifi.isConnected());
}

/**
 * \brief Storage for dynamic context-menu items contributed by modules.
 */
static constexpr uint8_t MAX_CONTEXT_ITEMS = 12;
static constexpr uint8_t MAX_PLUGIN_ITEMS  = 4;
// Number of module context-menu wrapper callbacks defined below. The module
// item array and the registry request are both bounded by this so that
// s_moduleCallbacks is never indexed out of range. Keep in sync with the
// s_moduleCallbacks[] table (enforced by static_assert after it).
static constexpr uint8_t MAX_MODULE_CONTEXT_ITEMS = 7;
static ContextMenuItem s_contextItems[MAX_CONTEXT_ITEMS];
static core::LockScreenContextItem s_moduleContextItems[MAX_MODULE_CONTEXT_ITEMS];
static uint8_t s_moduleContextCount = 0;

static cdc::plugin_manager::PluginManager::LockscreenItem s_pluginContextItems[MAX_PLUGIN_ITEMS];
static uint8_t s_pluginContextCount = 0;

/**
 * \brief Wrapper callback for module context item at index 0.
 * \return void
 */
static void moduleContextCallback0() { if (s_moduleContextItems[0].callback) { s_moduleContextItems[0].callback(); } hideContextMenu(); }
/**
 * \brief Wrapper callback for module context item at index 1.
 * \return void
 */
static void moduleContextCallback1() { if (s_moduleContextItems[1].callback) { s_moduleContextItems[1].callback(); } hideContextMenu(); }
/**
 * \brief Wrapper callback for module context item at index 2.
 * \return void
 */
static void moduleContextCallback2() { if (s_moduleContextItems[2].callback) { s_moduleContextItems[2].callback(); } hideContextMenu(); }
/**
 * \brief Wrapper callback for module context item at index 3.
 * \return void
 */
static void moduleContextCallback3() { if (s_moduleContextItems[3].callback) { s_moduleContextItems[3].callback(); } hideContextMenu(); }
/**
 * \brief Wrapper callback for module context item at index 4.
 * \return void
 */
static void moduleContextCallback4() { if (s_moduleContextItems[4].callback) { s_moduleContextItems[4].callback(); } hideContextMenu(); }
/**
 * \brief Wrapper callback for module context item at index 5.
 * \return void
 */
static void moduleContextCallback5() { if (s_moduleContextItems[5].callback) { s_moduleContextItems[5].callback(); } hideContextMenu(); }
/**
 * \brief Wrapper callback for module context item at index 6.
 * \return void
 */
static void moduleContextCallback6() { if (s_moduleContextItems[6].callback) { s_moduleContextItems[6].callback(); } hideContextMenu(); }

static void (*const s_moduleCallbacks[])() = {
    moduleContextCallback0, moduleContextCallback1, moduleContextCallback2,
    moduleContextCallback3, moduleContextCallback4, moduleContextCallback5,
    moduleContextCallback6
};
static_assert(sizeof(s_moduleCallbacks) / sizeof(s_moduleCallbacks[0]) == MAX_MODULE_CONTEXT_ITEMS,
              "module context callback count must match MAX_MODULE_CONTEXT_ITEMS");

static void pluginContextCallback0() {
    cdc::plugin_manager::PluginManager::instance().triggerLockscreenItem(s_pluginContextItems[0]);
    hideContextMenu();
}
static void pluginContextCallback1() {
    cdc::plugin_manager::PluginManager::instance().triggerLockscreenItem(s_pluginContextItems[1]);
    hideContextMenu();
}
static void pluginContextCallback2() {
    cdc::plugin_manager::PluginManager::instance().triggerLockscreenItem(s_pluginContextItems[2]);
    hideContextMenu();
}
static void pluginContextCallback3() {
    cdc::plugin_manager::PluginManager::instance().triggerLockscreenItem(s_pluginContextItems[3]);
    hideContextMenu();
}
static void (*const s_pluginCallbacks[MAX_PLUGIN_ITEMS])() = {
    pluginContextCallback0, pluginContextCallback1,
    pluginContextCallback2, pluginContextCallback3,
};

/**
 * \brief Handles lock-screen key actions.
 *
 * Key `3` opens the context menu; any other key triggers unlock callback.
 *
 * \param key Pressed key code.
 * \return Input consumption result.
 */
InputResult LockScreenView::onKey(char key) {
    // KEY_MENU ('3') opens context menu for light toggle + module items
    if (key == KEY_MENU) {
        uint8_t itemCount = 0;

        // First item: Light toggle (built-in)
        s_contextItems[itemCount++] = {ui::tr("core.light"), onLightMenuCallback};

        // Second item: WiFi toggle (built-in); label reflects current state
        const char* wifiLabel = WifiHandlers::instance().isConnected()
            ? ui::tr("core.wifi_off") : ui::tr("core.wifi_on");
        s_contextItems[itemCount++] = {wifiLabel, onWifiToggleCallback};

        // Get module items from registry
        auto& moduleReg = core::ModuleRegistry::instance();
        s_moduleContextCount = moduleReg.getLockScreenContextItems(s_moduleContextItems, MAX_MODULE_CONTEXT_ITEMS);

        // Add module items to context menu
        for (uint8_t i = 0; i < s_moduleContextCount && itemCount < MAX_CONTEXT_ITEMS; i++) {
            const char* label = s_moduleContextItems[i].getLabel ? s_moduleContextItems[i].getLabel() : "???";
            s_contextItems[itemCount++] = {label, s_moduleCallbacks[i]};
        }

        // Plugin-contributed lockscreen items.
        s_pluginContextCount = cdc::plugin_manager::PluginManager::instance()
            .getLockscreenItems(s_pluginContextItems, MAX_PLUGIN_ITEMS);
        for (uint8_t i = 0; i < s_pluginContextCount && itemCount < MAX_CONTEXT_ITEMS; i++) {
            s_contextItems[itemCount++] = {s_pluginContextItems[i].label, s_pluginCallbacks[i]};
        }

        showContextMenu(ui::tr("core.actions"), s_contextItems, itemCount);
        return InputResult::CONSUMED;
    }

    // Any other key triggers unlock
    if (onUnlock_) {
        onUnlock_();
    }
    return InputResult::CONSUMED;
}

/**
 * \brief Per-tick handler for long-press deep-sleep detection.
 * \param nowMs Current uptime in milliseconds.
 */
void LockScreenView::onTick(uint32_t nowMs) {
    // Check for long-press N -> deep sleep (flight mode)
    checkDeepSleepTrigger(nowMs);
}

/**
 * \brief Detects and handles long press on `N` key to enter deep sleep.
 * \param nowMs Current uptime in milliseconds.
 */
void LockScreenView::checkDeepSleepTrigger(uint32_t nowMs) {
    auto* keypad = hal::getKeypadInstance();
    if (!keypad) return;

    bool nPressed = keypad->isKeyPressed(hal::Key::KEY_NO);

    if (nPressed) {
        if (nPressStartMs_ == 0) {
            // N key just pressed, start timing
            nPressStartMs_ = nowMs;
        } else {
            // Check if held long enough
            uint32_t elapsed = nowMs - nPressStartMs_;
            if (elapsed >= DEEP_SLEEP_HOLD_MS) {
                LOG_I(TAG, "Long-press N detected, entering deep sleep...");

                // Render deep sleep screen and push to e-paper
                renderDeepSleepScreen();

                // Turn off backlight
                auto* display = hal::getDisplayInstance();
                if (display) {
                    display->backlightOff();
                }

                // Wait 2s so user can release button without triggering wakeup
                vTaskDelay(pdMS_TO_TICKS(2000));

                // Enter deep sleep (does not return - causes reset on wake)
                auto* sleep = hal::getSleepControllerInstance();
                if (sleep) {
                    sleep->enterDeepSleep();
                }

                // Should not reach here
                nPressStartMs_ = 0;
            }
        }
    } else {
        // N released, reset timer
        nPressStartMs_ = 0;
    }
}

/**
 * \brief Returns footer hint based on current lock-screen mode.
 * \return Localized footer hint string.
 */
const char* LockScreenView::getFooterHint() const {
    switch (footerMode_) {
        case FooterMode::DEEP_SLEEP: return ui::tr("core.deep_sleep");
        case FooterMode::SHIP_MODE:  return ui::tr("core.ship_mode_hint");
        default:                     return ui::tr("core.press_any_key");
    }
}

/**
 * \brief Renders battery icon including charging/no-battery overlays.
 * \param gfxPtr Native graphics pointer.
 * \param x Left coordinate.
 * \param y Top coordinate.
 */
void LockScreenView::renderBattery(void* gfxPtr, int x, int y) {
    auto* gfx = static_cast<Gdey029T94*>(gfxPtr);
    // Battery outline
    gfx->drawRect(x, y, BAT_WIDTH, BAT_HEIGHT, EPD_BLACK);

    // Battery tip
    gfx->fillRect(x + BAT_WIDTH, y + (BAT_HEIGHT - BAT_TIP_HEIGHT) / 2,
                  BAT_TIP_WIDTH, BAT_TIP_HEIGHT, EPD_BLACK);

    // Fill level (skip if no battery - show empty outline)
    bool noBattery = (statusIcons_ & StatusIcon::NO_BATTERY) != StatusIcon::NONE;
    if (!noBattery) {
        int fillWidth = (batteryPercent_ * (BAT_WIDTH - 4)) / 100;
        if (fillWidth > 0) {
            gfx->fillRect(x + 2, y + 2, fillWidth, BAT_HEIGHT - 4, EPD_BLACK);
        }
    }

    // Charging indicator
    if ((statusIcons_ & StatusIcon::CHARGING) != StatusIcon::NONE) {
        // Draw lightning bolt
        gfx->drawLine(x + BAT_WIDTH / 2 + 2, y + 1, x + BAT_WIDTH / 2 - 2, y + BAT_HEIGHT / 2, EPD_BLACK);
        gfx->drawLine(x + BAT_WIDTH / 2 - 2, y + BAT_HEIGHT / 2, x + BAT_WIDTH / 2 + 2, y + BAT_HEIGHT / 2, EPD_BLACK);
        gfx->drawLine(x + BAT_WIDTH / 2 + 2, y + BAT_HEIGHT / 2, x + BAT_WIDTH / 2 - 2, y + BAT_HEIGHT - 2, EPD_BLACK);
    }

    // No battery indicator - diagonal strike-through
    if ((statusIcons_ & StatusIcon::NO_BATTERY) != StatusIcon::NONE) {
        gfx->drawLine(x - 2, y + BAT_HEIGHT + 2, x + BAT_WIDTH + BAT_TIP_WIDTH + 2, y - 2, EPD_BLACK);
        gfx->drawLine(x - 2, y + BAT_HEIGHT + 3, x + BAT_WIDTH + BAT_TIP_WIDTH + 2, y - 1, EPD_BLACK);
    }
}

/**
 * \brief Renders top-row status icons.
 * \param gfxPtr Native graphics pointer.
 * \param x Start x coordinate.
 * \param y Start y coordinate.
 */
void LockScreenView::renderStatusIcons(void* gfxPtr, int x, int y) {
    auto* gfx = static_cast<Gdey029T94*>(gfxPtr);
    int iconX = x;
    const int iconSpacing = 14;

    // Lock icon
    if ((statusIcons_ & StatusIcon::LOCK) != StatusIcon::NONE) {
        // Draw padlock
        gfx->drawRect(iconX, y + 4, 8, 6, EPD_BLACK);
        gfx->drawCircle(iconX + 4, y + 3, 3, EPD_BLACK);
        iconX -= iconSpacing;
    }

    // WiFi icon - three stacked ripple arcs with a base dot
    if ((statusIcons_ & StatusIcon::WIFI) != StatusIcon::NONE) {
        const int cx = iconX + 5;
        const int cy = y + 10;

        // Arc 3 (large): 5px horizontal cap with stepped sides
        gfx->drawLine(cx - 2, cy - 8, cx + 2, cy - 8, EPD_BLACK);
        gfx->drawPixel(cx - 3, cy - 7, EPD_BLACK);
        gfx->drawPixel(cx + 3, cy - 7, EPD_BLACK);
        gfx->drawPixel(cx - 4, cy - 6, EPD_BLACK);
        gfx->drawPixel(cx + 4, cy - 6, EPD_BLACK);

        // Arc 2 (medium): 3px horizontal cap with stepped sides
        gfx->drawLine(cx - 1, cy - 5, cx + 1, cy - 5, EPD_BLACK);
        gfx->drawPixel(cx - 2, cy - 4, EPD_BLACK);
        gfx->drawPixel(cx + 2, cy - 4, EPD_BLACK);

        // Arc 1 (small): single-pixel peak
        gfx->drawPixel(cx, cy - 2, EPD_BLACK);

        // Base dot (2x2)
        gfx->fillRect(cx, cy, 2, 2, EPD_BLACK);

        iconX -= iconSpacing;
    }

    // BLE icon
    if ((statusIcons_ & StatusIcon::BLE) != StatusIcon::NONE) {
        // Bluetooth rune
        gfx->drawLine(iconX + 4, y, iconX + 4, y + 10, EPD_BLACK);
        gfx->drawLine(iconX + 4, y, iconX + 8, y + 3, EPD_BLACK);
        gfx->drawLine(iconX + 8, y + 3, iconX + 2, y + 7, EPD_BLACK);
        gfx->drawLine(iconX + 2, y + 3, iconX + 8, y + 7, EPD_BLACK);
        gfx->drawLine(iconX + 8, y + 7, iconX + 4, y + 10, EPD_BLACK);
        iconX -= iconSpacing;
    }

    // USB icon (simplified USB trident)
    if ((statusIcons_ & StatusIcon::USB) != StatusIcon::NONE) {
        int ux = iconX, uy = y;
        // Main stem
        gfx->drawLine(ux + 5, uy + 4, ux + 5, uy + 12, EPD_BLACK);
        // Top horizontal
        gfx->drawLine(ux + 2, uy + 4, ux + 8, uy + 4, EPD_BLACK);
        // Left branch with circle
        gfx->drawLine(ux + 2, uy + 4, ux + 2, uy, EPD_BLACK);
        gfx->fillCircle(ux + 2, uy, 1, EPD_BLACK);
        // Right branch with rectangle
        gfx->drawLine(ux + 8, uy + 4, ux + 8, uy + 2, EPD_BLACK);
        gfx->fillRect(ux + 6, uy, 4, 3, EPD_BLACK);
        // Bottom arrow
        gfx->drawLine(ux + 5, uy + 12, ux + 3, uy + 10, EPD_BLACK);
        gfx->drawLine(ux + 5, uy + 12, ux + 7, uy + 10, EPD_BLACK);
        iconX -= iconSpacing;
    }

    // Backlight icon (sun)
    if ((statusIcons_ & StatusIcon::BACKLIGHT) != StatusIcon::NONE) {
        gfx->fillCircle(iconX + 4, y + 5, 2, EPD_BLACK);
        for (int i = 0; i < 8; i++) {
            int angle = i * 45;
            int dx = (angle == 0 || angle == 180) ? 4 : (angle == 90 || angle == 270) ? 0 : 3;
            int dy = (angle == 90 || angle == 270) ? 4 : (angle == 0 || angle == 180) ? 0 : 3;
            if (angle > 90 && angle < 270) dx = -dx;
            if (angle > 0 && angle < 180) dy = -dy;
            gfx->drawPixel(iconX + 4 + dx, y + 5 + dy, EPD_BLACK);
        }
        iconX -= iconSpacing;
    }

    // Sleep icons
    if ((statusIcons_ & StatusIcon::DEEP_SLEEP) != StatusIcon::NONE) {
        gfx->setCursor(iconX, y + 2);
        gfx->print("zzZ");
        iconX -= iconSpacing + 10;
    } else if ((statusIcons_ & StatusIcon::LIGHT_SLEEP) != StatusIcon::NONE) {
        gfx->setCursor(iconX, y + 2);
        gfx->print("z");
        iconX -= iconSpacing;
    }

    // Caffeinated icon (sleep inhibited) - coffee cup
    if ((statusIcons_ & StatusIcon::CAFFEINATED) != StatusIcon::NONE) {
        int cx = iconX, cy = y;
        // Cup body
        gfx->drawRect(cx, cy + 3, 8, 7, EPD_BLACK);
        // Cup handle
        gfx->drawLine(cx + 8, cy + 4, cx + 10, cy + 4, EPD_BLACK);
        gfx->drawLine(cx + 10, cy + 4, cx + 10, cy + 8, EPD_BLACK);
        gfx->drawLine(cx + 8, cy + 8, cx + 10, cy + 8, EPD_BLACK);
        // Steam (wavy lines)
        gfx->drawPixel(cx + 2, cy + 1, EPD_BLACK);
        gfx->drawPixel(cx + 3, cy, EPD_BLACK);
        gfx->drawPixel(cx + 5, cy + 1, EPD_BLACK);
        gfx->drawPixel(cx + 6, cy, EPD_BLACK);
        iconX -= iconSpacing;
    }

    // Background plugin running - play triangle inside a square frame
    if ((statusIcons_ & StatusIcon::BACKGROUND) != StatusIcon::NONE) {
        int bx = iconX, by = y;
        gfx->drawRect(bx, by + 1, 11, 11, EPD_BLACK);
        const int cy = by + 6;
        for (int row = 0; row <= 8; row++) {
            int rowY = by + 2 + row;
            int r = rowY > cy ? rowY - cy : cy - rowY;
            int rightX = bx + 8 - r;
            if (rightX >= bx + 3) {
                gfx->drawLine(bx + 3, rowY, rightX, rowY, EPD_BLACK);
            }
        }
        iconX -= iconSpacing;
    }
    (void)iconX;
}

/**
 * \brief Renders a minimal transition screen and blocks until the refresh ends.
 * \param mode Footer mode selecting the transition hint (deep sleep / ship mode).
 */
void LockScreenView::renderTransitionScreen(FooterMode mode) {
    footerMode_ = mode;

    // Clear clock and status icons for minimal screen
    setClock("");
    setDate("");
    statusIcons_ = StatusIcon::NONE;

    // Render normal lockscreen (with transition footer) and push to display
    render(false);

    auto* display = hal::getDisplayInstance();
    if (display) {
        display->flushSync(hal::RefreshMode::PARTIAL);
    }
}

/**
 * \brief Renders and flushes dedicated deep-sleep transition screen.
 */
void LockScreenView::renderDeepSleepScreen() {
    renderTransitionScreen(FooterMode::DEEP_SLEEP);
}

/**
 * \brief Renders and flushes dedicated ship-mode transition screen.
 */
void LockScreenView::renderShipModeScreen() {
    renderTransitionScreen(FooterMode::SHIP_MODE);
}

/**
 * \brief Renders complete lock-screen layout.
 * \param partial `true` for partial redraw, `false` for full redraw.
 */
void LockScreenView::render(bool partial) {
    if (preRenderCb_) {
        preRenderCb_();
    }

    hal::IDisplay* display = hal::getDisplayInstance();
    if (!display) return;

    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;

    if (!partial) {
        gfx->fillScreen(EPD_WHITE);
    }

    gfx->setTextColor(EPD_BLACK);

    // === Top Left: Clock ===
    gfx->setFont(nullptr);
    gfx->setTextSize(2);  // Size 2 for better fit
    gfx->setCursor(5, CLOCK_Y);
    gfx->print(clock_);

    // === Below clock: Date ===
    gfx->setTextSize(1);
    gfx->setCursor(5, DATE_Y);
    render::printText(gfx, date_);

    // === Top Right: Battery ===
    renderBattery(gfx, BATTERY_X, BATTERY_Y);

    // === Right of battery: Status icons ===
    renderStatusIcons(gfx, BATTERY_X - 20, ICONS_Y);

    constexpr int FIT_BUDGET = DISPLAY_WIDTH - 10;

    auto fitAndDraw = [&](const char* text, int baselineY,
                          const GFXfont* const* candidates, size_t count) {
        if (!text || !text[0]) return;
        const GFXfont* f = cdc::ui::render::pickFontThatFits(
            gfx, text, FIT_BUDGET, candidates, count, true);
        int16_t x1, y1;
        uint16_t w = 0, h = 0;
        cdc::ui::render::measureText(gfx, text, f, 0, 0, &x1, &y1, &w, &h);
        int x = (display->getWidth() - w) / 2;
        gfx->setCursor(x, baselineY);
        cdc::ui::render::drawText(gfx, text, f);
    };

    using cdc::ui::FontId;
    static const GFXfont* const NAME_FONTS[] = {
        cdc::ui::getGfxFont(FontId::Bold12pt),
        cdc::ui::getGfxFont(FontId::Bold9pt),
        cdc::ui::getGfxFont(FontId::Builtin),
    };
    static const GFXfont* const INFO_FONTS[] = {
        cdc::ui::getGfxFont(FontId::Bold9pt),
        cdc::ui::getGfxFont(FontId::Builtin),
    };

    // === Center: Name (size 3 = 12pt, fallback to smaller) ===
    fitAndDraw(name_,  NAME_Y,  NAME_FONTS, std::size(NAME_FONTS));

    // === Info line 1 (size 2 = 9pt, fallback to 1) ===
    fitAndDraw(info_,  INFO_Y,  INFO_FONTS, std::size(INFO_FONTS));

    // === Info line 2 (size 2 = 9pt, fallback to 1) ===
    fitAndDraw(info2_, INFO2_Y, INFO_FONTS, std::size(INFO_FONTS));

    // === Bottom: Footer hint (size 1 = built-in 6x8) ===
    gfx->setFont(nullptr);
    gfx->setTextSize(1);
    const char* hint = getFooterHint();
    if (hint && hint[0]) {
        int16_t x1, y1;
        uint16_t w, h;
        gfx->getTextBounds(hint, 0, 0, &x1, &y1, &w, &h);
        int hintX = (display->getWidth() - w) / 2;
        gfx->setCursor(hintX, display->getHeight() - 10);
        render::printText(gfx, hint);
    }

    dirty_ = false;
}

} // namespace cdc::ui
