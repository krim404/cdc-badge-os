/**
 * \file host_api_ext_feature.cpp
 * \brief External-feature host API: one plugin invokes a named feature
 *        provided by another installed plugin ("Open with" semantics).
 *
 * Threading mirrors host_api_msg.cpp. host_ext_feature_use runs inside the
 * caller's WASM frame and only stashes the job (it must never start the
 * provider inline - startPlugin would tear down the executing caller
 * instance). plg_ext_feature_pump() runs on the plugin tick task, switches
 * the provider to the foreground and fires its handler action; the payload is
 * consumable for that dispatch only. The provider later reports the outcome
 * via host_ext_feature_result, which routes the status back to the caller as
 * an action - if the caller is still loaded.
 *
 * All state below is touched from WASM frames (tick task or UI dispatch) and
 * the pump; a single mutex keeps the job slot consistent across those paths.
 */

#include "plugin_manager/host_api.h"
#include "plugin_manager/ExtFeatureName.h"
#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginManager.h"
#include "cdc_views/MessageBox.h"
#include "cdc_ui/I18n.h"
#include "cdc_log.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstring>
#include <string>

namespace pm = cdc::plugin_manager;

extern "C" void* plg_get_active_plugin(void);

namespace {

const char* TAG = "PLG_FEAT";

constexpr uint8_t MAX_HANDLERS = 4;

SemaphoreHandle_t s_lock = nullptr;
void lock_init() { if (!s_lock) s_lock = xSemaphoreCreateMutex(); }
struct Guard {
    bool held = false;
    Guard()  { if (s_lock) held = (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE); }
    ~Guard() { if (held)  xSemaphoreGive(s_lock); }
};

// Provider handler table: registered from plugin_init, dropped on unload.
struct Handler {
    bool     used = false;
    void*    plugin = nullptr;
    char     feature[HOST_EXT_FEATURE_NAME_MAX] = {};
    uint32_t action_id = 0;
};
Handler s_handlers[MAX_HANDLERS];

// Single job slot walking Idle -> Stashed -> Delivered -> Idle.
struct FeatureJob {
    enum class St : uint8_t { Idle, Stashed, Delivered };
    St          st = St::Idle;
    char        feature[HOST_EXT_FEATURE_NAME_MAX] = {};
    std::string provider_id;
    void*       caller = nullptr;   // Plugin*; may dangle after the handoff
    std::string caller_id;          // for isLoaded() re-check before dispatch
    uint32_t    status_action_id = 0;
    uint8_t*    data = nullptr;     // PSRAM, owned by the slot
    uint32_t    len = 0;
};
FeatureJob s_job;

// Payload of the job currently delivered to a handler action (consumable for
// that dispatch only, like host_msg_consume).
char     s_cur_feature[HOST_EXT_FEATURE_NAME_MAX] = {};
uint32_t s_cur_len = 0;
uint8_t* s_cur_data = nullptr;

void job_free_locked() {
    if (s_job.data) { heap_caps_free(s_job.data); s_job.data = nullptr; }
    s_job.len = 0;
    s_job.st = FeatureJob::St::Idle;
    s_job.caller = nullptr;
    s_job.caller_id.clear();
    s_job.provider_id.clear();
    s_job.status_action_id = 0;
    s_job.feature[0] = '\0';
}

int find_handler(void* plugin, const char* feature) {
    for (int i = 0; i < MAX_HANDLERS; ++i) {
        if (s_handlers[i].used && s_handlers[i].plugin == plugin &&
            std::strncmp(s_handlers[i].feature, feature, HOST_EXT_FEATURE_NAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

int find_handler_by_feature(const char* feature) {
    for (int i = 0; i < MAX_HANDLERS; ++i) {
        if (s_handlers[i].used &&
            std::strncmp(s_handlers[i].feature, feature, HOST_EXT_FEATURE_NAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

// Fire the caller's status action if the caller plugin is still loaded under
// the same pointer. Must be called WITHOUT holding s_lock (dispatch enters WASM).
void report_to_caller(void* caller, const std::string& caller_id,
                      uint32_t action_id, int32_t status) {
    if (!caller || !action_id || caller_id.empty()) return;
    if (!pm::PluginManager::instance().isLoaded(caller_id)) return;
    // dispatchActionTo re-validates pointer membership in the fore/background
    // slots, so a reused address for a different plugin is dropped there.
    pm::PluginManager::instance().dispatchActionTo(
        static_cast<pm::Plugin*>(caller), action_id, 0,
        static_cast<uint32_t>(status));
}

}  // namespace

extern "C" {

int host_ext_feature_available(const char* feature) {
    if (!pm::isValidExtFeatureName(feature)) return HOST_ERR_INVALID_ARG;
    return pm::PluginManager::instance().featureInstalled(feature)
               ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_ext_feature_use(const char* feature, const uint8_t* data, size_t len,
                         uint32_t status_action_id) {
    if (!pm::isValidExtFeatureName(feature)) return HOST_ERR_INVALID_ARG;
    if (len > HOST_EXT_FEATURE_PAYLOAD_MAX) return HOST_ERR_INVALID_ARG;
    if (!data && len > 0) return HOST_ERR_INVALID_ARG;

    auto* caller = static_cast<pm::Plugin*>(plg_get_active_plugin());
    if (!caller) return HOST_ERR_GENERIC;

    std::string provider = pm::PluginManager::instance().featureProviderId(feature);
    if (provider.empty()) {
        cdc::ui::showMessage(cdc::ui::tr("core.feature_missing"),
                             cdc::ui::MessageIcon::ERROR, 2500);
        return HOST_ERR_NOT_FOUND;
    }
    if (provider == caller->id()) return HOST_ERR_INVALID_ARG;

    uint8_t* copy = nullptr;
    if (len > 0) {
        copy = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
        if (!copy) return HOST_ERR_NO_MEMORY;
        std::memcpy(copy, data, len);
    }

    lock_init();
    Guard g;
    if (s_job.st != FeatureJob::St::Idle) {
        if (copy) heap_caps_free(copy);
        return HOST_ERR_BUSY;
    }
    s_job.st = FeatureJob::St::Stashed;
    std::strncpy(s_job.feature, feature, sizeof(s_job.feature) - 1);
    s_job.feature[sizeof(s_job.feature) - 1] = '\0';
    s_job.provider_id = std::move(provider);
    s_job.caller = caller;
    s_job.caller_id = caller->id();
    s_job.status_action_id = status_action_id;
    s_job.data = copy;
    s_job.len = static_cast<uint32_t>(len);
    return HOST_OK;
}

int host_ext_feature_register_handler(const char* feature, uint32_t action_id) {
    if (!pm::isValidExtFeatureName(feature)) return HOST_ERR_INVALID_ARG;
    auto* p = static_cast<pm::Plugin*>(plg_get_active_plugin());
    if (!p) return HOST_ERR_GENERIC;

    bool declared = false;
    for (const auto& name : p->manifest().capabilities.provides) {
        if (name == feature) { declared = true; break; }
    }
    if (!declared) return HOST_ERR_NO_CAPABILITY;

    int idx = find_handler(p, feature);
    if (idx < 0) {
        for (int i = 0; i < MAX_HANDLERS; ++i) {
            if (!s_handlers[i].used) { idx = i; break; }
        }
    }
    if (idx < 0) return HOST_ERR_NO_MEMORY;

    Handler& h = s_handlers[idx];
    h.used = true;
    h.plugin = p;
    std::strncpy(h.feature, feature, sizeof(h.feature) - 1);
    h.feature[sizeof(h.feature) - 1] = '\0';
    h.action_id = action_id;
    return HOST_OK;
}

int host_ext_feature_consume(uint8_t* buf, size_t buf_size,
                             char* feature_out, size_t feature_size) {
    if (!buf || buf_size == 0) return HOST_ERR_INVALID_ARG;
    uint32_t n = s_cur_len;
    if (n > buf_size) n = static_cast<uint32_t>(buf_size);
    if (n > 0 && s_cur_data) std::memcpy(buf, s_cur_data, n);
    if (feature_out && feature_size) {
        std::strncpy(feature_out, s_cur_feature, feature_size - 1);
        feature_out[feature_size - 1] = '\0';
    }
    return static_cast<int>(n);
}

int host_ext_feature_result(int32_t status_code) {
    auto* p = plg_get_active_plugin();
    if (!p) return HOST_ERR_GENERIC;

    void*       caller = nullptr;
    std::string caller_id;
    uint32_t    aid = 0;
    {
        Guard g;
        if (s_job.st != FeatureJob::St::Delivered) return HOST_ERR_NOT_FOUND;
        int idx = find_handler_by_feature(s_job.feature);
        if (idx < 0 || s_handlers[idx].plugin != p) return HOST_ERR_NOT_FOUND;
        caller = s_job.caller;
        caller_id = s_job.caller_id;
        aid = s_job.status_action_id;
        job_free_locked();
    }
    report_to_caller(caller, caller_id, aid, status_code);
    return HOST_OK;
}

// Plugin tick task: start the provider for a stashed job and deliver it.
void plg_ext_feature_pump(void) {
    char     feature[HOST_EXT_FEATURE_NAME_MAX];
    std::string provider_id;
    {
        Guard g;
        if (s_job.st != FeatureJob::St::Stashed) return;
        std::memcpy(feature, s_job.feature, sizeof(feature));
        provider_id = s_job.provider_id;
    }

    auto& mgr = pm::PluginManager::instance();
    bool started = mgr.activePluginId() == provider_id;
    if (!started) started = (mgr.startPlugin(provider_id) == pm::StartResult::Ok);

    int idx = started ? find_handler_by_feature(feature) : -1;
    if (idx < 0) {
        // Provider failed to start or never registered its handler: fail the
        // job back to the caller.
        void*       caller = nullptr;
        std::string caller_id;
        uint32_t    aid = 0;
        {
            Guard g;
            if (s_job.st != FeatureJob::St::Stashed) return;
            caller = s_job.caller;
            caller_id = s_job.caller_id;
            aid = s_job.status_action_id;
            job_free_locked();
        }
        LOG_W(TAG, "feature '%s': provider %s unavailable", feature, provider_id.c_str());
        report_to_caller(caller, caller_id, aid, HOST_EXT_FEATURE_STATUS_ERROR);
        return;
    }

    uint32_t len = 0;
    {
        Guard g;
        if (s_job.st != FeatureJob::St::Stashed) return;
        s_job.st = FeatureJob::St::Delivered;
        s_cur_data = s_job.data;
        s_cur_len = s_job.len;
        std::memcpy(s_cur_feature, s_job.feature, sizeof(s_cur_feature));
        len = s_job.len;
    }
    mgr.dispatchActionTo(static_cast<pm::Plugin*>(s_handlers[idx].plugin),
                         s_handlers[idx].action_id, 0, len);
    // Payload stays owned by the job slot until host_ext_feature_result, but
    // it is only consumable during the dispatch above.
    Guard g;
    s_cur_len = 0;
    s_cur_data = nullptr;
    s_cur_feature[0] = '\0';
}

// Drop handlers and fail/clear the job when one of its plugins unloads.
void plg_ext_feature_on_unload(void* plugin) {
    for (int i = 0; i < MAX_HANDLERS; ++i) {
        if (s_handlers[i].used && s_handlers[i].plugin == plugin) {
            s_handlers[i] = Handler{};
        }
    }

    lock_init();
    void*       caller = nullptr;
    std::string caller_id;
    uint32_t    aid = 0;
    bool        provider_died = false;
    {
        Guard g;
        if (s_job.st == FeatureJob::St::Idle) return;
        if (s_job.caller == plugin) {
            // Caller gone: keep the job (the provider may still print), but
            // drop the status routing.
            s_job.caller = nullptr;
            s_job.caller_id.clear();
            s_job.status_action_id = 0;
            return;
        }
        if (s_job.st == FeatureJob::St::Delivered) {
            int idx = find_handler_by_feature(s_job.feature);
            // Handler entries for `plugin` were cleared above; a Delivered job
            // whose feature no longer resolves lost its provider.
            provider_died = (idx < 0);
        }
        if (!provider_died) return;
        caller = s_job.caller;
        caller_id = s_job.caller_id;
        aid = s_job.status_action_id;
        job_free_locked();
    }
    report_to_caller(caller, caller_id, aid, HOST_EXT_FEATURE_STATUS_ERROR);
}

}  // extern "C"
