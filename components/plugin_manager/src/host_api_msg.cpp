/**
 * \file host_api_msg.cpp
 * \brief Message-transfer host API for plugins: register MIME handlers, pull a
 *        received payload, and push a typed payload to a nearby badge.
 *
 * Threading mirrors the BLE consume idiom. The cdc_msg deliver callback runs on
 * the main task; it only stashes the payload + sets a pending flag (never calls
 * into WASM). plg_msg_pump() runs on the plugin tick task, moves the stash into
 * a per-action slot and fires the plugin's action, which then pulls the bytes
 * with host_msg_consume. The handler table and the per-action slot are only
 * touched on the tick task, so only the pending stash needs a mutex (it crosses
 * the main <-> tick boundary). Registration therefore never holds that mutex
 * while calling into cdc_msg, avoiding a lock-ordering deadlock.
 */

#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginManager.h"
#include "cdc_msg/MessageTransfer.h"

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstring>

namespace pm = cdc::plugin_manager;
namespace msg = cdc::msg;

extern "C" void* plg_get_active_plugin(void);

namespace {

constexpr uint8_t MAX_HANDLERS = 8;

// Guards only the pending inbound stash (main task <-> tick task).
SemaphoreHandle_t s_lock = nullptr;
void lock_init() { if (!s_lock) s_lock = xSemaphoreCreateMutex(); }
struct Guard {
    bool held = false;
    Guard()  { if (s_lock) held = (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE); }
    ~Guard() { if (held)  xSemaphoreGive(s_lock); }
};

// Handler table: tick-task only, no lock needed.
struct Handler {
    bool     used = false;
    void*    plugin = nullptr;
    char     mime[HOST_MSG_MIME_MAX] = {};
    uint32_t action_id = 0;
};
Handler s_handlers[MAX_HANDLERS];

// Pending inbound delivery: written by on_deliver (main task), drained by pump.
struct Pending {
    bool     valid = false;
    void*    plugin = nullptr;
    uint32_t action_id = 0;
    char     mime[HOST_MSG_MIME_MAX] = {};
    uint32_t len = 0;
    uint8_t  buf[HOST_MSG_PAYLOAD_MAX];
};
EXT_RAM_BSS_ATTR Pending s_pending;

// Payload currently delivered to an action (tick-task only, read by consume).
char     s_cur_mime[HOST_MSG_MIME_MAX] = {};
uint32_t s_cur_len = 0;
EXT_RAM_BSS_ATTR uint8_t s_cur_buf[HOST_MSG_PAYLOAD_MAX];

bool msg_allowed() {
    auto* p = static_cast<pm::Plugin*>(plg_get_active_plugin());
    if (!p) return false;
    const auto& caps = p->manifest().capabilities;
    return caps.ble && !caps.message_types.empty();
}

int find_handler(void* plugin, const char* mime) {
    for (int i = 0; i < MAX_HANDLERS; ++i) {
        if (s_handlers[i].used && s_handlers[i].plugin == plugin &&
            std::strncmp(s_handlers[i].mime, mime, HOST_MSG_MIME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

// cdc_msg deliver callback. Main task; stash only, never enter WASM.
bool on_deliver(void* plugin, uint32_t action_id, const uint8_t* data, uint32_t len,
                const char* mime) {
    if (!data || len == 0 || len > HOST_MSG_PAYLOAD_MAX) return false;
    Guard g;
    if (s_pending.valid) return false;  // previous delivery not yet pumped
    s_pending.valid = true;
    s_pending.plugin = plugin;
    s_pending.action_id = action_id;
    std::strncpy(s_pending.mime, mime ? mime : "", sizeof(s_pending.mime) - 1);
    s_pending.mime[sizeof(s_pending.mime) - 1] = '\0';
    s_pending.len = len;
    std::memcpy(s_pending.buf, data, len);
    return true;
}

// Payload for a MIME type handled by a not-yet-loaded plugin: stashed on the
// main task, then loaded + delivered on the plugin task (handle_deferred).
struct DeferredMsg {
    bool     valid = false;
    char     mime[HOST_MSG_MIME_MAX] = {};
    char     peerName[msg::kNameBufSize] = {};
    uint32_t len = 0;
    uint8_t  buf[HOST_MSG_PAYLOAD_MAX];
};
EXT_RAM_BSS_ATTR DeferredMsg s_deferred;

int find_handler_by_mime(const char* mime) {
    for (int i = 0; i < MAX_HANDLERS; ++i) {
        if (s_handlers[i].used && std::strncmp(s_handlers[i].mime, mime, HOST_MSG_MIME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

// cdc_msg deferred-deliver callback (main task): stash for the plugin task.
bool stash_deferred(const uint8_t* data, uint32_t len, const char* mime, const char* peerName) {
    if (!data || len == 0 || len > HOST_MSG_PAYLOAD_MAX) return false;
    Guard g;
    if (s_deferred.valid) return false;
    s_deferred.valid = true;
    std::strncpy(s_deferred.mime, mime ? mime : "", sizeof(s_deferred.mime) - 1);
    s_deferred.mime[sizeof(s_deferred.mime) - 1] = '\0';
    std::strncpy(s_deferred.peerName, peerName ? peerName : "", sizeof(s_deferred.peerName) - 1);
    s_deferred.peerName[sizeof(s_deferred.peerName) - 1] = '\0';
    s_deferred.len = len;
    std::memcpy(s_deferred.buf, data, len);
    return true;
}

// Plugin task: load the handler plugin for a stashed deferred message, then
// deliver the payload to its just-registered action.
void handle_deferred() {
    char     mime[HOST_MSG_MIME_MAX];
    uint32_t len;
    {
        Guard g;
        if (!s_deferred.valid) return;
        std::memcpy(mime, s_deferred.mime, sizeof(mime));
        std::memcpy(s_cur_buf, s_deferred.buf, s_deferred.len);
        std::memcpy(s_cur_mime, s_deferred.mime, sizeof(s_cur_mime));
        len = s_deferred.len;
        s_cur_len = 0;  // not consumable until the handler action actually fires
        s_deferred.valid = false;
    }
    if (pm::PluginManager::instance().activateForMessageType(mime)) {
        int idx = find_handler_by_mime(mime);
        if (idx >= 0) {
            s_cur_len = len;  // payload becomes consumable for this dispatch only
            pm::PluginManager::instance().dispatchActionTo(
                static_cast<pm::Plugin*>(s_handlers[idx].plugin), s_handlers[idx].action_id, 0, len);
        }
    }
    s_cur_len = 0;
    s_cur_mime[0] = '\0';
}

}  // namespace

extern "C" {

int host_msg_register_handler(const char* mime_type, uint32_t action_id) {
    if (!msg_allowed()) return HOST_ERR_NO_CAPABILITY;
    if (!mime_type || !mime_type[0] ||
        strnlen(mime_type, HOST_MSG_MIME_MAX) >= HOST_MSG_MIME_MAX) {
        return HOST_ERR_INVALID_ARG;
    }
    void* plugin = plg_get_active_plugin();
    lock_init();

    // The cdc_msg registry holds one handler per MIME type. Refuse a second,
    // different plugin claiming a type another loaded plugin already owns,
    // rather than silently clobbering it (and later cross-unregistering it).
    for (int i = 0; i < MAX_HANDLERS; ++i) {
        if (s_handlers[i].used && s_handlers[i].plugin != plugin &&
            std::strncmp(s_handlers[i].mime, mime_type, HOST_MSG_MIME_MAX) == 0) {
            return HOST_ERR_BUSY;
        }
    }

    int idx = find_handler(plugin, mime_type);
    if (idx < 0) {
        for (int i = 0; i < MAX_HANDLERS; ++i) {
            if (!s_handlers[i].used) { idx = i; break; }
        }
    }
    if (idx < 0) return HOST_ERR_NO_MEMORY;

    Handler& h = s_handlers[idx];
    h.used = true;
    h.plugin = plugin;
    std::strncpy(h.mime, mime_type, sizeof(h.mime) - 1);
    h.mime[sizeof(h.mime) - 1] = '\0';
    h.action_id = action_id;

    const char* descKey =
        (std::strncmp(mime_type, "text/", 5) == 0) ? "core.msg_text" : "core.msg_data";
    bool ok = msg::MessageTransfer::instance().registerHandler(
        h.mime, descKey,
        [plugin, action_id](const uint8_t* d, uint32_t l, const char* m, const char*) -> bool {
            return on_deliver(plugin, action_id, d, l, m);
        });
    if (!ok) {
        h = Handler{};
        return HOST_ERR_NO_MEMORY;
    }
    return HOST_OK;
}

int host_msg_unregister_handler(const char* mime_type) {
    if (!mime_type) return HOST_ERR_INVALID_ARG;
    void* plugin = plg_get_active_plugin();
    int idx = find_handler(plugin, mime_type);
    if (idx < 0) return HOST_ERR_NOT_FOUND;
    msg::MessageTransfer::instance().unregisterHandler(s_handlers[idx].mime);
    s_handlers[idx] = Handler{};
    return HOST_OK;
}

int host_msg_consume(uint8_t* buf, size_t buf_size, char* mime_out, size_t mime_size) {
    if (!buf || buf_size == 0) return HOST_ERR_INVALID_ARG;
    uint32_t n = s_cur_len;
    if (n > buf_size) n = static_cast<uint32_t>(buf_size);
    std::memcpy(buf, s_cur_buf, n);
    if (mime_out && mime_size) {
        std::strncpy(mime_out, s_cur_mime, mime_size - 1);
        mime_out[mime_size - 1] = '\0';
    }
    return static_cast<int>(n);
}

int host_msg_send_interactive(const char* mime_type, const uint8_t* data, size_t len) {
    if (!msg_allowed()) return HOST_ERR_NO_CAPABILITY;
    if (!mime_type || !data || len == 0 || len > HOST_MSG_PAYLOAD_MAX) return HOST_ERR_INVALID_ARG;
    bool ok = msg::MessageTransfer::instance().beginInteractiveSend(
        mime_type, data, static_cast<uint32_t>(len));
    return ok ? HOST_OK : HOST_ERR_BUSY;
}

int host_msg_send(const uint8_t addr[6], uint8_t addr_type, const char* mime_type,
                  const uint8_t* data, size_t len) {
    if (!msg_allowed()) return HOST_ERR_NO_CAPABILITY;
    if (!addr || !mime_type || !data || len == 0 || len > HOST_MSG_PAYLOAD_MAX) {
        return HOST_ERR_INVALID_ARG;
    }
    bool ok = msg::MessageTransfer::instance().sendTo(
        addr, addr_type, mime_type, data, static_cast<uint32_t>(len));
    return ok ? HOST_OK : HOST_ERR_BUSY;
}

// Register the deferred-handler resolver with cdc_msg (called once at PluginManager init).
void plg_msg_init(void) {
    msg::MessageTransfer::instance().setDeferredHandler(
        [](const char* mime) -> bool {
            return pm::PluginManager::instance().messageTypeInstalled(mime);
        },
        stash_deferred);
}

// Drain one completed inbound delivery and fire the owning plugin's action.
void plg_msg_pump(void) {
    handle_deferred();  // first: load + deliver to a not-yet-loaded handler plugin

    void* plugin = nullptr;
    uint32_t aid = 0, len = 0;
    {
        Guard g;
        if (!s_pending.valid) return;
        plugin = s_pending.plugin;
        aid = s_pending.action_id;
        len = s_pending.len;
        std::memcpy(s_cur_buf, s_pending.buf, len);
        std::memcpy(s_cur_mime, s_pending.mime, sizeof(s_cur_mime));
        s_cur_len = len;
        s_pending.valid = false;
    }
    if (plugin && aid) {
        pm::PluginManager::instance().dispatchActionTo(
            static_cast<pm::Plugin*>(plugin), aid, 0, len);
    }
    s_cur_len = 0;       // payload only valid during the action dispatch
    s_cur_mime[0] = '\0';
}

// Drop any handlers owned by a plugin being unloaded.
void plg_msg_on_unload(void* plugin) {
    for (int i = 0; i < MAX_HANDLERS; ++i) {
        if (s_handlers[i].used && s_handlers[i].plugin == plugin) {
            msg::MessageTransfer::instance().unregisterHandler(s_handlers[i].mime);
            s_handlers[i] = Handler{};
        }
    }
    Guard g;
    if (s_pending.valid && s_pending.plugin == plugin) s_pending.valid = false;
}

}  // extern "C"
