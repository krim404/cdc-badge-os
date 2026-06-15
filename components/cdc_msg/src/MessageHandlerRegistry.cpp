/**
 * \file
 * \brief MIME-to-handler registry for the message-transfer framework.
 */

#include "cdc_msg/MessageHandlerRegistry.h"

#include <cstring>

namespace cdc::msg {

namespace {

/// Validate a MIME string: non-null, 1..kMaxMimeLen bytes, NUL-terminated.
bool validMime(const char* mime)
{
    if (!mime) return false;
    size_t n = strnlen(mime, kMimeBufSize);
    return n > 0 && n <= kMaxMimeLen;
}

}  // namespace

int MessageHandlerRegistry::find(const char* mime) const
{
    if (!mime) return -1;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].used && std::strncmp(entries_[i].mime, mime, kMimeBufSize) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool MessageHandlerRegistry::registerHandler(const char* mime, const char* descKey,
                                             DeliverFn deliver)
{
    if (!validMime(mime) || !deliver) return false;

    int idx = find(mime);
    if (idx < 0) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (!entries_[i].used) { idx = static_cast<int>(i); break; }
        }
    }
    if (idx < 0) return false;  // table full

    Entry& e = entries_[static_cast<size_t>(idx)];
    e.used = true;
    std::strncpy(e.mime, mime, sizeof(e.mime) - 1);
    e.mime[sizeof(e.mime) - 1] = '\0';
    e.descKey = descKey;
    e.fn = std::move(deliver);
    return true;
}

void MessageHandlerRegistry::unregisterHandler(const char* mime)
{
    int idx = find(mime);
    if (idx < 0) return;
    entries_[static_cast<size_t>(idx)] = Entry{};
}

bool MessageHandlerRegistry::hasHandler(const char* mime) const
{
    return find(mime) >= 0;
}

const char* MessageHandlerRegistry::descKey(const char* mime) const
{
    int idx = find(mime);
    return idx < 0 ? nullptr : entries_[static_cast<size_t>(idx)].descKey;
}

bool MessageHandlerRegistry::deliver(const char* mime, const uint8_t* data, uint32_t len,
                                     const char* peerName) const
{
    int idx = find(mime);
    if (idx < 0) return false;
    const Entry& e = entries_[static_cast<size_t>(idx)];
    if (!e.fn) return false;
    return e.fn(data, len, mime, peerName);
}

}  // namespace cdc::msg
