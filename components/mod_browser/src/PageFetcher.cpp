#include "PageFetcher.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#include "cdc_log.h"

#include <cstring>

namespace cdc::browser {
namespace {

static constexpr const char* TAG = "BROWSER-HTTP";
static constexpr int kMaxRedirects = 5;
static constexpr int kTimeoutMs = 8000;

// Active jar for the in-flight request, so the header event handler can capture
// Set-Cookie. Only one fetch runs at a time (serialized worker task).
CookieJar* s_activeJar = nullptr;

bool feedable(const char* ct)
{
    if (!ct || !ct[0]) return true;  // unknown -> attempt
    return std::strstr(ct, "html") || std::strstr(ct, "xml") || std::strncmp(ct, "text/", 5) == 0;
}

bool isRedirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

esp_err_t headerHandler(esp_http_client_event_t* evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && s_activeJar && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "Set-Cookie") == 0) s_activeJar->put(evt->header_value);
    }
    return ESP_OK;
}

// Issue the request on an open handle as GET or POST, attaching cookies.
bool doOpen(esp_http_client_handle_t h, bool isPost, const char* body, size_t bodyLen, CookieJar* jar)
{
    esp_http_client_set_method(h, isPost ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (jar && jar->count) {
        char cookie[CookieJar::kMax * CookieJar::kItemCap + 16];
        jar->header(cookie, sizeof(cookie));
        if (cookie[0]) esp_http_client_set_header(h, "Cookie", cookie);
    }
    if (isPost) {
        esp_http_client_set_header(h, "Content-Type", "application/x-www-form-urlencoded");
        if (esp_http_client_open(h, static_cast<int>(bodyLen)) != ESP_OK) return false;
        if (body && bodyLen) {
            int w = esp_http_client_write(h, body, bodyLen);
            if (w < 0) return false;
        }
    } else if (esp_http_client_open(h, 0) != ESP_OK) {
        return false;
    }
    return true;
}

// Open url with the given method, following redirects (as GET). Returns the handle
// with headers fetched, or nullptr on transport failure. Fills status/contentType.
esp_http_client_handle_t openFollowing(const char* url, bool isPost, const char* body,
                                       size_t bodyLen, CookieJar* jar, int& status, char* ctOut,
                                       size_t ctCap, char* finalUrlOut, size_t finalCap)
{
    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.method = isPost ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.timeout_ms = kTimeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;  // handled manually for the streaming path
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.event_handler = headerHandler;

    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return nullptr;

    s_activeJar = jar;
    if (!doOpen(h, isPost, body, bodyLen, jar)) {
        s_activeJar = nullptr;
        esp_http_client_cleanup(h);
        return nullptr;
    }
    for (int redirect = 0;; ++redirect) {
        esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        if (isRedirect(status) && redirect < kMaxRedirects) {
            esp_http_client_set_redirection(h);
            esp_http_client_close(h);
            if (!doOpen(h, false, nullptr, 0, jar)) {  // follow redirects as GET
                s_activeJar = nullptr;
                esp_http_client_cleanup(h);
                return nullptr;
            }
            continue;
        }
        break;
    }
    s_activeJar = nullptr;
    if (ctOut && ctCap) {
        ctOut[0] = '\0';
        char* ct = nullptr;
        if (esp_http_client_get_header(h, "Content-Type", &ct) == ESP_OK && ct) {
            std::strncpy(ctOut, ct, ctCap - 1);
            ctOut[ctCap - 1] = '\0';
        }
    }
    if (finalUrlOut && finalCap) {
        finalUrlOut[0] = '\0';
        esp_http_client_get_url(h, finalUrlOut, static_cast<int>(finalCap));
    }
    return h;
}

FetchResult streamBody(esp_http_client_handle_t h, HtmlTokenizer* tok, size_t maxBytes,
                       char* rawOut, size_t rawCap, size_t* rawLen, FetchResult r)
{
    r.ok = true;
    if (rawLen) *rawLen = 0;
    if (feedable(r.contentType) && (tok || rawOut)) {
        r.bodyFed = true;
        char buf[1024];
        size_t rawPos = 0;
        while (r.bytes < maxBytes) {
            int n = esp_http_client_read(h, buf, sizeof(buf));
            if (n <= 0) break;
            if (tok) tok->feed(buf, static_cast<size_t>(n));
            if (rawOut && rawCap && rawPos < rawCap - 1) {
                size_t room = rawCap - 1 - rawPos;
                size_t c = (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
                std::memcpy(rawOut + rawPos, buf, c);
                rawPos += c;
            }
            r.bytes += static_cast<size_t>(n);
        }
        if (rawOut && rawCap) rawOut[rawPos] = '\0';
        if (rawLen) *rawLen = rawPos;
        if (r.bytes >= maxBytes) r.truncated = true;
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return r;
}

}  // namespace

void CookieJar::put(const char* v)
{
    if (!v || !v[0]) return;
    // Keep "name=value" up to the first ';' (drop attributes).
    size_t n = 0;
    while (v[n] && v[n] != ';') ++n;
    if (n == 0 || n >= kItemCap) {
        if (n >= kItemCap) n = kItemCap - 1;
        else return;
    }
    // Determine the cookie name (up to '=').
    size_t nameLen = 0;
    while (nameLen < n && v[nameLen] != '=') ++nameLen;
    // Replace an existing cookie with the same name.
    for (uint8_t i = 0; i < count; ++i) {
        if (std::strncmp(items[i], v, nameLen) == 0 && items[i][nameLen] == '=') {
            std::memcpy(items[i], v, n);
            items[i][n] = '\0';
            return;
        }
    }
    if (count >= kMax) return;
    std::memcpy(items[count], v, n);
    items[count][n] = '\0';
    ++count;
}

void CookieJar::header(char* out, size_t cap) const
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    size_t pos = 0;
    for (uint8_t i = 0; i < count; ++i) {
        size_t need = std::strlen(items[i]) + (i ? 2 : 0);
        if (pos + need >= cap) break;
        if (i) { out[pos++] = ';'; out[pos++] = ' '; }
        std::strcpy(out + pos, items[i]);
        pos += std::strlen(items[i]);
    }
    out[pos] = '\0';
}

FetchResult fetchExtract(const char* url, bool isPost, const char* body, size_t bodyLen,
                         size_t maxBytes, CookieJar* jar, TokenizerProvider provide, void* ctx,
                         char* rawOut, size_t rawCap, size_t* rawLen)
{
    FetchResult r;
    char finalUrl[512] = {0};
    esp_http_client_handle_t h = openFollowing(url, isPost, body, bodyLen, jar, r.status,
                                               r.contentType, sizeof(r.contentType), finalUrl,
                                               sizeof(finalUrl));
    if (!h) {
        LOG_W(TAG, "%s open failed for %s", isPost ? "POST" : "GET", url ? url : "(null)");
        return r;
    }
    r.ok = true;
    HtmlTokenizer* tok = nullptr;
    if (feedable(r.contentType) && provide) {
        tok = provide(ctx, finalUrl[0] ? finalUrl : url);
    }
    return streamBody(h, tok, maxBytes, rawOut, rawCap, rawLen, r);
}

int probe(const char* url, CookieJar* jar)
{
    int status = -1;
    char ct[8];
    esp_http_client_handle_t h =
        openFollowing(url, false, nullptr, 0, jar, status, ct, sizeof(ct), nullptr, 0);
    if (!h) return -1;
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return status;
}

}  // namespace cdc::browser
