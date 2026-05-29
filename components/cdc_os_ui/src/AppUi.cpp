/**
 * \file
 * \brief Core App UI setup including lock flow, menus, and status icon updates.
 *
 * Feature-specific menu logic lives in dedicated compilation units:
 * - WifiMenuUi.cpp
 * - BluetoothMenuUi.cpp
 * - ExpertMenuUi.cpp
 */

#include "AppUiInternal.h"
#include "cdc_os_ui/AppUi.h"
#include "cdc_os_ui/views/LockScreenView.h"
#include "cdc_os_ui/views/PinChangeView.h"
#include "cdc_os_ui/views/BlePairingPromptView.h"
#include "cdc_os_ui/WifiHandlers.h"
#include "cdc_os_ui/SettingsHandlers.h"
#include "cdc_os_ui/SleepManager.h"
#include "cdc_os_ui/HardwareInfo.h"
#include "cdc_core/PinManager.h"
#include "cdc_core/TropicSlotMap.h"
#include "cdc_core/TropicStorage.h"
#include "cdc_core/UsbManager.h"
#include "cdc_core/Raii.h"

#include "cdc_views/SliderView.h"
#include "cdc_views/PinEntryView.h"
#include "cdc_views/DateInputView.h"
#include "cdc_views/TimeInputView.h"
#include "plugin_manager/PluginListView.h"
#include "plugin_manager/PluginManager.h"

#include "cdc_hal/IDisplay.h"
#include "cdc_hal/IKeypad.h"
#include "cdc_hal/IPowerManager.h"
#include "cdc_hal/ISleepController.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_hal/IWifiController.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_hal/IRtc.h"

#include "serial_cmd/SerialCmd.h"
#include "nvs.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

namespace cdc::ui {

/** \brief Menu sizing and inactivity timeout constants. */

static constexpr uint8_t MAIN_MENU_MAX_ITEMS = 16;
static constexpr uint8_t MAIN_MENU_FIXED_COUNT = 3;  // Plugins + Tools + Settings
static constexpr uint8_t TOOLS_FIXED_COUNT = 4;       // Modules, WiFi, Bluetooth, Expert
static constexpr uint8_t TOOLS_MAX_ITEMS = 16;

static constexpr uint32_t INACTIVITY_TIMEOUT_MS = 5 * 60 * 1000;

/** \brief Index enums for fixed settings and language menus. */

enum SettingsMenuIdx {
    SETTINGS_IDX_BRIGHTNESS = 0,
    SETTINGS_IDX_LANGUAGE,
    SETTINGS_IDX_TIMEZONE,
    SETTINGS_IDX_AUTO_SLEEP,
    SETTINGS_IDX_BADGE_TEXT,
    SETTINGS_IDX_SET_DATE,
    SETTINGS_IDX_SET_TIME,
    SETTINGS_IDX_CHANGE_PIN,
    SETTINGS_IDX_COUNT
};

enum LanguageMenuIdx {
    LANG_IDX_ENGLISH = 0,
    LANG_IDX_GERMAN,
    LANG_IDX_COUNT
};

/** \brief Static UI state and lazily constructed view pointers. */
static LockScreenView* s_lockScreen = nullptr;
static PinEntryView* s_pinEntry = nullptr;
static ListView* s_mainMenu = nullptr;
static ListView* s_toolsMenu = nullptr;
static ListView* s_settingsMenu = nullptr;
static SliderView* s_brightnessSlider = nullptr;
static SliderView* s_sleepSlider = nullptr;
static SliderView* s_timezoneSlider = nullptr;
static ListView* s_languageMenu = nullptr;
static DateInputView* s_dateInput = nullptr;
static TimeInputView* s_timeInput = nullptr;
static PinChangeView* s_pinChangeView = nullptr;
static BlePairingPromptView* s_pairingPrompt = nullptr;
static cdc::plugin_manager::PluginListView* s_pluginListView = nullptr;

// Numeric-comparison pairing is requested from the nimble_host task; the prompt
// must be shown from the main/UI task. The request is parked here under a mutex
// and the UI work is deferred via BLE_PAIRING_REQUEST.
struct PendingPairing {
    bool     valid = false;
    uint16_t connHandle = 0xFFFF;
    uint32_t passkey = 0;
};
static PendingPairing s_pendingPairing;
static SemaphoreHandle_t s_blePendingMutex = nullptr;

/** \brief Runtime dependencies provided during `ui_init`. */
static UiDeps s_deps = {};

/** \brief Main-menu backing storage for module and fixed menu entries. */
static ListItem s_mainMenuItems[MAIN_MENU_MAX_ITEMS];
static core::ModuleMenuItem s_mainMenuModuleItems[MAIN_MENU_MAX_ITEMS];
static uint8_t s_mainMenuPluginCount = 0;

/** \brief Tools-menu backing storage for fixed and module entries. */
static ListItem s_toolsItems[TOOLS_MAX_ITEMS];
static core::ModuleMenuItem s_toolsModuleItems[TOOLS_MAX_ITEMS];
static uint8_t s_toolsModuleCount = 0;

/** \brief Settings menu backing storage. */
static ListItem s_settingsItems[SETTINGS_IDX_COUNT];

/** \brief Language menu backing storage. */
static ListItem s_languageItems[LANG_IDX_COUNT];

/** \brief Last rendered minute for lock-screen clock throttling. */
static int8_t s_lastMinute = -1;

/** \brief Last known status-icon inputs to avoid redundant updates. */
static bool s_lastUsbConnected = false;
static bool s_lastCharging = false;
static bool s_lastWifiConnected = false;
static bool s_lastBleEnabled = false;
static bool s_lastBatteryPresent = false;

// Throttle for the battery-percent ADC sample on the lockscreen. The BQ25895
// ADC step is 20 mV, mapped over 1000 mV (3200-4200 mV) the linear curve gives
// ~2 %/step; an unrestricted per-tick sample makes ADC jitter flip the
// rendered percentage, marking the view dirty and triggering a partial EPD
// refresh on essentially every tick.
static constexpr uint32_t BATTERY_SAMPLE_INTERVAL_MS = 30000;
static uint32_t s_lastBatterySampleMs = 0;

/** \brief Prevents stale key events directly after unlock transition. */
static bool s_ignoreKeyUntilRelease = false;

/** \brief Returns main-menu index of the fixed "Plugins" item. */
static inline uint8_t getPluginsIndex()  { return s_mainMenuPluginCount; }
/** \brief Returns main-menu index of the fixed "Tools" item. */
static inline uint8_t getToolsIndex()    { return s_mainMenuPluginCount + 1; }
/** \brief Returns main-menu index of the fixed "Settings" item. */
static inline uint8_t getSettingsIndex() { return s_mainMenuPluginCount + 2; }
/** \brief Returns effective main-menu item count including fixed entries. */
static inline uint8_t getMainMenuCount() { return s_mainMenuPluginCount + MAIN_MENU_FIXED_COUNT; }

/** \brief Starts unlock flow from lock screen. */
static void onUnlockRequested();
/** \brief Verifies entered PIN via PinManager. */
static bool onPinVerify(const char* pin);
/** \brief Handles successful unlock and transitions to main menu. */
static void onPinSuccess();
/** \brief Handles main-menu item selection. */
static void onMainMenuSelect(uint16_t index, void* userData);
/** \brief Handles tools-menu item selection. */
static void onToolsSelect(uint16_t index, void* userData);
/** \brief Handles settings-menu item selection. */
static void onSettingsSelect(uint16_t index, void* userData);
/** \brief Handles language-menu item selection. */
static void onLanguageSelect(uint16_t index, void* userData);
/** \brief Rebuilds labels for translatable menus after language change. */
static void rebuildMenuLabels();
/** \brief Callback invoked when inactivity timeout is reached. */
static void onInactivityTimeout();
/** \brief Drains buffered keypad events. */
static void clearKeypadBuffer();

/**
 * \brief Draws RSSI signal bars using the shared lock-screen visual style.
 * \param gfx Display graphics context.
 * \param x Left position.
 * \param y Top reference position.
 * \param rssi Signal strength in dBm.
 * \param inverted Whether to draw inverted colors.
 */
void drawSignalBars(Gdey029T94* gfx, int x, int y, int8_t rssi, bool inverted) {
    if (!gfx) return;

    int bars;
    if (rssi > -50) bars = 4;
    else if (rssi > -60) bars = 3;
    else if (rssi > -70) bars = 2;
    else bars = 1;

    uint16_t fg = inverted ? EPD_WHITE : EPD_BLACK;

    int barWidth = 3;
    int gap = 1;
    int baseY = y + 13;

    for (int i = 0; i < 4; i++) {
        int barHeight = 4 + i * 3;
        int bx = x + i * (barWidth + gap);
        int by = baseY - barHeight;

        if (i < bars) {
            gfx->fillRect(bx, by, barWidth, barHeight, fg);
        } else {
            gfx->drawRect(bx, by, barWidth, barHeight, fg);
        }
    }
}

/**
 * \brief Updates a single boolean-driven status icon on the lock screen.
 *
 * Adds the icon when transitioning to active, removes it when transitioning
 * to inactive, and updates the cached previous state. Performs no work when
 * the state is unchanged.
 *
 * \param icon Status icon identifier to toggle.
 * \param active Current desired active state.
 * \param last Reference to cached previous state, updated on change.
 */
static void updateStatusIcon(StatusIcon icon, bool active, bool& last) {
    if (active == last) return;
    if (active) {
        s_lockScreen->addStatusIcon(icon);
    } else {
        s_lockScreen->removeStatusIcon(icon);
    }
    last = active;
}

/**
 * \brief Updates the battery percentage indicator on the lock screen.
 *
 * Pushes the current percentage when a battery is present, otherwise sets
 * the indicator to the no-battery sentinel value (-1).
 */
static void updateBatteryIndicator() {
    bool present = s_deps.power->isBatteryPresent();
    uint32_t nowMs = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);

    if (present != s_lastBatteryPresent) {
        if (present) {
            s_lockScreen->removeStatusIcon(StatusIcon::NO_BATTERY);
            s_lockScreen->setBatteryPercent(s_deps.power->getBatteryPercent());
        } else {
            s_lockScreen->addStatusIcon(StatusIcon::NO_BATTERY);
            s_lockScreen->setBatteryPercent(0);
        }
        s_lastBatteryPresent = present;
        s_lastBatterySampleMs = nowMs;
        return;
    }

    if (!present) return;

    if (s_lastBatterySampleMs != 0 &&
        (nowMs - s_lastBatterySampleMs) < BATTERY_SAMPLE_INTERVAL_MS) {
        return;
    }
    s_lastBatterySampleMs = nowMs;
    s_lockScreen->setBatteryPercent(s_deps.power->getBatteryPercent());
}

/**
 * \brief Synchronizes lock-screen status icons with current hardware state.
 */
void updatePowerStatusIcons() {
    if (!s_lockScreen || !s_deps.power) return;

    const bool usbConnected = s_deps.power->isUsbConnected();
    const hal::ChargeStatus chargeStatus = s_deps.power->getChargeStatus();
    const bool charging = (chargeStatus == hal::ChargeStatus::FAST_CHARGE ||
                           chargeStatus == hal::ChargeStatus::PRE_CHARGE);

    auto* wifi = hal::getWifiControllerInstance();
    const bool wifiConnected = wifi && wifi->isConnected();

    auto* ble = hal::getBluetoothControllerInstance();
    const bool bleEnabled = ble && ble->isEnabled();

    updateStatusIcon(StatusIcon::USB, usbConnected, s_lastUsbConnected);
    updateStatusIcon(StatusIcon::CHARGING, charging, s_lastCharging);
    updateStatusIcon(StatusIcon::WIFI, wifiConnected, s_lastWifiConnected);
    updateStatusIcon(StatusIcon::BLE, bleEnabled, s_lastBleEnabled);

    updateBatteryIndicator();
}

/**
 * \brief Clears pending keypad key events.
 */
static void clearKeypadBuffer() {
    if (!s_deps.keypad) return;
    while (s_deps.keypad->getNextKey() != hal::Key::KEY_NONE) {}
}

/**
 * \brief Updates lock-screen clock/date once per minute while lock screen is visible.
 */
static void updateLockScreenClock() {
    if (!s_lockScreen) return;
    if (ViewStack::instance().current() != s_lockScreen) return;

    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    if (!t || t->tm_min == s_lastMinute) return;

    s_lastMinute = t->tm_min;
    char buf[40];
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    s_lockScreen->setClock(buf);
    snprintf(buf, sizeof(buf), "%02d.%02d.%04d", t->tm_mday, t->tm_mon + 1, t->tm_year + 1900);
    s_lockScreen->setDate(buf);
}

/**
 * \brief Handles lock-screen unlock request and opens PIN entry when required.
 */
static void onUnlockRequested() {
    clearKeypadBuffer();
    s_ignoreKeyUntilRelease = true;

    if (s_pinEntry) {
        s_pinEntry->clear();
        ViewStack::instance().push(s_pinEntry);
    }
}

/**
 * \brief Verifies entered badge PIN against secure PinManager state.
 * \param pin Null-terminated PIN text.
 * \return `true` if PIN is valid, otherwise `false`.
 */
static bool onPinVerify(const char* pin) {
    return core::PinManager::instance().verifyBadgePin(pin);
}

/**
 * \brief Handles successful unlock, switches to main menu, and dispatches unlock hooks.
 */
static void onPinSuccess() {
    ViewStack::instance().replace(s_mainMenu);
    core::ModuleRegistry::instance().dispatchUnlock();
    if (s_deps.display) s_deps.display->backlightOn();
}

/**
 * \brief Locks UI back to root lock-screen state on inactivity timeout.
 */
static void onInactivityTimeout() {
    // Lock screen
    while (ViewStack::instance().depth() > 1) {
        ViewStack::instance().pop();
    }
    s_ignoreKeyUntilRelease = true;
    clearKeypadBuffer();
}

/**
 * \brief Rebuilds main menu entries including dynamically provided modules.
 */
void rebuildMainMenu() {
    auto& moduleReg = core::ModuleRegistry::instance();

    s_mainMenuPluginCount = moduleReg.getMenuItems(
        core::MenuLocation::MAIN_MENU,
        s_mainMenuModuleItems,
        MAIN_MENU_MAX_ITEMS - MAIN_MENU_FIXED_COUNT
    );

    for (uint8_t i = 0; i < s_mainMenuPluginCount; i++) {
        s_mainMenuItems[i] = {s_mainMenuModuleItems[i].label, 0, false, nullptr};
    }

    s_mainMenuItems[getPluginsIndex()]  = {ui::tr("core.plugins"),  0, false, nullptr};
    s_mainMenuItems[getToolsIndex()]    = {ui::tr("core.tools"),    0, false, nullptr};
    s_mainMenuItems[getSettingsIndex()] = {ui::tr("core.settings"), 0, false, nullptr};

    if (s_mainMenu) {
        s_mainMenu->init(ui::tr("core.main_menu"), s_mainMenuItems, getMainMenuCount());
    }
}

/**
 * \brief Rebuilds tools menu entries including dynamic module tools.
 */
void rebuildToolsMenu() {
    // Fixed items
    s_toolsItems[0] = {ui::tr("core.modules"), 0, false, nullptr};
    s_toolsItems[1] = {ui::tr("core.wifi_menu"), 0, false, nullptr};
    auto* ble = hal::getBluetoothControllerInstance();
    s_toolsItems[2] = {ui::tr("core.bluetooth"), static_cast<uint8_t>(ble && ble->isEnabled() ? '*' : 0), false, nullptr};
    s_toolsItems[3] = {ui::tr("core.expert"), 0, false, nullptr};

    auto& moduleReg = core::ModuleRegistry::instance();
    s_toolsModuleCount = moduleReg.getMenuItems(
        core::MenuLocation::TOOLS_MENU,
        s_toolsModuleItems,
        TOOLS_MAX_ITEMS - TOOLS_FIXED_COUNT
    );

    for (uint8_t i = 0; i < s_toolsModuleCount; i++) {
        s_toolsItems[TOOLS_FIXED_COUNT + i] = {s_toolsModuleItems[i].label, 0, false, nullptr};
    }

    if (s_toolsMenu) {
        s_toolsMenu->init(ui::tr("core.tools"), s_toolsItems, TOOLS_FIXED_COUNT + s_toolsModuleCount);
    }
}

/**
 * \brief Rebuilds all translatable menu labels and refreshes visible menus.
 */
static void rebuildMenuLabels() {
    // Settings items
    s_settingsItems[SETTINGS_IDX_BRIGHTNESS] = {ui::tr("core.brightness"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_LANGUAGE] = {ui::tr("core.language"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_TIMEZONE] = {ui::tr("core.timezone"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_AUTO_SLEEP] = {ui::tr("core.auto_sleep"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_BADGE_TEXT] = {ui::tr("core.badge_text"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_SET_DATE] = {ui::tr("core.set_date"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_SET_TIME] = {ui::tr("core.set_time"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_CHANGE_PIN] = {ui::tr("core.change_pin"), 0, false, nullptr};

    if (s_settingsMenu) {
        s_settingsMenu->init(ui::tr("core.settings"), s_settingsItems, SETTINGS_IDX_COUNT);
    }

    rebuildMainMenu();
    rebuildToolsMenu();
}

/**
 * \brief Handles main-menu selection and dispatches configured destination views.
 * \param index Selected item index.
 * \param userData Optional callback user data.
 */
static void onMainMenuSelect(uint16_t index, void* userData) {
    (void)userData;

    // Module items first
    if (index < s_mainMenuPluginCount) {
        auto& item = s_mainMenuModuleItems[index];
        if (item.getView) {
            IView* view = item.getView();
            if (view) ViewStack::instance().push(view);
        }
        return;
    }

    if (index == getPluginsIndex()) {
        if (!s_pluginListView) s_pluginListView = new cdc::plugin_manager::PluginListView();
        ViewStack::instance().push(s_pluginListView);
    } else if (index == getToolsIndex()) {
        ViewStack::instance().push(s_toolsMenu);
    } else if (index == getSettingsIndex()) {
        ViewStack::instance().push(s_settingsMenu);
    }
}

/**
 * \brief Handles tools-menu selection and routes to fixed or module-defined views.
 * \param index Selected item index.
 * \param userData Optional callback user data.
 */
static void onToolsSelect(uint16_t index, void* userData) {
    (void)userData;

    switch (index) {
        case 0: showModulesView(); return;
        case 1: showWifiMainMenu(); return;
        case 2: showBluetoothMenu(); return;
        case 3: showExpertMenu(); return;
    }

    uint8_t moduleIdx = index - TOOLS_FIXED_COUNT;
    if (moduleIdx < s_toolsModuleCount) {
        auto& item = s_toolsModuleItems[moduleIdx];
        if (item.getView) {
            IView* view = item.getView();
            if (view) ViewStack::instance().push(view);
        }
    }
}

/**
 * \brief Handles settings-menu actions.
 * \param index Selected settings item index.
 * \param userData Optional callback user data.
 */
static void onSettingsSelect(uint16_t index, void* userData) {
    (void)userData;

    switch (index) {
        case SETTINGS_IDX_BRIGHTNESS:
            ViewStack::instance().push(s_brightnessSlider);
            break;
        case SETTINGS_IDX_LANGUAGE:
            ViewStack::instance().push(s_languageMenu);
            break;
        case SETTINGS_IDX_TIMEZONE:
            ViewStack::instance().push(s_timezoneSlider);
            break;
        case SETTINGS_IDX_AUTO_SLEEP:
            ViewStack::instance().push(s_sleepSlider);
            break;
        case SETTINGS_IDX_BADGE_TEXT:
            settings::startBadgeTextEdit();
            break;
        case SETTINGS_IDX_SET_DATE:
            ViewStack::instance().push(s_dateInput);
            break;
        case SETTINGS_IDX_SET_TIME:
            ViewStack::instance().push(s_timeInput);
            break;
        case SETTINGS_IDX_CHANGE_PIN:
            if (s_pinChangeView) {
                s_pinChangeView->init(core::PinManager::BADGE_PIN_MIN, core::PinManager::BADGE_PIN_MAX);
                ViewStack::instance().push(s_pinChangeView);
            }
            break;
    }
}

/**
 * \brief Applies new UI language and rebuilds translated menus.
 * \param index Selected language item index.
 * \param userData Optional callback user data.
 */
static void onLanguageSelect(uint16_t index, void* userData) {
    (void)userData;

    const char* code = "en";
    switch (index) {
        case LANG_IDX_ENGLISH: code = "en"; break;
        case LANG_IDX_GERMAN:  code = "de"; break;
    }
    I18n::instance().setLanguageCode(code);
    cdc::plugin_manager::PluginManager::instance().reloadActiveLangOverlay();
    ViewStack::instance().pop();
}

/**
 * \brief Returns whether the badge is currently locked (showing lock screen with no menu above).
 * \return `true` if the lock screen is the only view on the stack.
 */
static bool isBadgeLocked() {
    return ViewStack::instance().depth() <= 1 &&
           ViewStack::instance().current() == s_lockScreen;
}

/**
 * \brief Numeric-comparison pairing request, invoked on the nimble_host task.
 * \param connHandle BLE connection handle being paired.
 * \param passkey Six-digit confirmation code shown by the remote host.
 *
 * Touches no UI: parks the request under s_blePendingMutex and defers the prompt
 * to the main task via BLE_PAIRING_REQUEST. A second request arriving before the
 * first is consumed is rejected inline.
 */
static void onBleNumericComparison(uint16_t connHandle, uint32_t passkey) {
    auto* ble = hal::getBluetoothControllerInstance();

    bool dropped = false;
    {
        core::MutexGuard guard(s_blePendingMutex);
        if (s_pendingPairing.valid) {
            dropped = true;
        } else {
            s_pendingPairing.connHandle = connHandle;
            s_pendingPairing.passkey = passkey;
            s_pendingPairing.valid = true;
        }
    }
    if (dropped) {
        if (ble) ble->respondToNumericComparison(connHandle, false);
        return;
    }

    if (!core::EventBus::instance().publish(core::EventType::BLE_PAIRING_REQUEST)) {
        {
            core::MutexGuard guard(s_blePendingMutex);
            s_pendingPairing.valid = false;
        }
        if (ble) ble->respondToNumericComparison(connHandle, false);
    }
}

/**
 * \brief Main-task handler for a deferred numeric-comparison pairing request.
 * \param evt Unused event payload; the request is read from s_pendingPairing.
 *
 * If the badge is locked the request is rejected without prompting. Otherwise a
 * modal pairing prompt is shown; ui_process renders it on the next pass.
 */
static void onBlePairingRequestEvent(const core::Event& evt) {
    (void)evt;
    PendingPairing req;
    {
        core::MutexGuard guard(s_blePendingMutex);
        req = s_pendingPairing;
        s_pendingPairing.valid = false;
    }
    if (!req.valid) return;

    auto* ble = hal::getBluetoothControllerInstance();
    if (isBadgeLocked()) {
        if (ble) ble->respondToNumericComparison(req.connHandle, false);
        return;
    }
    if (!s_pairingPrompt) {
        s_pairingPrompt = new BlePairingPromptView();
    }
    s_pairingPrompt->prepare(req.connHandle, req.passkey);
    ViewStack::instance().showModal(s_pairingPrompt);
}

/**
 * \brief Initializes App UI, builds all core views, and wires callbacks.
 * \param deps Hardware/service dependencies used by the UI runtime.
 */
void ui_init(const UiDeps& deps) {
    s_deps = deps;

    // Initialize I18n
    I18n::instance().init();

    // Refresh menu labels whenever the active translation table changes,
    // so item label pointers stay in sync with the loaded overlay.
    I18n::instance().setOnLanguageChanged([]() {
        ui_rebuild_menus();
    });

    // Create LockScreen
    s_lockScreen = new LockScreenView();
    s_lockScreen->init();

    if (s_deps.keypad) {
        s_deps.keypad->setLongPressEnabled(true, 800);
        s_deps.keypad->setLongPressCallback([](hal::Key key) {
            char keyChar = static_cast<char>(key);
            ViewStack::instance().dispatchLongPress(keyChar);
        });
    }

    // Load display texts from NVS
    {
        nvs_handle_t nvs;
        char buf[64];
        size_t len;

        if (nvs_open("display", NVS_READONLY, &nvs) == ESP_OK) {
            len = sizeof(buf);
            if (nvs_get_str(nvs, "name", buf, &len) == ESP_OK && len > 1) {
                s_lockScreen->setDisplayName(buf);
            } else {
                s_lockScreen->setDisplayName(ui::tr("core.default_name"));
            }

            len = sizeof(buf);
            if (nvs_get_str(nvs, "info", buf, &len) == ESP_OK && len > 1) {
                s_lockScreen->setInfo(buf);
            } else {
                s_lockScreen->setInfo(ui::tr("core.default_info"));
            }

            len = sizeof(buf);
            if (nvs_get_str(nvs, "info2", buf, &len) == ESP_OK && len > 1) {
                s_lockScreen->setInfo2(buf);
            }

            nvs_close(nvs);
        } else {
            s_lockScreen->setDisplayName(ui::tr("core.default_name"));
            s_lockScreen->setInfo(ui::tr("core.default_info"));
        }
    }

    s_lockScreen->setOnUnlock(onUnlockRequested);
    s_lockScreen->setPreRenderCallback([]() {
        if (s_deps.power) {
            s_deps.power->refresh();
        }
        updatePowerStatusIcons();
    });

    if (!core::TropicSlotMap::instance().isValid()) {
        const char* msg = core::TropicSlotMap::instance().errorMessage();
        showToastAlertSticky(msg ? msg : "Slot map invalid");
    }

    // Set initial clock/date from RTC
    {
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        if (t) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
            s_lockScreen->setClock(buf);
            snprintf(buf, sizeof(buf), "%02d.%02d.%04d", t->tm_mday, t->tm_mon + 1, t->tm_year + 1900);
            s_lockScreen->setDate(buf);
            s_lastMinute = t->tm_min;
        }
    }

    // Set initial battery/charging status
    if (s_deps.power) {
        s_lockScreen->setBatteryPercent(s_deps.power->getBatteryPercent());
        if (s_deps.power->getChargeStatus() == hal::ChargeStatus::FAST_CHARGE ||
            s_deps.power->getChargeStatus() == hal::ChargeStatus::PRE_CHARGE) {
            s_lockScreen->addStatusIcon(StatusIcon::CHARGING);
        }
        if (s_deps.power->isUsbConnected()) {
            s_lockScreen->addStatusIcon(StatusIcon::USB);
        }
        s_lastUsbConnected = s_deps.power->isUsbConnected();
        s_lastCharging = (s_deps.power->getChargeStatus() == hal::ChargeStatus::FAST_CHARGE ||
                          s_deps.power->getChargeStatus() == hal::ChargeStatus::PRE_CHARGE);
    }

    // Create PinEntryView
    s_pinEntry = new PinEntryView();
    s_pinEntry->init(ui::tr("core.enter_pin"), 8, 3);
    s_pinEntry->setOnVerify(onPinVerify);
    s_pinEntry->setOnSuccess(onPinSuccess);

    // Create Main Menu
    s_mainMenu = new ListView();
    s_mainMenu->setOnSelect(onMainMenuSelect);

    // Tools Menu
    s_toolsMenu = new ListView();
    s_toolsMenu->setOnSelect(onToolsSelect);

    // Build menus with module items
    rebuildMainMenu();
    rebuildToolsMenu();

    // Settings Menu
    s_settingsItems[SETTINGS_IDX_BRIGHTNESS] = {ui::tr("core.brightness"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_LANGUAGE] = {ui::tr("core.language"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_TIMEZONE] = {ui::tr("core.timezone"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_AUTO_SLEEP] = {ui::tr("core.auto_sleep"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_BADGE_TEXT] = {ui::tr("core.badge_text"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_SET_DATE] = {ui::tr("core.set_date"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_SET_TIME] = {ui::tr("core.set_time"), 0, false, nullptr};
    s_settingsItems[SETTINGS_IDX_CHANGE_PIN] = {ui::tr("core.change_pin"), 0, false, nullptr};
    s_settingsMenu = new ListView();
    s_settingsMenu->init(ui::tr("core.settings"), s_settingsItems, SETTINGS_IDX_COUNT);
    s_settingsMenu->setOnSelect(onSettingsSelect);

    // Initialize settings handlers
    settings::init(s_deps.display, s_deps.sleep, s_lockScreen);

    // Brightness Slider
    s_brightnessSlider = new SliderView();
    uint16_t currentBrightness = s_deps.display ? s_deps.display->getBacklight() / 10 : 50;
    s_brightnessSlider->init(ui::tr("core.brightness"), 0, 100, currentBrightness, 1, "%");
    s_brightnessSlider->setStepCallback(settings::brightnessStepCallback);
    s_brightnessSlider->setOnSave(settings::onBrightnessSave);
    s_brightnessSlider->setOnChange(settings::onBrightnessChange);

    // Auto Sleep Slider
    s_sleepSlider = new SliderView();
    uint16_t currentSleepMin = 0;
    if (s_deps.sleep) {
        currentSleepMin = static_cast<uint16_t>(s_deps.sleep->getLightSleepInterval() / 60);
    }
    s_sleepSlider->init(ui::tr("core.auto_sleep"), 0, 60, currentSleepMin, 1, ui::tr("core.minutes"));
    s_sleepSlider->setZeroLabel(ui::tr("core.never"));
    s_sleepSlider->setOnSave(settings::onSleepIntervalSave);

    // Timezone Slider
    s_timezoneSlider = new SliderView();
    {
        auto* rtcTz = hal::getRtcInstance();
        int8_t currentTz = rtcTz ? rtcTz->getTimezoneOffset() : 0;
        uint16_t tzSliderValue = static_cast<uint16_t>(currentTz + 12);
        s_timezoneSlider->init(ui::tr("core.timezone"), 0, 26, tzSliderValue, 1, "h");
        s_timezoneSlider->setDisplayOffset(-12);
        s_timezoneSlider->setOnSave(settings::onTimezoneSave);
    }

    // Language Menu
    s_languageItems[LANG_IDX_ENGLISH] = {"English", 0, false, nullptr};
    s_languageItems[LANG_IDX_GERMAN]  = {"Deutsch", 0, false, nullptr};
    s_languageMenu = new ListView();
    s_languageMenu->init(ui::tr("core.language"), s_languageItems, LANG_IDX_COUNT);
    s_languageMenu->setOnSelect(onLanguageSelect);

    // Date/Time Input Views
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    s_dateInput = new DateInputView();
    s_dateInput->init(ui::tr("core.set_date"),
                      tm ? tm->tm_mday : 1,
                      tm ? tm->tm_mon + 1 : 1,
                      tm ? tm->tm_year + 1900 : 2026);
    s_dateInput->setOnConfirm(settings::onDateConfirm);

    s_timeInput = new TimeInputView();
    s_timeInput->init(ui::tr("core.set_time"),
                      tm ? tm->tm_hour : 12,
                      tm ? tm->tm_min : 0);
    s_timeInput->setOnConfirm(settings::onTimeConfirm);

    // PIN Change View
    s_pinChangeView = new PinChangeView();
    s_pinChangeView->init(core::PinManager::BADGE_PIN_MIN, core::PinManager::BADGE_PIN_MAX);
    s_pinChangeView->setOnComplete(settings::onPinChangeComplete);

    // Initialize PinManager
    core::PinManager::instance().init();

    // Initialize SleepManager
    SleepManager::instance().init(s_deps.sleep, s_deps.power, s_lockScreen);

    // Push to ViewStack
    ViewStack::instance().push(s_lockScreen);

    // Configure inactivity timeout
    ViewStack::instance().setInactivityTimeout(onInactivityTimeout, INACTIVITY_TIMEOUT_MS);

    // Subscribe to module error events
    core::EventBus::instance().subscribe(onModuleErrorEvent,
                                         core::EventBus::eventMask(core::EventType::MODULE_ERROR));

    // Central numeric-comparison pairing prompt: the request arrives on the
    // nimble_host task and is deferred to the main task for UI work.
    if (!s_blePendingMutex) {
        s_blePendingMutex = xSemaphoreCreateMutex();
    }
    core::EventBus::instance().subscribe(onBlePairingRequestEvent,
                                         core::EventBus::eventMask(core::EventType::BLE_PAIRING_REQUEST));
    if (auto* ble = hal::getBluetoothControllerInstance()) {
        ble->addNumericComparisonCallback(onBleNumericComparison);
    }

    // Serial callbacks
    serial::SerialCmd::setTextCallback([](const char* field, const char* value) {
        if (!s_lockScreen) return;
        // Serial text arrives already as CP437 (matches T9 and the display
        // pipeline), so it is stored verbatim.
        if (strcmp(field, "name") == 0) {
            s_lockScreen->setDisplayName(value);
            settings::saveDisplayField("name", value);
        } else if (strcmp(field, "info") == 0) {
            s_lockScreen->setInfo(value);
            settings::saveDisplayField("info", value);
        } else if (strcmp(field, "info2") == 0) {
            s_lockScreen->setInfo2(value);
            settings::saveDisplayField("info2", value);
        }
    });

    serial::SerialCmd::setTimeCallback([]() {
        if (!s_lockScreen) return;
        time_t now = time(nullptr);
        struct tm* tm = localtime(&now);
        if (tm) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
            s_lockScreen->setClock(buf);
            snprintf(buf, sizeof(buf), "%02d.%02d.%d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);
            s_lockScreen->setDate(buf);
        }
    });
}

/**
 * \brief Refreshes module-backed menus once module startup is complete.
 */
void ui_on_modules_ready() {
    rebuildToolsMenu();
    rebuildMainMenu();
}

/**
 * \brief Rebuilds dynamic UI menus.
 *
 * Re-runs all `tr()` lookups for settings, main and tools menus so that
 * label pointers reflect the currently active i18n overlay. Must be called
 * after the overlay has been (re)loaded.
 */
void ui_rebuild_menus() {
    rebuildMenuLabels();
}

/**
 * \brief Puts the badge into a quiet pre-reset state.
 *
 * Replaces lock-screen text with a clear "BOOTLOADER MODE" banner, drops
 * any open views/modals, forces a full EPD refresh and switches the
 * backlight off. The follow-up download-boot reset is triggered by the
 * caller from a separate worker task.
 */
void prepareForBootloaderReset() {
    if (s_lockScreen) {
        s_lockScreen->setDisplayName("BOOTLOADER MODE");
        s_lockScreen->setInfo("Awaiting flash...");
        s_lockScreen->setInfo2("Press RESET to resume");
        s_lockScreen->markDirty();
    }

    auto& stack = ViewStack::instance();
    stack.hideModal();
    while (stack.depth() > 1) {
        stack.pop();
    }

    if (s_lockScreen) {
        s_lockScreen->render(false);
        if (s_deps.display) {
            s_deps.display->flushSync(hal::RefreshMode::FULL);
            // Defensive wait in case the driver returns from flushSync()
            // before the panel busy line has fully deasserted.
            for (int i = 0; i < 50 && s_deps.display->isBusy(); ++i) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            s_deps.display->backlightOff();
        }
    }
}

/**
 * \brief Main UI tick: input processing, timeouts, status updates, and rendering.
 * \param nowMs Current monotonic time in milliseconds.
 */
void ui_process(uint32_t nowMs) {
    // Update status icons when on lock screen
    if (s_lockScreen && ViewStack::instance().current() == s_lockScreen) {
        updatePowerStatusIcons();
    }

    // Update clock at minute change
    updateLockScreenClock();

    // Keypad input
    if (s_deps.keypad) {
        if (s_ignoreKeyUntilRelease) {
            clearKeypadBuffer();
            if (!s_deps.keypad->anyKeyDown()) {
                s_ignoreKeyUntilRelease = false;
            }
        } else {
            hal::Key key = s_deps.keypad->getNextKey();
            if (key != hal::Key::KEY_NONE) {
                char keyChar = static_cast<char>(key);
                ViewStack::instance().dispatchKey(keyChar);
                SleepManager::instance().resetTimer(nowMs);
            }
        }
    }

    settings::processPendingBadgeText();

    nowMs = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    ViewStack::instance().dispatchTick(nowMs);
    if (ViewStack::instance().depth() > 1) {
        ViewStack::instance().checkInactivity(nowMs);
    } else {
        // On lock screen: check for light sleep
        SleepManager::instance().checkLockScreenSleep(nowMs);
    }

    // Render if needed
    if (ViewStack::instance().needsRender()) {
        ViewStack::instance().render();
    }
}

} // namespace cdc::ui
