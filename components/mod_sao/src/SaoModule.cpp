#include "mod_sao/SaoModule.h"
#include "mod_sao/sao.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_ui/I18n.h"
#include "cdc_views/InfoView.h"
#include "cdc_log.h"
#include <cstring>

static const char* TAG = "SAO";

namespace cdc::mod_sao {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_sao.title", "SAO"},
};

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

static ui::InfoView s_infoView;
static char s_infoText[256];

static ui::IView* getInfoView() {
    sao_get_info_string(s_infoText, sizeof(s_infoText));
    s_infoView.init(ui::tr("mod_sao.title"), s_infoText);
    return &s_infoView;
}

SaoModule& SaoModule::instance() {
    static SaoModule inst;
    return inst;
}

bool SaoModule::init() {
    LOG_I(TAG, "Initializing SAO module");
    registerStrings();
    core::ModuleRegistry::instance().registerModule(this);
    state_ = core::ServiceState::INITIALIZED;
    return true;
}

bool SaoModule::start() {
    if (!core::ModuleBase::start()) {
        return false;
    }
    if (!sao_init()) {
        LOG_W(TAG, "SAO init failed (I2C1 may be unavailable)");
    }
    return true;
}

uint8_t SaoModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;
    items[0] = {
        ui::tr("mod_sao.title"),
        120,
        getInfoView,
        nullptr,
        getName(),
        core::MenuLocation::TOOLS_MENU,
        nullptr
    };
    return 1;
}

void SaoModule::onUnlock() {
    if (state_ != core::ServiceState::STARTED) return;
    sao_scan();
}

} // namespace cdc::mod_sao

extern "C" void mod_sao_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_sao::SaoModule::instance();
        module.init();
    });
}
