/**
 * \file host_api_http.cpp
 * \brief HTTP client wrapper over esp_http_client.
 *
 * Holds up to 4 simultaneous in-flight requests in a fixed slot table so the
 * WAMR-WASM side can identify them by integer handle. The whole response body
 * is buffered in PSRAM during host_http_perform (via the event handler, or a
 * manual drain on the auth-error path); host_http_read_chunk then hands it out
 * sequentially from that buffer, so reads are chunked but not truly streamed.
 * The body buffer is capped at MAX_HTTP_BODY_BYTES and silently truncated past
 * it; the read cursor is one-way (a body cannot be re-read); Content-Type is
 * not exposed. Connections are kept alive in a small per-origin pool and reused
 * across requests; a slot borrows a pooled connection (returned to the pool on
 * close) or, when the pool is full, owns a transient client freed on close.
 */

#include "plugin_manager/Raii.h"
#include "plugin_manager/SlotTable.h"
#include "plugin_manager/host_api.h"
#include "cdc_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>

extern "C" void* plg_get_active_plugin(void);

namespace {

struct EspHttpClientDeleter {
    void operator()(esp_http_client_handle_t h) const noexcept
    {
        if (h) esp_http_client_cleanup(h);
    }
};

using EspHttpClient = std::unique_ptr<
    std::remove_pointer_t<esp_http_client_handle_t>, EspHttpClientDeleter>;
using HttpBody      = cdc::plugin_manager::CStdUniquePtr<char>;

struct HttpSlot {
    bool          used        = false;
    void*         owner       = nullptr;  // plugin that opened the request
    // The client used for this request: either borrowed from the connection pool
    // (`pool_idx >= 0`) or, when the pool is full, a transient client owned by
    // `owned` (`pool_idx < 0`).
    esp_http_client_handle_t client = nullptr;
    int           pool_idx    = -1;
    EspHttpClient owned;
    HttpBody      body;
    size_t        body_len    = 0;
    size_t        body_cap    = 0;
    size_t        body_cursor = 0;
    int           status      = 0;
    size_t        content_length = 0;
};

constexpr size_t MAX_HTTP_SLOTS = 4;
constexpr size_t MAX_HTTP_BODY_BYTES = 1 * 1024 * 1024;
cdc::plugin_manager::SlotTable<HttpSlot, MAX_HTTP_SLOTS> s_slots{};

// Keep-alive connection pool: clients are kept open across requests and reused
// per origin, so only the first request to a host pays the TCP+TLS handshake.
// Plugin host calls are serialised by the PluginManager call mutex, so the
// `s_active_slot` redirect below is safe (no concurrent perform), but several
// slots may be open at once (different origins), hence a pool rather than one
// shared client.
struct PoolConn {
    EspHttpClient client;
    std::string   origin;          // "scheme://host[:port]" this client is bound to
    bool          in_use = false;  // currently checked out by an open request
};

constexpr size_t MAX_HTTP_CONNS = MAX_HTTP_SLOTS;
PoolConn  s_pool[MAX_HTTP_CONNS];
HttpSlot* s_active_slot = nullptr;  // slot the body event handler writes into

// Origin key ("scheme://host[:port]") used to match a pooled connection to a URL.
std::string originKey(const char* url)
{
    std::string u(url ? url : "");
    size_t scheme = u.find("://");
    if (scheme == std::string::npos) return u;
    size_t path = u.find('/', scheme + 3);
    return path == std::string::npos ? u : u.substr(0, path);
}

// Append response bytes to the slot buffer, growing PSRAM storage up to
// MAX_HTTP_BODY_BYTES. Returns false if the cap is reached or realloc fails.
bool appendBody(HttpSlot& slot, const char* data, size_t len)
{
    if (!data || len == 0) return true;
    size_t needed = slot.body_len + len + 1;
    if (needed > MAX_HTTP_BODY_BYTES) return false;
    if (needed > slot.body_cap) {
        size_t new_cap = slot.body_cap ? slot.body_cap : 4096;
        while (new_cap < needed) new_cap *= 2;
        if (new_cap > MAX_HTTP_BODY_BYTES) new_cap = MAX_HTTP_BODY_BYTES;
        char* raw = slot.body.release();
        char* grown = static_cast<char*>(std::realloc(raw, new_cap));
        if (!grown) {
            slot.body.reset(raw);
            return false;
        }
        slot.body.reset(grown);
        slot.body_cap = new_cap;
    }
    std::memcpy(slot.body.get() + slot.body_len, data, len);
    slot.body_len += len;
    slot.body.get()[slot.body_len] = '\0';
    return true;
}

esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    // The client is reused across requests, so the body target is the slot
    // currently performing, not a fixed init-time user_data pointer.
    HttpSlot* slot = s_active_slot;
    if (!slot) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        if (!appendBody(*slot, static_cast<const char*>(evt->data),
                        static_cast<size_t>(evt->data_len))) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

HttpSlot* slotFor(int handle) { return s_slots.lookup(handle); }

// Release a slot: return a borrowed pool connection to the pool (a transient
// client is freed by the slot reset) and clear the slot. Shared by
// host_http_close and the plugin-unload sweep.
void closeSlot(HttpSlot& slot)
{
    if (slot.pool_idx >= 0 && static_cast<size_t>(slot.pool_idx) < MAX_HTTP_CONNS) {
        s_pool[slot.pool_idx].in_use = false;
    }
    slot = HttpSlot{};
}

esp_http_client_method_t mapMethod(uint8_t m) {
    switch (m) {
        case HTTP_GET:    return HTTP_METHOD_GET;
        case HTTP_POST:   return HTTP_METHOD_POST;
        case HTTP_PUT:    return HTTP_METHOD_PUT;
        case HTTP_DELETE: return HTTP_METHOD_DELETE;
        default:          return HTTP_METHOD_GET;
    }
}

}  // namespace

extern "C" {

esp_http_client_handle_t makeClient(uint8_t method, const char* url, uint32_t timeout_ms)
{
    esp_http_client_config_t cfg{};
    cfg.url            = url;
    cfg.method         = mapMethod(method);
    cfg.timeout_ms     = timeout_ms ? static_cast<int>(timeout_ms) : 5000;
    cfg.event_handler  = http_event_handler;
    cfg.disable_auto_redirect = false;
    cfg.crt_bundle_attach     = esp_crt_bundle_attach;
    cfg.keep_alive_enable     = true;
    cfg.buffer_size           = 4096;
    cfg.buffer_size_tx        = 1024;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) LOG_E("HTTP", "esp_http_client_init failed for url='%s'", url);
    return h;
}

int host_http_open(uint8_t method, const char* url, uint32_t timeout_ms)
{
    if (!url) return HOST_ERR_INVALID_ARG;
    int slot_id = 0;
    HttpSlot* slot = s_slots.allocate(slot_id);
    if (!slot) return HOST_ERR_NO_MEMORY;

    *slot       = HttpSlot{};
    slot->used  = true;
    slot->owner = plg_get_active_plugin();

    std::string key = originKey(url);

    // 1) Reuse an idle pooled connection for the same origin (keep-alive hit).
    for (size_t i = 0; i < MAX_HTTP_CONNS; ++i) {
        if (s_pool[i].client && !s_pool[i].in_use && s_pool[i].origin == key) {
            esp_http_client_set_url(s_pool[i].client.get(), url);
            esp_http_client_set_method(s_pool[i].client.get(), mapMethod(method));
            s_pool[i].in_use = true;
            slot->pool_idx   = static_cast<int>(i);
            slot->client     = s_pool[i].client.get();
            return slot_id;
        }
    }

    // 2) Build a fresh keep-alive client for this origin.
    esp_http_client_handle_t h = makeClient(method, url, timeout_ms);
    if (!h) {
        *slot = HttpSlot{};
        return HOST_ERR_GENERIC;
    }

    // 3) Park it in the pool: prefer an empty entry, else evict an idle one.
    int idx = -1;
    for (size_t i = 0; i < MAX_HTTP_CONNS; ++i) {
        if (!s_pool[i].client) { idx = static_cast<int>(i); break; }
    }
    if (idx < 0) {
        for (size_t i = 0; i < MAX_HTTP_CONNS; ++i) {
            if (!s_pool[i].in_use) { idx = static_cast<int>(i); break; }
        }
    }
    if (idx >= 0) {
        s_pool[idx].client.reset(h);
        s_pool[idx].origin = key;
        s_pool[idx].in_use = true;
        slot->pool_idx     = idx;
        slot->client       = h;
    } else {
        // All pooled connections are in flight: use a transient, slot-owned client.
        slot->owned.reset(h);
        slot->pool_idx = -1;
        slot->client   = h;
    }
    return slot_id;
}

int host_http_set_header(int handle, const char* key, const char* value)
{
    auto* slot = slotFor(handle);
    if (!slot || !slot->client || !key || !value) return HOST_ERR_INVALID_ARG;
    return esp_http_client_set_header(slot->client, key, value) == ESP_OK
           ? HOST_OK : HOST_ERR_GENERIC;
}

int host_http_set_body(int handle, const uint8_t* body, size_t len)
{
    auto* slot = slotFor(handle);
    if (!slot || !slot->client) return HOST_ERR_INVALID_ARG;
    return esp_http_client_set_post_field(slot->client,
                                          reinterpret_cast<const char*>(body),
                                          static_cast<int>(len)) == ESP_OK
           ? HOST_OK : HOST_ERR_GENERIC;
}

int host_http_perform(int handle)
{
    auto* slot = slotFor(handle);
    if (!slot || !slot->client) return HOST_ERR_INVALID_ARG;
    s_active_slot = slot;
    esp_err_t err = esp_http_client_perform(slot->client);
    s_active_slot = nullptr;
    slot->status         = esp_http_client_get_status_code(slot->client);
    slot->content_length = static_cast<size_t>(
        esp_http_client_get_content_length(slot->client));
    // esp_http_client_perform reports ESP_ERR_NOT_SUPPORTED for a 401 whose
    // WWW-Authenticate scheme it cannot satisfy (e.g. Bearer) and returns on an
    // early path where client->response is never allocated. Calling
    // esp_http_client_read there dereferences a NULL response and faults, so the
    // body must NOT be drained on the error path. A zero status additionally
    // means no HTTP exchange happened (connect/TLS failure). In every error case
    // surface the captured status with an empty body and drop the keep-alive
    // connection so the next request starts from a clean client.
    if (err != ESP_OK) {
        if (slot->status == 0) {
            LOG_E("HTTP", "perform failed: %s (0x%x)", esp_err_to_name(err), err);
        } else {
            LOG_W("HTTP", "perform err %s with status=%d; body not drained",
                  esp_err_to_name(err), slot->status);
        }
        if (slot->pool_idx >= 0 && static_cast<size_t>(slot->pool_idx) < MAX_HTTP_CONNS) {
            s_pool[slot->pool_idx] = PoolConn{};
        } else {
            slot->owned.reset();
        }
        slot->client   = nullptr;
        slot->pool_idx = -1;
        if (slot->status == 0) {
            return HOST_ERR_GENERIC;
        }
    }
    LOG_I("HTTP", "status=%d content_length=%zu", slot->status, slot->content_length);
    return HOST_OK;
}

int host_http_status(int handle)
{
    auto* slot = slotFor(handle);
    return slot ? slot->status : HOST_ERR_INVALID_ARG;
}

int host_http_read_chunk(int handle, uint8_t* buf, size_t buf_size, size_t* out_len)
{
    auto* slot = slotFor(handle);
    if (!slot || !buf || !out_len) return HOST_ERR_INVALID_ARG;
    size_t remaining = slot->body_len - slot->body_cursor;
    size_t n = remaining < buf_size ? remaining : buf_size;
    if (n) std::memcpy(buf, slot->body.get() + slot->body_cursor, n);
    slot->body_cursor += n;
    *out_len = n;
    return HOST_OK;
}

size_t host_http_content_length(int handle)
{
    auto* slot = slotFor(handle);
    return slot ? slot->content_length : 0;
}

int host_http_close(int handle)
{
    auto* slot = slotFor(handle);
    if (!slot) return HOST_ERR_INVALID_ARG;
    closeSlot(*slot);  // RAII frees the transient client (if any) + body buffer
    return HOST_OK;
}

void plg_http_on_unload(void* plugin)
{
    if (!plugin) return;
    for (size_t i = 0; i < MAX_HTTP_SLOTS; ++i) {
        HttpSlot& slot = s_slots.slots[i];
        if (!slot.used || slot.owner != plugin) continue;
        LOG_W("HTTP", "force-closing leaked HTTP slot %u", static_cast<unsigned>(i + 1));
        if (s_active_slot == &slot) s_active_slot = nullptr;
        closeSlot(slot);
    }
    // Drop idle pooled keep-alive connections: one reused across a plugin
    // exit/re-enter can hand back a stale or truncated body. Plugins are
    // UI-exclusive, so any connection not in flight is safe to discard here; the
    // next request rebuilds a fresh connection.
    for (size_t i = 0; i < MAX_HTTP_CONNS; ++i) {
        if (!s_pool[i].in_use) s_pool[i] = PoolConn{};
    }
}

}  // extern "C"
