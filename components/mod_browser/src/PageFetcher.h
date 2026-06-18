#pragma once

#include "HtmlTokenizer.h"

#include <cstddef>
#include <cstdint>

/**
 * \file PageFetcher.h
 * \brief Streaming HTTP(S) fetch (GET/POST) with a session cookie jar.
 */

namespace cdc::browser {

/// \brief Minimal per-session cookie store (name=value pairs, replace by name).
struct CookieJar {
    static constexpr uint8_t kMax = 8;
    static constexpr size_t  kItemCap = 160;  ///< "name=value" length cap.
    char    items[kMax][kItemCap] = {};
    uint8_t count = 0;

    void clear() { count = 0; }
    /// \brief Store a Set-Cookie value (keeps "name=value" up to the first ';').
    void put(const char* setCookieValue);
    /// \brief Build a "Cookie:" header value ("a=b; c=d") into \p out.
    void header(char* out, size_t cap) const;
};

struct FetchResult {
    bool   ok = false;           ///< Transport-level success (a response was received).
    int    status = 0;           ///< HTTP status code.
    char   contentType[64] = {0};
    bool   bodyFed = false;      ///< Body was streamed to the sink (feedable content type).
    size_t bytes = 0;            ///< Body bytes streamed.
    bool   truncated = false;    ///< Hit \p maxBytes before EOF.
};

/**
 * \brief Supplies the tokenizer once the final (post-redirect) URL is known, so the
 *        caller can build its extractor with the correct base URL for resolving
 *        relative links/form actions. Returns nullptr to skip body streaming.
 */
using TokenizerProvider = HtmlTokenizer* (*)(void* ctx, const char* finalUrl);

/**
 * \brief Fetch \p url (GET or POST a urlencoded \p body), follow redirects, then
 *        stream the response body into the tokenizer obtained from \p provide.
 *        Cookies from \p jar are sent and Set-Cookie responses captured into it.
 *
 * When \p rawOut is non-null the raw response bytes are also copied into it
 * (NUL-terminated, capped at \p rawCap-1) and the byte count written to \p rawLen.
 * Pass a null \p provide to capture the raw body only (no extraction).
 */
FetchResult fetchExtract(const char* url, bool isPost, const char* body, size_t bodyLen,
                         size_t maxBytes, CookieJar* jar, TokenizerProvider provide, void* ctx,
                         char* rawOut = nullptr, size_t rawCap = 0, size_t* rawLen = nullptr);

/// \brief GET \p url and return only the final HTTP status (no body). -1 on failure.
int probe(const char* url, CookieJar* jar = nullptr);

}  // namespace cdc::browser
