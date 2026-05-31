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
 * not exposed. The client handle and body buffer use RAII wrappers, so
 * resetting a slot via `*slot = HttpSlot{}` frees both resources automatically.
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
#include <type_traits>

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
    EspHttpClient handle;
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
    auto* slot = static_cast<HttpSlot*>(evt->user_data);
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

int host_http_open(uint8_t method, const char* url, uint32_t timeout_ms)
{
    if (!url) return HOST_ERR_INVALID_ARG;
    int slot_id = 0;
    HttpSlot* slot = s_slots.allocate(slot_id);
    if (!slot) return HOST_ERR_NO_MEMORY;

    *slot      = HttpSlot{};
    slot->used = true;

    esp_http_client_config_t cfg{};
    cfg.url            = url;
    cfg.method         = mapMethod(method);
    cfg.timeout_ms     = timeout_ms ? static_cast<int>(timeout_ms) : 5000;
    cfg.event_handler  = http_event_handler;
    cfg.user_data      = slot;
    cfg.disable_auto_redirect = false;
    cfg.crt_bundle_attach     = esp_crt_bundle_attach;
    cfg.buffer_size           = 4096;
    cfg.buffer_size_tx        = 1024;

    slot->handle.reset(esp_http_client_init(&cfg));
    if (!slot->handle) {
        LOG_E("HTTP", "esp_http_client_init failed for url='%s'", url);
        *slot = HttpSlot{};
        return HOST_ERR_GENERIC;
    }
    return slot_id;
}

int host_http_set_header(int handle, const char* key, const char* value)
{
    auto* slot = slotFor(handle);
    if (!slot || !key || !value) return HOST_ERR_INVALID_ARG;
    return esp_http_client_set_header(slot->handle.get(), key, value) == ESP_OK
           ? HOST_OK : HOST_ERR_GENERIC;
}

int host_http_set_body(int handle, const uint8_t* body, size_t len)
{
    auto* slot = slotFor(handle);
    if (!slot) return HOST_ERR_INVALID_ARG;
    return esp_http_client_set_post_field(slot->handle.get(),
                                          reinterpret_cast<const char*>(body),
                                          static_cast<int>(len)) == ESP_OK
           ? HOST_OK : HOST_ERR_GENERIC;
}

int host_http_perform(int handle)
{
    auto* slot = slotFor(handle);
    if (!slot) return HOST_ERR_INVALID_ARG;
    esp_err_t err = esp_http_client_perform(slot->handle.get());
    slot->status         = esp_http_client_get_status_code(slot->handle.get());
    slot->content_length = static_cast<size_t>(
        esp_http_client_get_content_length(slot->handle.get()));
    // esp_http_client_perform reports ESP_ERR_NOT_SUPPORTED for a 401 whose
    // WWW-Authenticate scheme it cannot satisfy (e.g. Bearer) and returns before
    // draining the body, so HTTP_EVENT_ON_DATA never fired. A zero status means
    // no HTTP exchange happened (connect/TLS failure): that stays a hard error.
    // Otherwise the exchange completed; drain the body manually so the plugin can
    // read the error response and act on the status code itself.
    if (err != ESP_OK) {
        if (slot->status == 0) {
            LOG_E("HTTP", "perform failed: %s (0x%x)", esp_err_to_name(err), err);
            return HOST_ERR_GENERIC;
        }
        char buf[512];
        int n;
        while ((n = esp_http_client_read(slot->handle.get(), buf, sizeof(buf))) > 0) {
            if (!appendBody(*slot, buf, static_cast<size_t>(n))) break;
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
    *slot = HttpSlot{};  // RAII frees handle + body
    return HOST_OK;
}

}  // extern "C"
