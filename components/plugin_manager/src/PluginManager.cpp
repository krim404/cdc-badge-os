#include "plugin_manager/PluginManager.h"
#include "plugin_manager/host_api.h"
#include "plugin_manager/CapabilityChecker.h"
#include "plugin_manager/Plugin.h"
#include "plugin_manager/Prerequisites.h"
#include "plugin_manager/WamrImports.h"
#include "wamr_runtime/Wamr.h"
#include "cdc_ui/I18n.h"
#include "cdc_log.h"
#include "cdc_views/ToastView.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#include "plugin_manager/LockscreenRegistry.h"
#include "plugin_manager/PluginListView.h"
#include "plugin_manager/PluginUiState.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_os_ui/SleepManager.h"

extern "C" {
#include "bh_log.h"
}

extern "C" void plg_ble_pump(void);
extern "C" void plg_ble_on_unload(void* plugin);

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace cdc::plugin_manager {

static const char* TAG = "PLG_MGR";

namespace {

constexpr uint32_t   TICK_INTERVAL_MS = 50;
constexpr uint32_t   TICK_STACK_BYTES = 8192;
constexpr UBaseType_t TICK_PRIORITY    = 5;

void invokeDeinit(Plugin& plugin)
{
    int32_t rc = 0;
    if (!plugin.callI("plugin_deinit", {}, &rc)) {
        LOG_W(TAG, "plugin_deinit missing for %s", plugin.id().c_str());
    } else if (rc != 0) {
        LOG_W(TAG, "plugin_deinit returned %ld for %s",
              static_cast<long>(rc), plugin.id().c_str());
    }
}

// Reflect a plugin's `prevent_sleep` capability into the SleepManager. The id
// string (stable for the plugin's lifetime in RAM) is used as the inhibitor
// reason, so it must be released before the plugin is unloaded.
void applySleepInhibitor(const Plugin& plugin, bool on)
{
    if (!plugin.manifest().capabilities.prevent_sleep) return;
    auto& sm = cdc::ui::SleepManager::instance();
    if (on) {
        sm.addSleepInhibitor(plugin.id().c_str());
    } else {
        sm.removeSleepInhibitor(plugin.id().c_str());
    }
}

struct ScopedLock {
    SemaphoreHandle_t m;
    bool taken = false;
    explicit ScopedLock(SemaphoreHandle_t s, uint32_t timeout_ms = 1000) : m(s) {
        if (m) taken = xSemaphoreTake(m, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    }
    ~ScopedLock() { if (taken && m) xSemaphoreGive(m); }
};

}  // namespace

PluginManager& PluginManager::instance() noexcept
{
    static PluginManager s;
    return s;
}

PluginManager::PluginManager() = default;
PluginManager::~PluginManager() = default;

bool PluginManager::init()
{
    if (initialised_) return true;

    bh_log_set_verbose_level(BH_LOG_LEVEL_FATAL);

    if (!cdc::wamr::init())          { LOG_E(TAG, "WAMR init failed");    return false; }
    if (!PluginStorage::mount())     { LOG_E(TAG, "FAT mount failed");    return false; }
    if (!register_host_imports())    { LOG_E(TAG, "imports failed");      return false; }

    call_mutex_ = xSemaphoreCreateRecursiveMutex();
    if (!call_mutex_) { LOG_E(TAG, "plugin mutex create failed"); return false; }

    auto ids = PluginStorage::listPluginIds();
    LOG_I(TAG, "PluginManager ready: %u plugin(s) installed",
          static_cast<unsigned>(ids.size()));

    initialised_ = true;
    startTickTask();
    loadAutoloadPlugins();
    return true;
}

void PluginManager::deinit()
{
    stopTickTask();
    stopActivePlugin();

    {
        ScopedLock l(static_cast<SemaphoreHandle_t>(call_mutex_));
        for (auto& p : background_) {
            (void)p->callI("plugin_on_exit");
            clearLockscreenRegistrationFor(p.get());
            plg_ble_on_unload(p.get());
            invokeDeinit(*p);
            Prerequisites::release(*p);
            p->unload();
        }
        background_.clear();
    }

    if (call_mutex_) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(call_mutex_));
        call_mutex_ = nullptr;
    }
    unregister_host_imports();
    PluginStorage::unmount();
    cdc::wamr::deinit();
    initialised_ = false;
}

std::vector<std::string> PluginManager::listInstalledIds() const
{
    return PluginStorage::listPluginIds();
}

std::optional<PluginManifest>
PluginManager::getManifest(const std::string& id) const
{
    std::string meta = PluginStorage::metaPath(id);
    auto fp = ::cdc::core::openFile(meta.c_str(), "rb");
    if (!fp) return std::nullopt;
    std::fseek(fp.get(), 0, SEEK_END);
    long n = std::ftell(fp.get());
    if (n <= 0) return std::nullopt;
    std::fseek(fp.get(), 0, SEEK_SET);
    std::string buf(static_cast<size_t>(n), '\0');
    if (std::fread(buf.data(), 1, n, fp.get()) != static_cast<size_t>(n)) {
        return std::nullopt;
    }
    PluginManifest out;
    if (!PluginManifest::parse(buf.data(), buf.size(), out)) return std::nullopt;
    return out;
}

StartResult PluginManager::startPlugin(const std::string& id_ref)
{
    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));
    const std::string id = id_ref;

    if (active_ && active_->id() == id) {
        (void)active_->callI("plugin_on_enter");
        return StartResult::Ok;
    }

    if (active_) {
        (void)active_->callI("plugin_on_exit");
        if (active_->manifest().capabilities.background) {
            background_.push_back(std::move(active_));
        } else {
            clearLockscreenRegistrationFor(active_.get());
            plg_ble_on_unload(active_.get());
            invokeDeinit(*active_);
            Prerequisites::release(*active_);
            active_->unload();
        }
        active_.reset();
    }

    // If the plugin is already running in the background, promote it
    // to foreground without reloading.
    auto it = std::find_if(background_.begin(), background_.end(),
                           [&](const std::unique_ptr<Plugin>& p) {
                               return p && p->id() == id;
                           });
    if (it != background_.end()) {
        auto plugin = std::move(*it);
        background_.erase(it);

        int32_t enter_rc = 0;
        if (!plugin->callI("plugin_on_enter", {}, &enter_rc)) {
            LOG_E(TAG, "plugin_on_enter missing for %s", id.c_str());
            background_.push_back(std::move(plugin));   // keep it as background
            return StartResult::PluginOnEnterFailed;
        }
        active_ = std::move(plugin);
        LOG_I(TAG, "plugin %s promoted bg->fg", id.c_str());
        return StartResult::Ok;
    }

    auto mf_opt = getManifest(id);
    if (!mf_opt) {
        LOG_W(TAG, "manifest missing/invalid for %s", id.c_str());
        return StartResult::ManifestInvalid;
    }
    const PluginManifest& mf = *mf_opt;

    auto check = CapabilityChecker::validate(mf);
    if (!check.ok()) {
        LOG_W(TAG, "capability check failed for %s: %s",
              id.c_str(), check.detail.c_str());
        return StartResult::CapabilityRejected;
    }

    auto plugin = std::make_unique<Plugin>();
    if (!plugin->load(id, mf)) {
        LOG_E(TAG, "Plugin::load failed for %s", id.c_str());
        return StartResult::WamrLoadFailed;
    }
    plugin->loadLangOverlay();

    int32_t init_rc = 0;
    if (!plugin->callI("plugin_init", {}, &init_rc) || init_rc != 0) {
        LOG_E(TAG, "plugin_init failed for %s (rc=%ld)", id.c_str(),
              static_cast<long>(init_rc));
        plugin->unload();
        return StartResult::PluginInitFailed;
    }

    std::string failed_name, on_fail;
    PrereqResult pr = Prerequisites::walk(*plugin, failed_name, on_fail);
    if (pr == PrereqResult::HardFailed) {
        LOG_E(TAG, "prerequisite '%s' aborted start of %s",
              failed_name.c_str(), id.c_str());
        Prerequisites::release(*plugin);
        invokeDeinit(*plugin);
        plugin->unload();
        return StartResult::PrerequisiteFailed;
    }
    if (pr == PrereqResult::SoftFailed) {
        LOG_W(TAG, "prerequisite '%s' soft-failed for %s, continuing",
              failed_name.c_str(), id.c_str());
    }

    static cdc::ui::ToastView s_loading_toast;
    char loading_msg[80];
    {
        const auto& meta = plugin->manifest().i18n_meta;
        const char* display = id.c_str();
        auto it = meta.find("name");
        if (it != meta.end() && !it->second.by_lang.empty()) {
            display = it->second.by_lang.begin()->second.c_str();
        }
        std::snprintf(loading_msg, sizeof(loading_msg), "%s\n%s",
                      cdc::ui::tr("core.plugin_loading"), display);
    }
    s_loading_toast.init(loading_msg, cdc::ui::ToastView::Icon::TASK, 0, true);
    cdc::ui::ViewStack::instance().showModal(&s_loading_toast);

    auto hide_loading_if_top = []() {
        auto& vs = cdc::ui::ViewStack::instance();
        if (vs.getModal() == &s_loading_toast) {
            vs.hideModal();
        }
    };

    int32_t enter_rc = 0;
    if (!plugin->callI("plugin_on_enter", {}, &enter_rc)) {
        hide_loading_if_top();
        if (plugin->hasExport("plugin_on_enter")) {
            LOG_E(TAG, "plugin_on_enter trapped for %s", id.c_str());
        } else {
            LOG_E(TAG, "plugin_on_enter export missing for %s", id.c_str());
        }
        Prerequisites::release(*plugin);
        invokeDeinit(*plugin);
        plugin->unload();
        return StartResult::PluginOnEnterFailed;
    }
    hide_loading_if_top();
    if (enter_rc != 0) {
        LOG_W(TAG, "plugin_on_enter returned %ld for %s",
              static_cast<long>(enter_rc), id.c_str());
    }

    active_ = std::move(plugin);
    applySleepInhibitor(*active_, true);
    LOG_I(TAG, "plugin %s started", id.c_str());
    return StartResult::Ok;
}

void PluginManager::requestStopActivePlugin()
{
    pending_stop_.store(true, std::memory_order_release);
}

bool PluginManager::stopActivePlugin()
{
    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));
    if (!active_) return false;
    LOG_I(TAG, "stopping plugin %s", active_->id().c_str());
    (void)active_->callI("plugin_on_exit");

    // If the plugin is also a background service, demote it back instead of
    // unloading.
    if (active_->manifest().capabilities.background) {
        background_.push_back(std::move(active_));
        active_.reset();
        return true;
    }

    applySleepInhibitor(*active_, false);
    clearLockscreenRegistrationFor(active_.get());
    invokeDeinit(*active_);
    Prerequisites::release(*active_);
    active_->unload();
    active_.reset();
    PluginUiState::instance().resetForPluginStop();
    return true;
}

bool PluginManager::unloadFromRam(const std::string& id)
{
    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));
    if (active_ && active_->id() == id) {
        LOG_I(TAG, "unloading active plugin %s from RAM", id.c_str());
        (void)active_->callI("plugin_on_exit");
        while (cdc::ui::ViewStack::instance().depth() > 1) {
            cdc::ui::ViewStack::instance().pop();
        }
        PluginUiState::instance().resetForPluginStop();
        applySleepInhibitor(*active_, false);
        clearLockscreenRegistrationFor(active_.get());
        invokeDeinit(*active_);
        Prerequisites::release(*active_);
        active_->unload();
        active_.reset();
        return true;
    }
    auto it = std::find_if(background_.begin(), background_.end(),
                           [&](const std::unique_ptr<Plugin>& p) {
                               return p && p->id() == id;
                           });
    if (it == background_.end()) return false;
    LOG_I(TAG, "unloading background plugin %s from RAM", id.c_str());
    applySleepInhibitor(**it, false);
    clearLockscreenRegistrationFor(it->get());
    plg_ble_on_unload(it->get());
    invokeDeinit(**it);
    Prerequisites::release(**it);
    (*it)->unload();
    background_.erase(it);
    return true;
}

bool PluginManager::reloadBackgroundPlugin(const std::string& id)
{
    auto manifest = getManifest(id);
    if (!manifest || !manifest->capabilities.background) return false;

    // background:true is not "start at boot": only refresh an instance that is
    // already running in the background so it picks up a freshly uploaded
    // binary. An idle plugin stays unloaded until the user starts it manually.
    if (!isRunningInBackground(id)) return false;

    (void)unloadFromRam(id);

    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));
    if (!loadIntoBackground(id, *manifest)) return false;
    LOG_I(TAG, "background plugin %s reloaded", id.c_str());
    return true;
}

bool PluginManager::loadIntoBackground(const std::string& id, const PluginManifest& mf)
{
    auto plugin = std::make_unique<Plugin>();
    if (!plugin->load(id, mf)) {
        LOG_E(TAG, "bg load %s: load failed", id.c_str());
        return false;
    }
    plugin->loadLangOverlay();
    int32_t init_rc = 0;
    if (!plugin->callI("plugin_init", {}, &init_rc) || init_rc != 0) {
        LOG_E(TAG, "bg load %s: plugin_init failed (rc=%ld)", id.c_str(),
              static_cast<long>(init_rc));
        plugin->unload();
        return false;
    }
    std::string failed_name, on_fail;
    PrereqResult pr = Prerequisites::walk(*plugin, failed_name, on_fail);
    if (pr == PrereqResult::HardFailed) {
        LOG_E(TAG, "bg load %s: prereq '%s' aborted", id.c_str(), failed_name.c_str());
        Prerequisites::release(*plugin);
        invokeDeinit(*plugin);
        plugin->unload();
        return false;
    }
    background_.push_back(std::move(plugin));
    applySleepInhibitor(*background_.back(), true);
    return true;
}

void PluginManager::loadAutoloadPlugins()
{
    auto ids = PluginStorage::listPluginIds();
    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));
    for (const auto& id : ids) {
        auto mf = getManifest(id);
        if (!mf || !mf->capabilities.autoload) continue;
        if (isLoaded(id)) continue;

        auto check = CapabilityChecker::validate(*mf);
        if (!check.ok()) {
            LOG_W(TAG, "autoload %s rejected: %s", id.c_str(), check.detail.c_str());
            continue;
        }
        if (loadIntoBackground(id, *mf)) {
            LOG_I(TAG, "autoloaded plugin %s (resident)", id.c_str());
        } else {
            LOG_W(TAG, "autoload of %s failed", id.c_str());
        }
    }
}

uint8_t PluginManager::getLockscreenItems(LockscreenItem* out, uint8_t max) const
{
    LockscreenRegistration regs[8];
    const uint8_t n = collectLockscreenItems(regs, sizeof(regs) / sizeof(regs[0]));
    const uint8_t copy = n < max ? n : max;
    const std::string lang = ::cdc::ui::I18n::instance().getLanguageCode();

    for (uint8_t i = 0; i < copy; ++i) {
        Plugin* p = static_cast<Plugin*>(regs[i].plugin);
        out[i].plugin    = p;
        out[i].action_id = regs[i].action_id;

        const char* label = nullptr;
        if (p) {
            label = p->trKey(regs[i].label_key);                 // lang overlay
            if (!label) {
                // Fallback: manifest i18n.strings.<key>.<lang|en|first>
                const auto& strings = p->manifest().i18n_strings;
                auto it = strings.find(regs[i].label_key);
                if (it != strings.end()) {
                    auto pick = [&](const std::string& l) -> const char* {
                        auto lit = it->second.by_lang.find(l);
                        return lit != it->second.by_lang.end() ? lit->second.c_str() : nullptr;
                    };
                    label = pick(lang);
                    if (!label) label = pick("en");
                    if (!label && !it->second.by_lang.empty())
                        label = it->second.by_lang.begin()->second.c_str();
                }
            }
        }
        if (!label) label = regs[i].label_key;
        std::strncpy(out[i].label, label, sizeof(out[i].label) - 1);
        out[i].label[sizeof(out[i].label) - 1] = '\0';
    }
    return copy;
}

void PluginManager::triggerLockscreenItem(const LockscreenItem& item)
{
    dispatchActionTo(item.plugin, item.action_id, 0, 0);
}

bool        PluginManager::hasActivePlugin() const noexcept { return active_ != nullptr; }
std::string PluginManager::activePluginId()  const { return active_ ? active_->id() : std::string{}; }

bool PluginManager::isLoaded(const std::string& id) const
{
    if (active_ && active_->id() == id) return true;
    for (const auto& p : background_) if (p->id() == id) return true;
    return false;
}

bool PluginManager::isRunningInBackground(const std::string& id) const
{
    for (const auto& p : background_) if (p->id() == id) return true;
    return false;
}

bool PluginManager::activePluginIsBackground() const
{
    return active_ && active_->manifest().capabilities.background;
}

bool PluginManager::activePluginPreventsSleep() const
{
    return active_ && active_->manifest().capabilities.prevent_sleep;
}

bool PluginManager::hasBackgroundPlugin() const noexcept
{
    return !background_.empty();
}

void PluginManager::dispatchButton(uint32_t button_code)
{
    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));
    if (!active_) return;
    int32_t rc = 0;
    (void)active_->callI("plugin_on_button",
                         {static_cast<int32_t>(button_code)}, &rc);
}

void PluginManager::dispatchAction(uint32_t action_id, uint32_t idx, uint32_t user_data)
{
    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));
    if (!active_) return;
    int32_t rc = 0;
    (void)active_->callI("plugin_on_action",
                         {static_cast<int32_t>(action_id),
                          static_cast<int32_t>(idx),
                          static_cast<int32_t>(user_data)}, &rc);
}

void PluginManager::dispatchActionTo(Plugin* plugin, uint32_t action_id,
                                     uint32_t idx, uint32_t user_data)
{
    if (!plugin) return;
    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));

    bool found = (active_.get() == plugin);
    if (!found) {
        for (auto& p : background_) if (p.get() == plugin) { found = true; break; }
    }
    if (!found) return;  // stale subscription, plugin no longer loaded

    int32_t rc = 0;
    (void)plugin->callI("plugin_on_action",
                        {static_cast<int32_t>(action_id),
                         static_cast<int32_t>(idx),
                         static_cast<int32_t>(user_data)}, &rc);
}

void PluginManager::dispatchTick(uint64_t uptime_ms)
{
    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));
    int32_t rc = 0;
    const int32_t hi = static_cast<int32_t>(uptime_ms >> 32);
    const int32_t lo = static_cast<int32_t>(uptime_ms & 0xFFFFFFFFu);
    if (active_) (void)active_->callI("plugin_on_tick", {lo, hi}, &rc);
    for (auto& p : background_) {
        (void)p->callI("plugin_on_tick", {lo, hi}, &rc);
    }
    plg_ble_pump();
}

void PluginManager::dispatchEventAll(uint32_t event_type, uint32_t value)
{
    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));
    int32_t rc = 0;
    if (active_) {
        (void)active_->callI("plugin_on_event",
                             {static_cast<int32_t>(event_type),
                              static_cast<int32_t>(value)}, &rc);
    }
    for (auto& p : background_) {
        (void)p->callI("plugin_on_event",
                       {static_cast<int32_t>(event_type),
                        static_cast<int32_t>(value)}, &rc);
    }
}

bool PluginManager::dispatchCmd(const std::string& id, const char* cmd, size_t len)
{
    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));

    Plugin* target = (active_ && active_->id() == id) ? active_.get() : nullptr;
    if (!target) {
        for (auto& p : background_) if (p->id() == id) { target = p.get(); break; }
    }
    if (!target || !target->hasExport("plugin_on_cmd")) return false;

    pending_cmd_.assign(cmd ? cmd : "", len);
    int32_t rc = 0;
    (void)target->callI("plugin_on_cmd", {static_cast<int32_t>(len)}, &rc);
    pending_cmd_.clear();
    return true;
}

int PluginManager::consumeCmd(char* out, size_t out_size)
{
    if (!out || out_size == 0) return HOST_ERR_INVALID_ARG;
    size_t n = pending_cmd_.size();
    if (n >= out_size) n = out_size - 1;
    std::memcpy(out, pending_cmd_.data(), n);
    out[n] = '\0';
    pending_cmd_.clear();
    return static_cast<int>(n);
}

void PluginManager::forEachPlugin(const std::function<bool(Plugin&)>& visitor)
{
    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));
    if (active_) { if (!visitor(*active_)) return; }
    for (auto& p : background_) {
        if (!visitor(*p)) return;
    }
}

void PluginManager::reloadActiveLangOverlay()
{
    ScopedLock lock(static_cast<SemaphoreHandle_t>(call_mutex_));
    if (active_) active_->loadLangOverlay();
    for (auto& p : background_) p->loadLangOverlay();
}

void PluginManager::tickTaskTrampoline(void* arg)
{
    static_cast<PluginManager*>(arg)->tickTaskLoop();
    vTaskDelete(nullptr);
}

void PluginManager::tickTaskLoop()
{
    TickType_t last = xTaskGetTickCount();
    while (!tick_stop_) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_INTERVAL_MS));
        if (tick_stop_) break;
        if (pending_stop_.exchange(false, std::memory_order_acq_rel)) {
            cdc::ui::IView* top = cdc::ui::ViewStack::instance().current();
            PluginListView* listView = PluginListView::active();
            if (top && listView && top == static_cast<cdc::ui::IView*>(static_cast<cdc::ui::ViewBase*>(listView))) {
                stopActivePlugin();
            }
        }
        dispatchTick(esp_timer_get_time() / 1000);
    }
}

void PluginManager::startTickTask()
{
    if (tick_task_) return;
    tick_stop_ = false;
    TaskHandle_t handle = nullptr;
    if (xTaskCreate(&PluginManager::tickTaskTrampoline,
                    "plg_tick", TICK_STACK_BYTES, this,
                    TICK_PRIORITY, &handle) != pdPASS) {
        LOG_E(TAG, "failed to spawn plg_tick task");
        return;
    }
    tick_task_ = handle;
}

void PluginManager::stopTickTask()
{
    if (!tick_task_) return;
    tick_stop_ = true;
    // The task self-deletes after its next wake-up.
    // Wait briefly so it doesn't reference us after we go away.
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL_MS * 2));
    tick_task_ = nullptr;
}

}  // namespace cdc::plugin_manager
