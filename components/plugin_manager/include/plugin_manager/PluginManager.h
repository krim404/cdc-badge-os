/**
 * \file PluginManager.h
 * \brief Discovers, loads, runs and unloads WASM plugins on the badge.
 *
 * One PluginManager instance, registered as a cdc_core service. At most one
 * foreground plugin (the one the user is currently looking at) runs at any
 * given time. A plugin declaring `capabilities.background = true` keeps
 * running (and ticking in parallel with the foreground) after the user leaves
 * its view, instead of being unloaded. A plugin declaring
 * `capabilities.autoload = true` is loaded into the background at boot unless
 * the user has disabled it.
 */

#pragma once

#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginManifest.h"
#include "plugin_manager/PluginStorage.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cdc::plugin_manager {

enum class StartResult {
    Ok,
    PluginAlreadyRunning,
    ManifestMissing,
    ManifestInvalid,
    CapabilityRejected,
    WamrLoadFailed,
    WamrInstantiateFailed,
    PluginInitFailed,
    PrerequisiteFailed,
    PluginOnEnterFailed,
    Busy,
    PluginDisabled,
};

class PluginManager {
public:
    [[nodiscard]] static PluginManager& instance() noexcept;

    [[nodiscard]] bool init();
    void deinit();

    [[nodiscard]] std::vector<std::string>     listInstalledIds() const;
    [[nodiscard]] std::optional<PluginManifest> getManifest(const std::string& id) const;

    /// \brief True if any installed plugin's manifest declares this MIME type
    ///        for message transfer. Reads a cached index; safe to call from the
    ///        BLE host task.
    [[nodiscard]] bool messageTypeInstalled(const char* mime) const;
    /// \brief Load + start (headless) the installed plugin that declares this
    ///        MIME type, so its message handler becomes live. Must run on the
    ///        plugin tick task. \return true if a handling plugin is now loaded.
    bool activateForMessageType(const char* mime);
    /// \brief True if any installed plugin's manifest `provides` this external
    ///        feature. Reads the cached index (same mutex as the MIME index).
    [[nodiscard]] bool featureInstalled(const char* feature) const;
    /// \brief Installed plugin id providing this external feature, or empty.
    [[nodiscard]] std::string featureProviderId(const char* feature) const;
    [[nodiscard]] bool                         isPluginDisabled(const std::string& id) const;
    bool                                       setPluginDisabled(const std::string& id,
                                                                 bool disabled);

    [[nodiscard]] StartResult startPlugin(const std::string& id);
    bool                      stopActivePlugin();
    /// Unload a plugin from RAM regardless of foreground/background slot,
    /// keeping its files on disk. This is the forced-stop entry point for a
    /// resident background plugin. Returns true if the plugin was found and
    /// unloaded.
    bool                      unloadFromRam(const std::string& id);
    /// Unload from RAM (if loaded) and delete the plugin's stored files
    /// (wasm/aot/meta/lang/disabled). Returns true.
    bool                      uninstallPlugin(const std::string& id);
    /// Force-unload every loaded plugin (foreground + background) from RAM,
    /// keeping files on disk. Runs plugin_on_exit, drops the active plugin's
    /// views, and tears down all host resources per instance. Used by the
    /// anti-block instant lock.
    void                      unloadAllFromRam();
    /// Force a background plugin to be re-loaded from disk. Called after an
    /// upload overwrites the WASM so the running instance picks up the new
    /// binary without a reboot.
    bool                      reloadBackgroundPlugin(const std::string& id);
    /// Asynchronous stop request from a view callback (PluginListView::onResume).
    /// The tick task does the actual stop, after letting any in-flight action
    /// handler push a follow-up view first.
    void                      requestStopActivePlugin();
    [[nodiscard]] bool        hasActivePlugin() const noexcept;
    [[nodiscard]] std::string activePluginId()  const;
    /// View-stack depth recorded just before the active plugin's plugin_on_enter,
    /// i.e. the depth of the views below the plugin. The plugin's first view sits
    /// at pluginBaseDepth() + 1; host_ui_pop_to_plugin collapses down to it.
    [[nodiscard]] uint8_t     pluginBaseDepth() const noexcept { return plugin_base_depth_; }
    /// True if a plugin with `id` is loaded in RAM (foreground or background).
    [[nodiscard]] bool        isLoaded(const std::string& id) const;
    /// True if a plugin with `id` is currently resident in the background slot.
    [[nodiscard]] bool        isRunningInBackground(const std::string& id) const;
    /// True if at least one plugin is currently resident in the background slot.
    [[nodiscard]] bool        hasBackgroundPlugin() const noexcept;
    /// True if the foreground plugin declares `capabilities.background`, i.e. it
    /// keeps running after the user leaves its view instead of being unloaded.
    [[nodiscard]] bool        activePluginIsBackground() const;
    /// True if the foreground plugin declares `capabilities.prevent_sleep`,
    /// meaning it must stay permanently in the foreground: the idle auto-lock
    /// must not fire while it runs. Derived from the live `active_` slot, so it
    /// reverts to false on its own the moment the plugin leaves the foreground
    /// (no inhibitor flag that could leak).
    [[nodiscard]] bool        activePluginPreventsSleep() const;

    void dispatchButton(uint32_t button_code);
    void dispatchAction(uint32_t action_id, uint32_t idx, uint32_t user_data);
    /// Dispatch plugin_on_action to a specific plugin (e.g. event-bus subscriber).
    /// No-op if the plugin pointer is no longer in foreground or background.
    void dispatchActionTo(Plugin* plugin, uint32_t action_id, uint32_t idx, uint32_t user_data);
    void dispatchTick(uint64_t uptime_ms);

    /// Forward a command string to the plugin identified by `id` (foreground
    /// or background). Buffers the string and fires the optional
    /// `plugin_on_cmd(len)` export, which the plugin reads back via
    /// \ref consumeCmd. Returns false if no plugin with that id is loaded.
    bool dispatchCmd(const std::string& id, const char* cmd, size_t len);

    /// Copy the buffered command string into `out` and clear it. Called from
    /// the plugin (host_cmd_consume) inside its `plugin_on_cmd` handler.
    /// Returns the number of bytes copied, or a negative HOST_ERR_* code.
    int  consumeCmd(char* out, size_t out_size);

    /// Dispatch a bus event to every loaded plugin (foreground + background).
    /// Key events still go to the foreground only - see host_api_event.cpp.
    void dispatchEventAll(uint32_t event_type, uint32_t value);

    /// Iterate foreground + background plugins. Visitor returns false to stop.
    void forEachPlugin(const std::function<bool(Plugin&)>& visitor);

    /// Lockscreen quick-action contributed by a (background) plugin.
    /// `label` is already resolved against the plugin's manifest / lang
    /// overlay for the active language. `plugin` is opaque and round-tripped
    /// back through `triggerLockscreenItem`.
    struct LockscreenItem {
        Plugin*  plugin;
        char     label[40];
        uint32_t action_id;
    };

    /// Snapshot of all plugin lockscreen items. Returns the number written.
    uint8_t getLockscreenItems(LockscreenItem* out, uint8_t max) const;

    /// Fire `plugin_on_action(item.action_id, 0, 0)` on the owning plugin.
    void triggerLockscreenItem(const LockscreenItem& item);

    /// Re-read the active plugin's `<id>.lang` file for the current language.
    /// Called by the UI after the user switches language so plugin strings
    /// follow the active locale without restarting the plugin. No-op if no
    /// plugin is active.
    void reloadActiveLangOverlay();

private:
    PluginManager();
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    void startTickTask();
    void stopTickTask();
    static void tickTaskTrampoline(void* arg);
    void tickTaskLoop();

    /// Release every host resource the plugin holds (sleep inhibitor, lockscreen
    /// actions, BLE, GPIO/PWM, HTTP, prerequisites) and unload the WASM instance,
    /// in canonical order. Does NOT touch active_/background_ membership and does
    /// NOT call plugin_on_exit - the caller owns those. Caller must hold call_mutex_.
    /// \param runWasmDeinit run the plugin's own plugin_deinit first; pass false
    ///        for a trapped instance (no further WASM execution on a faulted module).
    void teardownPlugin(Plugin& p, bool runWasmDeinit);

    /// Verbose-log a trapped plugin and force-unload it from whichever slot holds
    /// it (skips further WASM: no plugin_on_exit, no plugin_deinit). Caller must
    /// hold call_mutex_ and must NOT be iterating active_/background_.
    /// \param fn Name of the export that trapped, for the diagnostic banner.
    void handleTrap(Plugin& p, const char* fn);

    /// Load, init and run prerequisites for a plugin straight into the
    /// background slot (headless: no foreground view, no plugin_on_enter).
    /// Caller must hold call_mutex_. Returns true on success.
    bool loadIntoBackground(const std::string& id, const PluginManifest& mf);

    /// At boot, start every installed plugin whose manifest declares
    /// `capabilities.autoload` as a resident background instance. Plugins
    /// without the flag stay unloaded until the user starts them manually.
    void loadAutoloadPlugins();

    /// Rebuild the MIME-type -> plugin-id index from installed manifests.
    void rebuildMessageIndex();
    /// Throttled refresh of the message index when the installed set changed.
    void maybeRefreshMessageIndex();

    std::unique_ptr<Plugin>              active_;       // foreground (user-visible)
    std::vector<std::unique_ptr<Plugin>> background_;   // manually-started resident plugins
    std::string                          pending_cmd_;  // buffered for plugin_on_cmd pull
    void*                                tick_task_  = nullptr;  // FreeRTOS TaskHandle_t
    void*                                call_mutex_ = nullptr;  // FreeRTOS SemaphoreHandle_t
    volatile bool                        tick_stop_  = false;
    std::atomic<bool>                    pending_stop_{false};
    bool initialised_ = false;
    uint8_t                              plugin_base_depth_ = 0;

    // MIME-type -> plugin-id index for auto-starting handler plugins on demand.
    // Guarded by msg_index_mutex_ (read from the BLE host task). Parallel arrays.
    void*                    msg_index_mutex_ = nullptr;  // FreeRTOS SemaphoreHandle_t
    std::vector<std::string> msg_index_mime_;
    std::vector<std::string> msg_index_id_;
    // External-feature -> plugin-id index, rebuilt together with the MIME index
    // and guarded by the same mutex. Parallel arrays.
    std::vector<std::string> feat_index_name_;
    std::vector<std::string> feat_index_id_;
    std::string              installed_sig_;
    uint32_t                 last_index_refresh_ms_ = 0;
};

}  // namespace cdc::plugin_manager
