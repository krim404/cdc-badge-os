/**
 * \file PluginManager.h
 * \brief Discovers, loads, runs and unloads WASM plugins on the badge.
 *
 * One PluginManager instance, registered as a cdc_core service. At most one
 * foreground plugin (the one the user is currently looking at) runs at any
 * given time. A plugin declaring `capabilities.background = true` keeps
 * running (and ticking in parallel with the foreground) after the user leaves
 * its view, instead of being unloaded. It is never auto-started at boot: the
 * user starts it manually and can force-stop it again via \ref unloadFromRam.
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
};

class PluginManager {
public:
    [[nodiscard]] static PluginManager& instance() noexcept;

    [[nodiscard]] bool init();
    void deinit();

    [[nodiscard]] std::vector<std::string>     listInstalledIds() const;
    [[nodiscard]] std::optional<PluginManifest> getManifest(const std::string& id) const;

    [[nodiscard]] StartResult startPlugin(const std::string& id);
    bool                      stopActivePlugin();
    /// Unload a plugin from RAM regardless of foreground/background slot,
    /// keeping its files on disk. This is the forced-stop entry point for a
    /// resident background plugin. Returns true if the plugin was found and
    /// unloaded.
    bool                      unloadFromRam(const std::string& id);
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

    /// Load, init and run prerequisites for a plugin straight into the
    /// background slot (headless: no foreground view, no plugin_on_enter).
    /// Caller must hold call_mutex_. Returns true on success.
    bool loadIntoBackground(const std::string& id, const PluginManifest& mf);

    /// At boot, start every installed plugin whose manifest declares
    /// `capabilities.autoload` as a resident background instance. Plugins
    /// without the flag stay unloaded until the user starts them manually.
    void loadAutoloadPlugins();

    std::unique_ptr<Plugin>              active_;       // foreground (user-visible)
    std::vector<std::unique_ptr<Plugin>> background_;   // manually-started resident plugins
    std::string                          pending_cmd_;  // buffered for plugin_on_cmd pull
    void*                                tick_task_  = nullptr;  // FreeRTOS TaskHandle_t
    void*                                call_mutex_ = nullptr;  // FreeRTOS SemaphoreHandle_t
    volatile bool                        tick_stop_  = false;
    std::atomic<bool>                    pending_stop_{false};
    bool initialised_ = false;
};

}  // namespace cdc::plugin_manager
