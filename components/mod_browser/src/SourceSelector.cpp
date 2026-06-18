#include "SourceSelector.h"

#include <cstdio>
#include <cstring>

namespace cdc::browser {
namespace {

struct Mirror {
    const char* host;
    const char* mirror;
};

constexpr Mirror kTextMirrorMap[] = {
    {"cnn.com", "lite.cnn.com"},
    {"www.cnn.com", "lite.cnn.com"},
    {"npr.org", "text.npr.org"},
    {"www.npr.org", "text.npr.org"},
};

/// Copy the host (authority) of \p url into \p out. Returns the offset in \p url
/// where the path begins (so the caller can re-attach it).
size_t splitHost(const char* url, char* host, size_t cap)
{
    const char* schemeEnd = std::strstr(url, "://");
    const char* h = schemeEnd ? schemeEnd + 3 : url;
    const char* e = h;
    while (*e && *e != '/' && *e != '?' && *e != '#') ++e;
    size_t n = static_cast<size_t>(e - h);
    if (n >= cap) n = cap - 1;
    std::memcpy(host, h, n);
    host[n] = '\0';
    return static_cast<size_t>(e - url);
}

}  // namespace

bool textMirrorFor(const char* requestUrl, char* out, size_t cap)
{
    if (!requestUrl || !out || cap == 0) return false;
    char host[128];
    size_t pathOff = splitHost(requestUrl, host, sizeof(host));
    for (const Mirror& m : kTextMirrorMap) {
        if (std::strcmp(host, m.host) == 0) {
            std::snprintf(out, cap, "https://%s%s", m.mirror, requestUrl + pathOff);
            return true;
        }
    }
    return false;
}

SourceDecision selectSource(const char* requestUrl, const HeadSignals& head)
{
    SourceDecision d;
    if (textMirrorFor(requestUrl, d.fetchUrl, sizeof(d.fetchUrl))) {
        d.source = Source::TextMirror;
        return d;
    }
    if (head.feedUrl[0]) {
        d.source = Source::Feed;
        std::snprintf(d.fetchUrl, sizeof(d.fetchUrl), "%s", head.feedUrl);
        return d;
    }
    if (head.amphtml[0]) {
        d.source = Source::Amp;
        std::snprintf(d.fetchUrl, sizeof(d.fetchUrl), "%s", head.amphtml);
        return d;
    }
    d.source = Source::MainExtract;
    const char* u = head.canonical[0] ? head.canonical : requestUrl;
    std::snprintf(d.fetchUrl, sizeof(d.fetchUrl), "%s", u ? u : "");
    return d;
}

}  // namespace cdc::browser
