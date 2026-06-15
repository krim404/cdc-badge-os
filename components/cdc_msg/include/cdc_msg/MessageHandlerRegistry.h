#pragma once

#include "cdc_msg/MessageTypes.h"

#include <array>
#include <cstdint>
#include <functional>

namespace cdc::msg {

/**
 * \brief Delivers a fully received payload to its owning module/plugin.
 *
 * \param data Received bytes. UNTRUSTED, attacker-controlled — the handler MUST
 *             validate them and must not assume NUL termination.
 * \param len  Number of bytes (0 < len <= kMaxPayloadBytes).
 * \param mime Matched MIME type (NUL-terminated ASCII).
 * \param peerName Untrusted sender display name (NUL-terminated), for display only.
 * \return true if the payload was accepted/stored.
 */
using DeliverFn = std::function<bool(const uint8_t* data, uint32_t len,
                                     const char* mime, const char* peerName)>;

/**
 * \brief Maps MIME types to delivery handlers and consent descriptions.
 *
 * Fixed-size, no dynamic allocation. Not thread-safe by itself; the owner
 * (MessageTransfer) serializes every access under its state mutex.
 */
class MessageHandlerRegistry {
public:
    /**
     * \brief Register (or replace) the handler for \p mime.
     * \param mime NUL-terminated ASCII MIME type, 1..kMaxMimeLen bytes.
     * \param descKey Stable i18n key (or literal label) shown in the consent prompt.
     * \param deliver Delivery callback (see DeliverFn).
     * \return false if arguments are invalid or the table is full.
     */
    bool registerHandler(const char* mime, const char* descKey, DeliverFn deliver);

    /// \brief Remove the handler for \p mime, if present.
    void unregisterHandler(const char* mime);

    /// \brief \return true if a handler is registered for \p mime.
    bool hasHandler(const char* mime) const;

    /// \brief \return the consent-prompt i18n key for \p mime, or nullptr if none.
    const char* descKey(const char* mime) const;

    /**
     * \brief Invoke the handler for \p mime with the received payload.
     * \return false if no handler is registered or the handler rejected the data.
     */
    bool deliver(const char* mime, const uint8_t* data, uint32_t len,
                 const char* peerName) const;

private:
    struct Entry {
        bool        used    = false;
        char        mime[kMimeBufSize] = {};
        const char* descKey = nullptr;
        DeliverFn   fn;
    };
    static constexpr uint8_t kMaxHandlers = 8;

    int find(const char* mime) const;

    std::array<Entry, kMaxHandlers> entries_{};
};

}  // namespace cdc::msg
