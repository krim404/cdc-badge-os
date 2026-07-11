/**
 * \file
 * \brief USB OTP HID module: lifecycle, Keyboard-slot arbitration and status UI.
 */

#include "mod_otphid/OtpHidModule.h"
#include "mod_otphid/OtpHidInterface.h"
#include "mod_otphid/OtpHidCr.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/UsbServiceManager.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ListView.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"
#include <goodisplay/gdey029T94.h>

static const char* TAG = "OTPHID";

namespace cdc::mod_otphid {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_otphid.title",      "USB OTP (CR)"},
    {"mod_otphid.connection", "Connection"},
    {"mod_otphid.waiting",    "Waiting for host"},
    {"mod_otphid.identity",   "Identity"},
    {"mod_otphid.cr_confirm", "Allow challenge-response?"},
};

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

/** \brief Status view showing OTP HID connection state and emulated identity. */
class OtpHidStatusView : public ui::ViewBase {
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

        ui::render::drawHeaderLeft(gfx, ui::tr("mod_otphid.title"), 8, 6, width);

        auto& otp = OtpHidInterface::instance();

        gfx->setCursor(8, 30);
        ui::render::printText(gfx, ui::tr("mod_otphid.connection"));
        ui::render::printText(gfx, ": ");
        const char* state = otp.isConnected()   ? ui::tr("core.connected")
                            : otp.isRegistered() ? ui::tr("mod_otphid.waiting")
                                                 : ui::tr("core.inactive");
        ui::render::printText(gfx, state);

        gfx->setCursor(8, 48);
        ui::render::printText(gfx, ui::tr("mod_otphid.identity"));
        ui::render::printText(gfx, ": OnlyKey 1D50:60FC");

        ui::render::drawFooterBar(gfx, width, height, nullptr, "[N] Back");
        clearDirty();
    }

    ui::InputResult onKey(char key) override {
        if (key == 'N') return ui::InputResult::REQUEST_POP;
        return ui::InputResult::IGNORED;
    }

    const char* getName() const override { return "OtpHidStatusView"; }
    const char* getFooterHint() const override { return "[N] Back"; }

private:
    uint32_t lastUpdate_ = 0;
};

/** \brief Static UI objects for OTP HID menu navigation. */
static ui::ListView s_menuView;
static OtpHidStatusView s_statusView;
static bool s_viewsInitialized = false;

enum MenuItem {
    MENU_STATUS = 0,
    MENU_COUNT
};

static ui::ListItem s_menuItems[MENU_COUNT];

/** \brief Handles top-level OTP HID menu selection. */
static void onMenuSelect(uint16_t index, void* userData) {
    (void)userData;
    switch (index) {
        case MENU_STATUS:
            ui::ViewStack::instance().push(&s_statusView);
            break;
    }
}

/** \brief Builds the top-level OTP HID menu entries. */
static void rebuildMenu() {
    s_menuItems[MENU_STATUS] = {ui::tr("core.status"), 0, false, nullptr};
    s_menuView.init(ui::tr("mod_otphid.title"), s_menuItems, MENU_COUNT);
}

OtpHidModule& OtpHidModule::instance() {
    static OtpHidModule inst;
    return inst;
}

bool OtpHidModule::init() {
    LOG_I(TAG, "Initializing USB OTP HID module");
    registerStrings();
    core::ModuleRegistry::instance().registerModule(this);
    core::UsbServiceManager::instance().registerModuleService("otphid", "mod_otphid.title",
                                                              getName(), {1, 0}, "kbd");
    state_ = core::ServiceState::INITIALIZED;
    return true;
}

bool OtpHidModule::start() {
    if (state_ != core::ServiceState::INITIALIZED &&
        state_ != core::ServiceState::STOPPED) {
        return false;
    }

    if (!OtpHidInterface::instance().registerUsb()) {
        // USB HID budget exhausted (e.g. FIDO + CCID already active) or the
        // Keyboard slot is held by mod_usbhid. Return false cleanly; the Expert
        // menu classifies this as UsbBudgetFull and shows core.usb_no_free_slot.
        LOG_W(TAG, "Start aborted: no free USB HID slot");
        return false;
    }

    state_ = core::ServiceState::STARTED;
    return true;
}

void OtpHidModule::stop() {
    OtpHidInterface::instance().unregisterUsb();
    state_ = core::ServiceState::STOPPED;
}

void OtpHidModule::onTick(uint32_t nowMs) {
    // Drive the CR state machine on the main task: all crypto + the touch
    // confirm UI happen here, never in the TinyUSB feature-report callbacks.
    OtpHidCr::instance().tick(nowMs);
}

uint8_t OtpHidModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    items[0] = {ui::tr("mod_otphid.title"), 57, []() -> ui::IView* {
        if (!s_viewsInitialized) {
            s_menuView.setOnSelect(onMenuSelect);
            s_viewsInitialized = true;
        }
        rebuildMenu();
        return &s_menuView;
    }, nullptr, "mod_otphid", core::MenuLocation::SETTINGS_MENU, nullptr};

    return 1;
}

} // namespace cdc::mod_otphid

/**
 * \brief Registers the USB OTP HID module with the global module registry.
 */
extern "C" void mod_otphid_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_otphid::OtpHidModule::instance();
        module.init();
        // Default-disabled: ModuleRegistry starts it only when enabled.
    });
}
