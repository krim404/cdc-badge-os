/**
 * \file host_api_ui.cpp
 * \brief Real implementations for the UI subset that plugins use most.
 *
 * Pushes pre-built cdc_views onto the ViewStack. The remaining UI host API
 * (T9, PIN, slider, date/time, list with action callbacks) lands in later
 * Phase-3 steps once the Plugin object can route action-id callbacks back
 * into the WASM module.
 */

#include "plugin_manager/host_api.h"
#include "plugin_manager/PluginManager.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_views/ToastView.h"
#include "cdc_views/InfoView.h"
#include "cdc_views/ImageView.h"
#include "cdc_views/MarkdownView.h"
#include "cdc_views/HtmlViewerHook.h"
#include "cdc_ui/ViewStack.h"
#include "host_str_conv.h"

#include <string>

namespace {

cdc::ui::ToastView::Icon toIcon(uint8_t v)
{
    switch (v) {
        case UI_ICON_SUCCESS: return cdc::ui::ToastView::Icon::SUCCESS;
        case UI_ICON_ERROR:   return cdc::ui::ToastView::Icon::ERROR;
        case UI_ICON_INFO:    return cdc::ui::ToastView::Icon::INFO;
        case UI_ICON_TASK:    return cdc::ui::ToastView::Icon::TASK;
        case UI_ICON_ALERT:   return cdc::ui::ToastView::Icon::ALERT;
        default:              return cdc::ui::ToastView::Icon::NONE;
    }
}

}  // namespace

extern "C" {

int host_ui_push_toast(const char* text, uint8_t icon, uint16_t duration_ms)
{
    if (!text) return HOST_ERR_INVALID_ARG;
    std::string cp = cdc::plugin_manager::toDisplay(text);
    static cdc::ui::ToastView s_pluginToast;
    s_pluginToast.init(cp.c_str(), toIcon(icon), duration_ms, true);
    cdc::ui::ViewStack::instance().showModal(&s_pluginToast);
    cdc::ui::ViewStack::instance().render();
    return HOST_OK;
}

int host_ui_push_message(const char* text, uint8_t icon, uint32_t duration_ms)
{
    return host_ui_push_toast(text, icon, static_cast<uint16_t>(duration_ms));
}

int host_ui_push_info(const char* title, const char* body)
{
    if (!title || !body) return HOST_ERR_INVALID_ARG;
    std::string cpTitle = cdc::plugin_manager::toDisplay(title);
    std::string cpBody  = cdc::plugin_manager::toDisplay(body);
    auto* info = new cdc::ui::InfoView();
    info->init(cpTitle.c_str(), cpBody.c_str());
    cdc::ui::ViewStack::instance().push(info);
    return HOST_OK;
}

int host_ui_view_image(const uint8_t* data, uint32_t len)
{
    if (!data || len == 0) return HOST_ERR_INVALID_ARG;
    cdc::ui::showImage(nullptr, data, len);
    return HOST_OK;
}

int host_ui_view_markdown(const uint8_t* data, uint32_t len)
{
    if (!data || len == 0) return HOST_ERR_INVALID_ARG;
    std::string src(reinterpret_cast<const char*>(data), len);
    std::string body = cdc::plugin_manager::toDisplay(src.c_str());
    cdc::ui::showMarkdown(nullptr, body.c_str(), body.size());
    return HOST_OK;
}

int host_browser_open(const char* url)
{
    if (!url || !url[0]) return HOST_ERR_INVALID_ARG;
    cdc::ui::UrlOpenerFn opener = cdc::ui::urlOpener();
    if (!opener) return HOST_ERR_NOT_SUPPORTED;  // browser module not present
    opener(url);  // URL stays raw UTF-8 (not display-converted)
    return HOST_OK;
}

int host_ui_pop(void)
{
    // A shown modal (toast/confirm/context menu) owns input and sits above the
    // view stack, so "go back" must dismiss it first; pop() applies to the view
    // stack only when no modal is up. Otherwise a plugin's ui::pop() would leave
    // its modal stranded and pop an unrelated view underneath.
    auto& vs = cdc::ui::ViewStack::instance();
    if (vs.hasModal()) {
        vs.hideModal();
    } else {
        vs.pop();
    }
    return HOST_OK;
}

int host_ui_pop_to_plugin(void)
{
    uint8_t base = cdc::plugin_manager::PluginManager::instance().pluginBaseDepth();
    // The plugin's first view sits at base + 1; collapse every overlay above it
    // in one step. Views below the plugin (launcher, home) stay untouched.
    cdc::ui::ViewStack::instance().popToDepth(base + 1);
    return HOST_OK;
}

int host_ui_repaint(void)
{
    return HOST_OK;
}

int host_ui_wink(uint8_t count, uint16_t period_ms)
{
    cdc::hal::winkBacklight(count ? count : 2, period_ms ? period_ms : 150);
    return HOST_OK;
}

}  // extern "C"
