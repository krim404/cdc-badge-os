#include "mod_blehid/BleHidModule.h"
#include "mod_blehid/BleHidKeyboard.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/ServiceRegistry.h"
#include "cdc_core/IKeyboardProvider.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ListView.h"
#include "cdc_views/ToastView.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_log.h"
#include <goodisplay/gdey029T94.h>
#include <cstring>

static const char* TAG = "HID";

namespace cdc::mod_blehid {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_blehid.title",           "BLE Keyboard"},
    {"mod_blehid.status",          "Status"},
    {"mod_blehid.start_adv",       "Start Advertising"},
    {"mod_blehid.stop_adv",        "Stop Advertising"},
    {"mod_blehid.unicode_method",  "Unicode Method"},
    {"mod_blehid.ascii_only",      "ASCII only"},
    {"mod_blehid.windows",         "Windows (Alt+Numpad)"},
    {"mod_blehid.linux",           "Linux (Ctrl+Shift+U)"},
    {"mod_blehid.macos",           "macOS (limited)"},
    {"mod_blehid.disconnect",      "Disconnect"},
};

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

/** \brief Status view showing BLE HID runtime state and selected input mode. */
class HidStatusView : public ui::ViewBase {
public:
    /** \brief Marks the view dirty on entry so status is rendered immediately. */
    void onEnter(void* context) override {
        (void)context;
        dirty_ = true;
    }

    /** \brief Triggers periodic redraw once per second. */
    void onTick(uint32_t nowMs) override {
        if (nowMs - lastUpdate_ >= 1000) {
            lastUpdate_ = nowMs;
            markDirty();
        }
    }

    /** \brief Renders keyboard status, connection state, and Unicode method. */
    void render(bool partial) override {
        auto* display = hal::getDisplayInstance();
        if (!display) return;

        auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
        if (!gfx) return;

        if (!partial) {
            gfx->fillScreen(EPD_WHITE);
        }

        auto& kb = BleHidKeyboard::instance();

        // Title
        gfx->setTextColor(EPD_BLACK);
        gfx->setTextSize(1);
        gfx->setCursor(8, 6);
        gfx->print(ui::tr("mod_blehid.title"));
        gfx->drawFastHLine(0, 22, display->getWidth(), EPD_BLACK);

        // Status
        gfx->setCursor(8, 30);
        gfx->print(ui::tr("mod_blehid.status"));
        gfx->print(": ");
        gfx->print(kb.getStatusText());

        // Connection indicator
        gfx->setCursor(8, 44);
        if (kb.isConnected()) {
            gfx->print("[CONNECTED]");
        } else if (kb.isAdvertising()) {
            gfx->print("[ADVERTISING]");
        } else {
            gfx->print("[IDLE]");
        }

        // Unicode method
        gfx->setCursor(8, 62);
        gfx->print(ui::tr("mod_blehid.unicode_method"));
        gfx->print(": ");
        switch (kb.getUnicodeMethod()) {
            case keyboard::UnicodeMethod::ASCII_ONLY:
                gfx->print(ui::tr("mod_blehid.ascii_only"));
                break;
            case keyboard::UnicodeMethod::WINDOWS:
                gfx->print("Windows");
                break;
            case keyboard::UnicodeMethod::LINUX:
                gfx->print("Linux");
                break;
            case keyboard::UnicodeMethod::MACOS:
                gfx->print("macOS");
                break;
        }

        clearDirty();
    }

    /**
     * \brief Handles key input in the status view.
     * \param key Input key code.
     * \return `REQUEST_POP` for `N`, otherwise `IGNORED`.
     */
    ui::InputResult onKey(char key) override {
        if (key == 'N') {
            return ui::InputResult::REQUEST_POP;
        }
        return ui::InputResult::IGNORED;
    }

    /** \brief Returns the internal view identifier. */
    const char* getName() const override { return "HidStatusView"; }
    /** \brief Returns footer hint text for available key actions. */
    const char* getFooterHint() const override { return "[N] Back"; }

private:
    uint32_t lastUpdate_ = 0;
};

/** \brief Static UI objects used by HID menu navigation. */
static ui::ListView s_menuView;
static ui::ListView s_unicodeMenu;
static HidStatusView s_statusView;
static bool s_viewsInitialized = false;

/**
 * \brief Rebuilds the top-level HID menu according to current keyboard state.
 */
static void rebuildMenu();
/**
 * \brief Handles top-level HID menu selection actions.
 * \param index Selected item index.
 * \param userData Optional callback user data.
 */
static void onMenuSelect(uint16_t index, void* userData);
/**
 * \brief Applies selected Unicode input method.
 * \param index Selected Unicode method index.
 * \param userData Optional callback user data.
 */
static void onUnicodeSelect(uint16_t index, void* userData);

enum MenuItem {
    MENU_STATUS = 0,
    MENU_TOGGLE_ADV,
    MENU_UNICODE,
    MENU_DISCONNECT,
    MENU_COUNT
};

static ui::ListItem s_menuItems[MENU_COUNT];

/** \brief Rebuilds top-level HID menu entries based on runtime state. */
static void rebuildMenu() {
    auto& kb = BleHidKeyboard::instance();

    s_menuItems[MENU_STATUS] = {ui::tr("mod_blehid.status"), 0, false, nullptr};

    if (kb.isAdvertising() || kb.isConnected()) {
        s_menuItems[MENU_TOGGLE_ADV] = {ui::tr("mod_blehid.stop_adv"), 0, false, nullptr};
    } else {
        s_menuItems[MENU_TOGGLE_ADV] = {ui::tr("mod_blehid.start_adv"), 0, false, nullptr};
    }

    s_menuItems[MENU_UNICODE] = {ui::tr("mod_blehid.unicode_method"), 0, false, nullptr};
    s_menuItems[MENU_DISCONNECT] = {ui::tr("mod_blehid.disconnect"), 0, !kb.isConnected(), nullptr};

    s_menuView.init(ui::tr("mod_blehid.title"), s_menuItems, MENU_COUNT);
}

/**
 * \brief Handles actions for selected HID menu item.
 * \param index Selected item index.
 * \param userData Optional callback user data.
 */
static void onMenuSelect(uint16_t index, void* userData) {
    (void)userData;
    auto& kb = BleHidKeyboard::instance();

    switch (index) {
        case MENU_STATUS:
            ui::ViewStack::instance().push(&s_statusView);
            break;

        case MENU_TOGGLE_ADV:
            if (kb.isAdvertising() || kb.isConnected()) {
                kb.stopAdvertising();
                ui::showToastInfo("Advertising stopped");
            } else {
                if (kb.startAdvertising()) {
                    ui::showToastSuccess("Advertising started");
                } else {
                    ui::showToastError("Failed to start");
                }
            }
            rebuildMenu();
            break;

        case MENU_UNICODE: {
            static ui::ListItem unicodeItems[4] = {
                {ui::tr("mod_blehid.ascii_only"), 0, false, nullptr},
                {ui::tr("mod_blehid.windows"), 0, false, nullptr},
                {ui::tr("mod_blehid.linux"), 0, false, nullptr},
                {ui::tr("mod_blehid.macos"), 0, false, nullptr}
            };
            s_unicodeMenu.init(ui::tr("mod_blehid.unicode_method"), unicodeItems, 4);
            s_unicodeMenu.setOnSelect(onUnicodeSelect);
            ui::ViewStack::instance().push(&s_unicodeMenu);
            break;
        }

        case MENU_DISCONNECT:
            if (kb.isConnected()) {
                auto* ble = cdc::hal::getBluetoothControllerInstance();
                if (ble) ble->disconnect();
                ui::showToastInfo("Disconnected");
                rebuildMenu();
            }
            break;
    }
}

/**
 * \brief Stores selected Unicode method and returns to previous view.
 * \param index Selected Unicode method index.
 * \param userData Optional callback user data.
 */
static void onUnicodeSelect(uint16_t index, void* userData) {
    (void)userData;
    auto& kb = BleHidKeyboard::instance();
    kb.setUnicodeMethod(static_cast<keyboard::UnicodeMethod>(index));
    ui::showToastSuccess(ui::tr("core.ok"));
    ui::ViewStack::instance().pop();
}

/**
 * \brief Returns HID module singleton instance.
 * \return Reference to singleton module.
 */
BleHidModule& BleHidModule::instance() {
    static BleHidModule inst;
    return inst;
}

/**
 * \brief Initializes BLE HID keyboard module and service registrations.
 * \return `true` on success, otherwise `false`.
 */
bool BleHidModule::init() {
    LOG_I(TAG, "Initializing BLE HID module");
    registerStrings();

    // Initialize BLE HID keyboard
    if (!BleHidKeyboard::instance().init()) {
        LOG_W(TAG, "BLE HID init failed (BLE might not be available)");
        // Continue anyway - module can show status
    }

    // Register keyboard service for other modules
    core::ServiceRegistry::instance().provide<core::IKeyboardProvider>(
        core::ServiceType::KEYBOARD,
        &BleHidKeyboard::instance()
    );

    core::ModuleRegistry::instance().registerModule(this);
    state_ = core::ServiceState::INITIALIZED;
    return true;
}

/**
 * \brief Starts the HID module.
 * \return `true` when state transition is valid, otherwise `false`.
 */
bool BleHidModule::start() {
    if (state_ != core::ServiceState::INITIALIZED &&
        state_ != core::ServiceState::STOPPED) {
        return false;
    }
    state_ = core::ServiceState::STARTED;
    return true;
}

/**
 * \brief Stops the HID module and deinitializes keyboard backend.
 */
void BleHidModule::stop() {
    BleHidKeyboard::instance().deinit();
    state_ = core::ServiceState::STOPPED;
}

/**
 * \brief Exposes module menu entry for settings menu integration.
 * \param items Destination array for menu items.
 * \param maxItems Capacity of `items`.
 * \return Number of entries written to `items`.
 */
uint8_t BleHidModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    items[0] = {ui::tr("mod_blehid.title"), 55, []() -> ui::IView* {
        if (!s_viewsInitialized) {
            s_menuView.setOnSelect(onMenuSelect);
            s_viewsInitialized = true;
        }
        rebuildMenu();
        return &s_menuView;
    }, nullptr, "mod_blehid", core::MenuLocation::SETTINGS_MENU, nullptr};

    return 1;
}

} // namespace cdc::mod_blehid

/**
 * \brief Registers HID module initializer with the global module registry.
 */
extern "C" void mod_blehid_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_blehid::BleHidModule::instance();
        module.init();
    });
}
