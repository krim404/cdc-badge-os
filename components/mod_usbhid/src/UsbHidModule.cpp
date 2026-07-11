/**
 * \file
 * \brief USB HID keyboard module: lifecycle, keyboard-provider arbitration and UI.
 */

#include "mod_usbhid/UsbHidModule.h"
#include "mod_usbhid/UsbHidKeyboard.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/ServiceRegistry.h"
#include "cdc_core/UsbManager.h"
#include "cdc_core/UsbServiceManager.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ListView.h"
#include "cdc_views/ToastView.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"
#include <goodisplay/gdey029T94.h>

static const char* TAG = "USBHID";

namespace cdc::mod_usbhid {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_usbhid.title",          "USB Keyboard"},
    {"mod_usbhid.connection",     "Connection"},
    {"mod_usbhid.waiting",        "Waiting for host"},
    {"mod_usbhid.not_registered", "Not active"},
    {"mod_usbhid.unicode_method", "Unicode Method"},
    {"mod_usbhid.ascii_only",     "ASCII only"},
    {"mod_usbhid.windows",        "Windows (Alt+Numpad)"},
    {"mod_usbhid.linux",          "Linux (Ctrl+Shift+U)"},
    {"mod_usbhid.macos",          "macOS (limited)"},
};

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

/** \brief Returns the i18n key for a Unicode method's display name. */
static const char* unicodeMethodKey(keyboard::UnicodeMethod method) {
    switch (method) {
        case keyboard::UnicodeMethod::WINDOWS: return "mod_usbhid.windows";
        case keyboard::UnicodeMethod::LINUX:   return "mod_usbhid.linux";
        case keyboard::UnicodeMethod::MACOS:   return "mod_usbhid.macos";
        case keyboard::UnicodeMethod::ASCII_ONLY:
        default:                               return "mod_usbhid.ascii_only";
    }
}

/** \brief Status view showing USB HID connection state and Unicode method. */
class UsbHidStatusView : public ui::ViewBase {
public:
    void onEnter(void* context) override {
        (void)context;
        dirty_ = true;
    }

    void onTick(uint32_t nowMs) override {
        if (nowMs - lastUpdate_ >= 1000) {
            lastUpdate_ = nowMs;
            markDirty();
        }
    }

    void render(bool partial) override {
        auto* display = hal::getDisplayInstance();
        if (!display) return;
        auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
        if (!gfx) return;

        const uint16_t width = display->getWidth();
        const uint16_t height = display->getHeight();

        if (!partial) gfx->fillScreen(EPD_WHITE);
        gfx->setFont(nullptr);  // 6x8 built-in (CP437)
        gfx->setTextColor(EPD_BLACK);
        gfx->setTextSize(1);

        ui::render::drawHeaderLeft(gfx, ui::tr("mod_usbhid.title"), 8, 6, width);

        auto& kb = UsbHidKeyboard::instance();

        gfx->setCursor(8, 30);
        ui::render::printText(gfx, ui::tr("mod_usbhid.connection"));
        ui::render::printText(gfx, ": ");
        const char* state = kb.isConnected()       ? ui::tr("core.connected")
                            : kb.isRegistered()     ? ui::tr("mod_usbhid.waiting")
                                                    : ui::tr("mod_usbhid.not_registered");
        ui::render::printText(gfx, state);

        gfx->setCursor(8, 48);
        ui::render::printText(gfx, ui::tr("mod_usbhid.unicode_method"));
        ui::render::printText(gfx, ": ");
        ui::render::printText(gfx, ui::tr(unicodeMethodKey(kb.getUnicodeMethod())));

        ui::render::drawFooterBar(gfx, width, height, nullptr, "[N] Back");
        clearDirty();
    }

    ui::InputResult onKey(char key) override {
        if (key == 'N') return ui::InputResult::REQUEST_POP;
        return ui::InputResult::IGNORED;
    }

    const char* getName() const override { return "UsbHidStatusView"; }
    const char* getFooterHint() const override { return "[N] Back"; }

private:
    uint32_t lastUpdate_ = 0;
};

/** \brief Static UI objects for USB HID menu navigation. */
static ui::ListView s_menuView;
static ui::ListView s_unicodeMenu;
static UsbHidStatusView s_statusView;
static bool s_viewsInitialized = false;

enum MenuItem {
    MENU_STATUS = 0,
    MENU_UNICODE,
    MENU_COUNT
};

static ui::ListItem s_menuItems[MENU_COUNT];

/** \brief Applies the selected Unicode input method and returns to the menu. */
static void onUnicodeSelect(uint16_t index, void* userData) {
    (void)userData;
    UsbHidKeyboard::instance().setUnicodeMethod(
        static_cast<keyboard::UnicodeMethod>(index));
    ui::showToastSuccess(ui::tr("core.ok"));
    ui::ViewStack::instance().pop();
}

/** \brief Handles top-level USB HID menu selection. */
static void onMenuSelect(uint16_t index, void* userData) {
    (void)userData;
    switch (index) {
        case MENU_STATUS:
            ui::ViewStack::instance().push(&s_statusView);
            break;
        case MENU_UNICODE: {
            static ui::ListItem unicodeItems[4] = {
                {ui::tr("mod_usbhid.ascii_only"), 0, false, nullptr},
                {ui::tr("mod_usbhid.windows"), 0, false, nullptr},
                {ui::tr("mod_usbhid.linux"), 0, false, nullptr},
                {ui::tr("mod_usbhid.macos"), 0, false, nullptr},
            };
            s_unicodeMenu.init(ui::tr("mod_usbhid.unicode_method"), unicodeItems, 4);
            s_unicodeMenu.setOnSelect(onUnicodeSelect);
            ui::ViewStack::instance().push(&s_unicodeMenu);
            break;
        }
    }
}

/** \brief Builds the top-level USB HID menu entries. */
static void rebuildMenu() {
    s_menuItems[MENU_STATUS] = {ui::tr("core.status"), 0, false, nullptr};
    s_menuItems[MENU_UNICODE] = {ui::tr("mod_usbhid.unicode_method"), 0, false, nullptr};
    s_menuView.init(ui::tr("mod_usbhid.title"), s_menuItems, MENU_COUNT);
}

UsbHidModule& UsbHidModule::instance() {
    static UsbHidModule inst;
    return inst;
}

bool UsbHidModule::init() {
    LOG_I(TAG, "Initializing USB HID keyboard module");
    registerStrings();
    core::ModuleRegistry::instance().registerModule(this);
    core::UsbServiceManager::instance().registerModuleService("kbd", "mod_usbhid.title",
                                                              getName(), {1, 0}, "otphid");
    state_ = core::ServiceState::INITIALIZED;
    return true;
}

bool UsbHidModule::start() {
    if (state_ != core::ServiceState::INITIALIZED &&
        state_ != core::ServiceState::STOPPED) {
        return false;
    }

    if (!UsbHidKeyboard::instance().registerUsb()) {
        // No endpoint slot and no CCID interface to borrow one from. Return
        // false cleanly; the Expert menu classifies this as UsbBudgetFull and
        // shows core.usb_no_free_slot.
        LOG_W(TAG, "Start aborted: no free USB HID slot");
        return false;
    }

    // Take over the keyboard provider, remembering the previous one (e.g. BLE)
    // so stop() can restore it.
    auto& sr = core::ServiceRegistry::instance();
    prevKeyboard_ = sr.request<core::IKeyboardProvider>(core::ServiceType::KEYBOARD);
    sr.provide<core::IKeyboardProvider>(core::ServiceType::KEYBOARD,
                                        &UsbHidKeyboard::instance());

    state_ = core::ServiceState::STARTED;
    return true;
}

void UsbHidModule::stop() {
    auto& sr = core::ServiceRegistry::instance();
    // Restore the previous provider only if we still own the slot, so we never
    // clobber a provider that registered after us.
    if (sr.request<core::IKeyboardProvider>(core::ServiceType::KEYBOARD) ==
        &UsbHidKeyboard::instance()) {
        sr.provide<core::IKeyboardProvider>(core::ServiceType::KEYBOARD, prevKeyboard_);
    }
    prevKeyboard_ = nullptr;

    UsbHidKeyboard::instance().unregisterUsb();
    state_ = core::ServiceState::STOPPED;
}

uint8_t UsbHidModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    items[0] = {ui::tr("mod_usbhid.title"), 56, []() -> ui::IView* {
        if (!s_viewsInitialized) {
            s_menuView.setOnSelect(onMenuSelect);
            s_viewsInitialized = true;
        }
        rebuildMenu();
        return &s_menuView;
    }, nullptr, "mod_usbhid", core::MenuLocation::SETTINGS_MENU, nullptr};

    return 1;
}

} // namespace cdc::mod_usbhid

/**
 * \brief Registers the USB HID keyboard module with the global module registry.
 */
extern "C" void mod_usbhid_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_usbhid::UsbHidModule::instance();
        module.init();
        // Default-disabled: ModuleRegistry starts it only when enabled.
    });
}
