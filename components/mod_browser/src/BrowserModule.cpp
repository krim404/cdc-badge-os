#include "mod_browser/BrowserModule.h"

#include "Bookmarks.h"
#include "BrowserController.h"

#include "cdc_core/ModuleRegistry.h"
#include "cdc_ui/I18n.h"
#include "cdc_views/HtmlViewerHook.h"
#include "cdc_log.h"

#include <cstddef>

static const char* TAG = "BROWSER";

namespace cdc::mod_browser {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_browser.title", "Browser"},
    {"mod_browser.loading", "Loading..."},
    {"mod_browser.menu", "Menu"},
    {"mod_browser.open_webpage", "Open webpage"},
    {"mod_browser.links", "Links"},
    {"mod_browser.no_links", "No links on this page"},
    {"mod_browser.bookmark", "Bookmark"},
    {"mod_browser.view_source", "View source"},
    {"mod_browser.view_full", "View full page"},
    {"mod_browser.view_readable", "View readable"},
    {"mod_browser.delete_bookmark", "Delete bookmark"},
    {"mod_browser.offline", "Not connected. Connect WiFi in Settings."},
    {"mod_browser.err_net", "Could not load the page."},
    {"mod_browser.err_type", "Unsupported content type."},
    {"mod_browser.err_empty", "No readable content (the page may need JavaScript)."},
    {"mod_browser.err_mem", "Out of memory."},
    {"mod_browser.captive_login", "Captive portal login"},
    {"mod_browser.captive_detected", "Captive portal detected"},
    {"mod_browser.captive_prompt", "Captive portal found. Log in now?"},
    {"mod_browser.connected", "Connected."},
    {"mod_browser.login", "Login"},
    {"mod_browser.submit", "Submit"},
    {"mod_browser.accept", "Accept"},
};

static void registerStrings()
{
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

BrowserModule& BrowserModule::instance()
{
    static BrowserModule inst;
    return inst;
}

bool BrowserModule::init()
{
    LOG_I(TAG, "Initializing browser module");
    registerStrings();
    browser::Bookmarks::instance().load();
    ui::setHtmlViewer(browser::browserShowLocalHtml);
    ui::setUrlOpener(browser::browserOpenUrl);
    core::ModuleRegistry::instance().registerModule(this);
    state_ = core::ServiceState::INITIALIZED;
    return true;
}

bool BrowserModule::start()
{
    return core::ModuleBase::start();
}

void BrowserModule::onTick(uint32_t)
{
    browser::browserPoll();
}

uint8_t BrowserModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems)
{
    if (!items || maxItems == 0) return 0;
    items[0] = {
        ui::tr("mod_browser.title"),
        110,
        browser::browserEntryView,
        nullptr,
        getName(),
        core::MenuLocation::TOOLS_MENU,
        nullptr,
    };
    return 1;
}

}  // namespace cdc::mod_browser

extern "C" void mod_browser_register()
{
    cdc::core::ModuleRegistry::instance().registerInitializer(
        []() { cdc::mod_browser::BrowserModule::instance().init(); });
}
