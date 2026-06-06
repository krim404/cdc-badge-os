#include "plugin_manager/PluginListView.h"
#include "plugin_manager/PluginManager.h"
#include "plugin_manager/host_api.h"
#include "cdc_views/ToastView.h"
#include "cdc_views/ContextMenuView.h"
#include "cdc_ui/I18n.h"
#include "cdc_log.h"

#include <cstdio>

namespace cdc::plugin_manager {

static const char* TAG = "PLG_UI";
static PluginListView* s_active = nullptr;

// Plugin id targeted by the currently-open context menu (key 3). The menu's
// callbacks take no arguments, so the selection is stashed here.
static std::string s_ctxPluginId;
static cdc::ui::ContextMenuItem s_ctxItems[2];

/**
 * \brief Context-menu Stop callback: force-unloads the selected plugin.
 */
static void onCtxStop()
{
    cdc::ui::hideContextMenu();
    if (!s_ctxPluginId.empty()) {
        PluginManager::instance().unloadFromRam(s_ctxPluginId);
    }
    if (s_active) {
        s_active->refresh();
    }
}

/**
 * \brief Context-menu Disable callback: persistently disables and unloads.
 */
static void onCtxDisable()
{
    cdc::ui::hideContextMenu();
    if (!s_ctxPluginId.empty()) {
        if (PluginManager::instance().setPluginDisabled(s_ctxPluginId, true)) {
            cdc::ui::showToastInfo(cdc::ui::tr("core.plugin_disabled"), 1800);
        } else {
            cdc::ui::showToastError("Disable failed", 3000);
        }
    }
    if (s_active) {
        s_active->refresh();
    }
}

/**
 * \brief Context-menu Enable callback: removes the persistent disabled marker.
 */
static void onCtxEnable()
{
    cdc::ui::hideContextMenu();
    if (!s_ctxPluginId.empty()) {
        if (PluginManager::instance().setPluginDisabled(s_ctxPluginId, false)) {
            cdc::ui::showToastInfo(cdc::ui::tr("core.plugin_enabled"), 1800);
        } else {
            cdc::ui::showToastError("Enable failed", 3000);
        }
    }
    if (s_active) {
        s_active->refresh();
    }
}

/// Show a toast describing a non-Ok plugin start result.
static void reportStartResult(StartResult result)
{
    if (result == StartResult::Ok) return;
    static char msg[64];
    const char* label = "Start failed";
    switch (result) {
        case StartResult::PluginAlreadyRunning: label = "Already running"; break;
        case StartResult::ManifestInvalid:      label = "Manifest invalid"; break;
        case StartResult::CapabilityRejected:   label = "Capabilities rejected"; break;
        case StartResult::WamrLoadFailed:       label = "WASM load failed"; break;
        case StartResult::PluginInitFailed:     label = "plugin_init failed"; break;
        case StartResult::PrerequisiteFailed:   label = "Prerequisite failed"; break;
        case StartResult::PluginOnEnterFailed:  label = "plugin_on_enter missing"; break;
        case StartResult::Busy:                 label = "Plugin busy"; break;
        case StartResult::PluginDisabled:       label = cdc::ui::tr("core.plugin_disabled"); break;
        default: break;
    }
    std::snprintf(msg, sizeof(msg), "%s\nErr %d", label, static_cast<int>(result));
    cdc::ui::showToastError(msg, 3000);
}

/**
 * \brief Context-menu Start callback: starts the selected (stopped) plugin.
 */
static void onCtxStart()
{
    cdc::ui::hideContextMenu();
    if (!s_ctxPluginId.empty()) {
        reportStartResult(PluginManager::instance().startPlugin(s_ctxPluginId));
    }
    if (s_active) {
        s_active->refresh();
    }
}

PluginListView* PluginListView::active() noexcept { return s_active; }

PluginListView::PluginListView() = default;

void PluginListView::onEnter(void* /*context*/)
{
    s_active = this;
    list_.setOnSelect(&PluginListView::onSelectStatic);
    list_.setOnMenu  (&PluginListView::onMenuStatic);
    rebuildItems();
}

void PluginListView::onExit()
{
    if (s_active == this) s_active = nullptr;
}

void PluginListView::onResume()
{
    s_active = this;
    // A plugin declaring capabilities.background keeps ticking after the user
    // leaves its view; tell them so before we demote it off the foreground.
    if (PluginManager::instance().activePluginIsBackground()) {
        cdc::ui::showToastInfo(cdc::ui::tr("core.plugin_bg_running"), 1800);
    }
    PluginManager::instance().requestStopActivePlugin();
    rebuildItems();
    list_.markDirty();
}

void PluginListView::render(bool partial)
{
    list_.render(partial);
}

cdc::ui::InputResult PluginListView::onKey(char key)
{
    return list_.onKey(key);
}

const char* PluginListView::getFooterHint() const
{
    return cdc::ui::tr("core.hint_plugin_list");
}

void PluginListView::refresh()
{
    rebuildItems();
    list_.markDirty();
}

void PluginListView::rebuildItems()
{
    auto& mgr = PluginManager::instance();
    ids_     = mgr.listInstalledIds();
    labels_.clear();
    items_.clear();
    labels_.reserve(ids_.size());
    items_.reserve(ids_.size());

    for (const auto& id : ids_) {
        std::string display = id;
        if (auto mf = PluginManager::instance().getManifest(id)) {
            auto it = mf->i18n_meta.find("name");
            if (it != mf->i18n_meta.end() && !it->second.by_lang.empty()) {
                auto def = it->second.by_lang.find(mf->default_language);
                if (def != it->second.by_lang.end()) {
                    display = def->second;
                } else {
                    display = it->second.by_lang.begin()->second;
                }
            }
        }
        labels_.push_back(std::move(display));
        // Mark as running for an already-background plugin AND for the active
        // plugin that is about to be demoted to background (the demotion is
        // async, so this would otherwise miss the indicator on first open).
        const bool running = mgr.isRunningInBackground(id) ||
                             (mgr.activePluginIsBackground() && mgr.activePluginId() == id);
        const bool disabled = mgr.isPluginDisabled(id);
        // List icons are drawn as raw CP437 glyphs; 'X' marks a disabled plugin.
        const uint8_t icon = disabled ? static_cast<uint8_t>('X')
                                      : (running ? UI_ICON_SUN : 0);
        items_.push_back(cdc::ui::ListItem{labels_.back().c_str(), icon, false, nullptr});
    }

    list_.init("Plugins", items_.data(), static_cast<uint16_t>(items_.size()));
    list_.setEmptyText("No plugins installed");
    list_.setHint(cdc::ui::tr("core.hint_plugin_list"));
}

void PluginListView::onSelectStatic(uint16_t index, void*)
{
    if (s_active) s_active->onSelect(index);
}

void PluginListView::onMenuStatic(uint16_t index, void*)
{
    if (s_active) s_active->onMenu(index);
}

void PluginListView::onSelect(uint16_t index)
{
    if (index >= ids_.size()) return;
    const auto& id = ids_[index];
    LOG_I(TAG, "start plugin %s", id.c_str());

    reportStartResult(PluginManager::instance().startPlugin(id));
}

void PluginListView::onMenu(uint16_t index)
{
    if (index >= ids_.size()) return;
    s_ctxPluginId = ids_[index];

    auto& mgr = PluginManager::instance();
    const bool running = mgr.isRunningInBackground(s_ctxPluginId) ||
                         (mgr.activePluginIsBackground() && mgr.activePluginId() == s_ctxPluginId);
    const bool disabled = mgr.isPluginDisabled(s_ctxPluginId);

    uint8_t count = 0;
    if (disabled) {
        s_ctxItems[count++] = cdc::ui::ContextMenuItem{cdc::ui::tr("core.enable"), &onCtxEnable};
    } else {
        s_ctxItems[count++] = running
            ? cdc::ui::ContextMenuItem{cdc::ui::tr("core.stop"),  &onCtxStop}
            : cdc::ui::ContextMenuItem{cdc::ui::tr("core.start"), &onCtxStart};
        s_ctxItems[count++] = cdc::ui::ContextMenuItem{cdc::ui::tr("core.disable"), &onCtxDisable};
    }
    cdc::ui::showContextMenu(cdc::ui::tr("core.actions"), s_ctxItems, count);
}

}  // namespace cdc::plugin_manager
