/**
 * \file PluginListView.h
 * \brief Main-menu entry "Plugins" - lists all installed WASM plugins.
 *
 * Y starts the highlighted plugin (delegated to PluginManager::startPlugin).
 * 3 opens a context menu with Start/Stop and Enable/Disable actions.
 * N pops back to the previous menu.
 *
 * A background-running plugin is marked with a leading sun icon. A disabled
 * plugin is marked with a crossed-out error icon and cannot be started.
 */

#pragma once

#include "cdc_ui/IView.h"
#include "cdc_views/ListView.h"
#include "plugin_manager/PluginManifest.h"

#include <string>
#include <vector>

namespace cdc::plugin_manager {

class PluginListView : public cdc::ui::ViewBase {
public:
    PluginListView();

    void onEnter(void* context = nullptr) override;
    void onExit() override;
    void onResume() override;
    void render(bool partial) override;
    bool needsRender() const override { return list_.needsRender(); }
    void markDirty()         override { list_.markDirty(); }
    void clearDirty()        override { list_.clearDirty(); }
    cdc::ui::InputResult onKey(char key) override;
    const char* getName() const override { return "PluginListView"; }
    const char* getFooterHint() const override;

    /// Currently-mounted PluginListView instance, or nullptr if none.
    [[nodiscard]] static PluginListView* active() noexcept;

    /// Rebuild the item list from installed plugins and request a redraw.
    /// Called after a context-menu Stop changes a plugin's running state.
    void refresh();

private:
    void rebuildItems();
    static void onSelectStatic(uint16_t index, void* userData);
    static void onMenuStatic(uint16_t index, void* userData);
    void onSelect(uint16_t index);
    void onMenu(uint16_t index);

    cdc::ui::ListView list_;
    std::vector<std::string> ids_;
    std::vector<std::string> labels_;
    std::vector<cdc::ui::ListItem> items_;
};

}  // namespace cdc::plugin_manager
