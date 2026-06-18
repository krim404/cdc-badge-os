# Contract: FeedParser, PageFetcher, SourceSelector

## FeedParser (host-testable, no GFX/IDF)

Streaming RSS/Atom item scanner. No XML library; a small tag-state scanner.

```cpp
namespace cdc::browser {

struct FeedItem {
    char title[128];
    char link[512];       // RSS: element text; Atom: <link href> attribute
    char* body;           // content:encoded / Atom <content> / <summary>; PSRAM, may be empty
    size_t bodyLen;
    bool   fullText;      // true if content:encoded / Atom <content> present (vs summary-only)
};

class FeedParser {
public:
    void feed(const char* bytes, size_t len);
    void finish();
    // Sink-style: onItem(const FeedItem&) for index; or first-item capture for "open article".
    bool getFirstItem(FeedItem& out) const;
    uint16_t itemCount() const;
};

} // namespace cdc::browser
```

Guarantees: handles `<![CDATA[...]]>`, the 5 predefined XML entities + numeric entities, RSS
`link` as element text vs Atom `link` as `href` attribute, and ordering where `description`
precedes `content:encoded`. Item bodies contain bounded chrome-free HTML and are re-run through
the HTML text pass (`decodeWebText` + a light tag strip). Memory constant; item buffers in PSRAM.

Host tests: RSS 2.0 full-text feed, Atom feed, summary-only feed (sets `fullText=false`), CDATA
bodies, entity-escaped bodies, malformed/truncated feed (no crash).

## SourceSelector (host-testable, no GFX/IDF)

Pure decision function over `HeadSignals` + URL host.

```cpp
enum class Source { TextMirror, Feed, Amp, MainExtract, OgCard };

struct SourceDecision { Source source; char fetchUrl[512]; };  // fetchUrl set for Mirror/Feed/Amp

SourceDecision selectSource(const char* requestUrl, const HeadSignals& head);
```

Order (research D3): host-map text mirror -> feed (`feedUrl`) -> amp (`amphtml`) -> MainExtract
(after applying `canonical`) -> (extractor decides OgCard fallback). `fetchUrl` is resolved
absolute. Host tests: each branch fires for the right inputs; canonical applied; no-variant page
-> MainExtract.

## PageFetcher (hardware-coupled; thin, not host-tested)

Wraps `esp_http_client` in a **streaming read loop** feeding a `TokenSink`/`FeedParser`. Never
buffers the whole response.

```cpp
struct FetchResult { int status; char contentType[64]; char finalUrl[512]; bool ok; };

class PageFetcher {
public:
    // Streams body into `sink->feed(...)` in ~4 KB chunks up to maxBytes; aborts when
    // cancel() is set (back key). Returns status/content-type/final URL.
    FetchResult fetch(const char* url, TokenSink* sink, size_t maxBytes, volatile bool* cancel);
};
```

Config (from `host_api_http.cpp`): `crt_bundle_attach = esp_crt_bundle_attach` (TLS validated),
auto-redirect on, `buffer_size = 4096`. Flow: `esp_http_client_init` -> `_open` ->
`_fetch_headers` -> read `Content-Type`/status/`Location` via `_get_header`/`_get_status_code`
-> loop `esp_http_client_read` (4 KB) -> `sink->feed` -> `_close`/`_cleanup`. Stops at
`maxBytes` (512 KB) or on `*cancel`.

Guarantees: bounded RAM (one 4 KB buffer + sink state), non-HTML `Content-Type` reported so the
caller shows a content-type notice (`text/plain` rendered as-is), cancellable mid-stream
(FR-014), all error categories surfaced (FR-013).
