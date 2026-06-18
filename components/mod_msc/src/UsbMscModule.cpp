/**
 * \file
 * \brief USB Mass Storage module: lifecycle, MSC backend wiring and status UI.
 */

#include "mod_msc/UsbMscModule.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/UsbManager.h"
#include "plugin_manager/PluginStorage.h"
#include "usb_badge/usb_msc.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/IView.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"
#include <goodisplay/gdey029T94.h>

static const char* TAG = "USBMSC";

namespace cdc::mod_msc {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_msc.title",     "USB Storage"},
    {"mod_msc.status",    "Status"},
    {"mod_msc.active",    "Active"},
    {"mod_msc.inactive",  "Inactive"},
    {"mod_msc.host",      "Host"},
    {"mod_msc.connected", "Connected"},
    {"mod_msc.idle",      "Idle"},
};

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

// --- MSC block backend: thunks onto the vfat storage layer ---

static bool msc_read(uint32_t lba, uint32_t offset, void* buf, uint32_t len) {
    return plugin_manager::PluginStorage::blockRead(lba, offset, buf, len);
}
static bool msc_write(uint32_t lba, uint32_t offset, const void* buf, uint32_t len) {
    return plugin_manager::PluginStorage::blockWrite(lba, offset, buf, len);
}
static uint64_t msc_total_bytes() {
    return plugin_manager::PluginStorage::blockTotalBytes();
}
static uint16_t msc_block_size() {
    return plugin_manager::PluginStorage::blockSize();
}
static void msc_set_host_active(bool active) {
    plugin_manager::PluginStorage::setHostActive(active);
}

static const usb_msc_backend_t kBackend = {
    msc_read, msc_write, msc_total_bytes, msc_block_size, msc_set_host_active,
};

/** \brief Status view showing whether the drive is exposed and a host attached. */
class UsbMscStatusView : public ui::ViewBase {
public:
    void onEnter(void* context) override { (void)context; dirty_ = true; }

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
        gfx->setFont(nullptr);
        gfx->setTextColor(EPD_BLACK);
        gfx->setTextSize(1);

        ui::render::drawHeaderLeft(gfx, ui::tr("mod_msc.title"), 8, 6, width);

        const bool active = core::UsbManager::instance().massStorageActive();
        const bool host = plugin_manager::PluginStorage::hostActive();

        gfx->setCursor(8, 30);
        ui::render::printText(gfx, ui::tr("mod_msc.status"));
        ui::render::printText(gfx, ": ");
        ui::render::printText(gfx, active ? ui::tr("mod_msc.active")
                                          : ui::tr("mod_msc.inactive"));

        gfx->setCursor(8, 48);
        ui::render::printText(gfx, ui::tr("mod_msc.host"));
        ui::render::printText(gfx, ": ");
        ui::render::printText(gfx, host ? ui::tr("mod_msc.connected")
                                        : ui::tr("mod_msc.idle"));

        ui::render::drawFooterBar(gfx, width, height, nullptr, "[N] Back");
        clearDirty();
    }

    ui::InputResult onKey(char key) override {
        if (key == 'N') return ui::InputResult::REQUEST_POP;
        return ui::InputResult::IGNORED;
    }

    const char* getName() const override { return "UsbMscStatusView"; }
    const char* getFooterHint() const override { return "[N] Back"; }

private:
    uint32_t lastUpdate_ = 0;
};

static UsbMscStatusView s_statusView;

UsbMscModule& UsbMscModule::instance() {
    static UsbMscModule inst;
    return inst;
}

bool UsbMscModule::init() {
    LOG_I(TAG, "Initializing USB Mass Storage module");
    registerStrings();
    core::ModuleRegistry::instance().registerModule(this);
    state_ = core::ServiceState::INITIALIZED;
    return true;
}

bool UsbMscModule::start() {
    if (state_ != core::ServiceState::INITIALIZED &&
        state_ != core::ServiceState::STOPPED) {
        return false;
    }

    usb_msc_set_backend(&kBackend);
    if (!core::UsbManager::instance().registerMassStorage(getName())) {
        // USB endpoint budget exhausted (e.g. FIDO + CCID already active).
        LOG_W(TAG, "Start aborted: no USB endpoints for MSC");
        usb_msc_set_backend(nullptr);
        return false;
    }

    state_ = core::ServiceState::STARTED;
    return true;
}

void UsbMscModule::stop() {
    core::UsbManager::instance().unregisterMassStorage(getName());
    usb_msc_set_backend(nullptr);
    plugin_manager::PluginStorage::setHostActive(false);
    state_ = core::ServiceState::STOPPED;
}

void UsbMscModule::onTick(uint32_t nowMs) {
    (void)nowMs;
    // Perform any remount scheduled when a host detached, off the USB task.
    plugin_manager::PluginStorage::remountIfPending();
}

uint8_t UsbMscModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    items[0] = {ui::tr("mod_msc.title"), 58, []() -> ui::IView* {
        return &s_statusView;
    }, nullptr, "mod_msc", core::MenuLocation::SETTINGS_MENU, nullptr};

    return 1;
}

} // namespace cdc::mod_msc

/**
 * \brief Registers the USB Mass Storage module with the global module registry.
 */
extern "C" void mod_msc_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_msc::UsbMscModule::instance();
        module.init();
        // Default-disabled: ModuleRegistry starts it only when enabled.
    });
}
