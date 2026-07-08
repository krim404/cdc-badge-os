/**
 * \file host_api_socket.cpp
 * \brief Outbound TCP / UDP client sockets for plugin network protocols.
 *
 * Plugins open a handle with host_socket_open (HOST_SOCK_TCP / HOST_SOCK_UDP),
 * exchange bytes via host_socket_write / host_socket_read, and release it with
 * host_socket_close. Up to MAX_SOCKET_SLOTS handles are tracked in a fixed slot
 * table keyed by 1-based integer handle; handles leaked by a crashing or
 * exiting plugin are swept in plg_socket_on_unload. Both protocols are
 * connected: a UDP socket fixes its peer at open time via connect(), so the
 * read/write path is identical for both. Gated on the "socket" capability.
 * lwIP owns DNS resolution and the protocol stacks; per-call timeouts map to
 * SO_RCVTIMEO / SO_SNDTIMEO, the TCP connect timeout to a non-blocking
 * connect bounded by select().
 */

#include "plugin_manager/SlotTable.h"
#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "cdc_log.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

extern "C" void* plg_get_active_plugin(void);

namespace {

static constexpr const char* TAG = "SOCK";

constexpr size_t   MAX_SOCKET_SLOTS   = 8;
constexpr uint32_t kDefaultTimeoutMs  = 5000;

struct SocketSlot {
    bool    used  = false;
    void*   owner = nullptr;  // plugin that opened the socket
    int     fd    = -1;
    uint8_t proto = HOST_SOCK_TCP;
};

cdc::plugin_manager::SlotTable<SocketSlot, MAX_SOCKET_SLOTS> s_slots{};

bool socketAllowed()
{
    auto* p = static_cast<cdc::plugin_manager::Plugin*>(plg_get_active_plugin());
    return p && p->manifest().capabilities.socket;
}

// Read/write/close are also allowed for a net_listen plugin operating on a
// connection it accepted via the listener (host_net_accept adopts the fd into
// this slot pool). Opening outbound connections still requires `socket`.
bool socketIoAllowed()
{
    auto* p = static_cast<cdc::plugin_manager::Plugin*>(plg_get_active_plugin());
    return p && (p->manifest().capabilities.socket || p->manifest().capabilities.net_listen);
}

SocketSlot* slotFor(int handle) { return s_slots.lookup(handle); }

void closeSlot(SocketSlot& slot)
{
    if (slot.fd >= 0) ::close(slot.fd);
    slot = SocketSlot{};
}

// Apply a per-call send or receive timeout to a socket fd.
void applyTimeout(int fd, int optname, uint32_t to_ms)
{
    struct timeval tv;
    tv.tv_sec  = static_cast<time_t>(to_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((to_ms % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, optname, &tv, sizeof(tv));
}

// TCP: bound the connect with a non-blocking handshake + select(); UDP:
// connect() only fixes the default peer and returns immediately.
int connectEndpoint(int fd, const struct sockaddr* addr, socklen_t addrlen,
                    uint8_t proto, uint32_t to_ms)
{
    if (proto == HOST_SOCK_UDP) {
        return ::connect(fd, addr, addrlen) == 0 ? HOST_OK : HOST_ERR_GENERIC;
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return HOST_ERR_GENERIC;
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd, addr, addrlen);
    if (rc != 0 && errno != EINPROGRESS) return HOST_ERR_GENERIC;

    if (rc != 0) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        struct timeval tv;
        tv.tv_sec  = static_cast<time_t>(to_ms / 1000);
        tv.tv_usec = static_cast<suseconds_t>((to_ms % 1000) * 1000);
        int sel = ::select(fd + 1, nullptr, &wset, nullptr, &tv);
        if (sel == 0) return HOST_ERR_TIMEOUT;
        if (sel < 0)  return HOST_ERR_GENERIC;

        int soerr = 0;
        socklen_t len = sizeof(soerr);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0 || soerr != 0) {
            return HOST_ERR_GENERIC;
        }
    }

    ::fcntl(fd, F_SETFL, flags);
    return HOST_OK;
}

}  // namespace

extern "C" {

int host_socket_open(uint8_t proto, const char* host, uint16_t port, uint32_t timeout_ms)
{
    if (!socketAllowed())                                    return HOST_ERR_NO_CAPABILITY;
    if (!host || !*host)                                     return HOST_ERR_INVALID_ARG;
    if (proto != HOST_SOCK_TCP && proto != HOST_SOCK_UDP)    return HOST_ERR_INVALID_ARG;

    const int      socktype = (proto == HOST_SOCK_UDP) ? SOCK_DGRAM : SOCK_STREAM;
    const uint32_t to_ms    = timeout_ms ? timeout_ms : kDefaultTimeoutMs;

    int slot_id = 0;
    SocketSlot* slot = s_slots.allocate(slot_id);
    if (!slot) return HOST_ERR_NO_MEMORY;
    *slot       = SocketSlot{};
    slot->used  = true;
    slot->owner = plg_get_active_plugin();

    char port_str[6];
    std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));

    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = socktype;
    struct addrinfo* res = nullptr;
    if (::getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        LOG_W(TAG, "resolve failed for '%s'", host);
        *slot = SocketSlot{};
        return HOST_ERR_NOT_FOUND;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        *slot = SocketSlot{};
        return HOST_ERR_GENERIC;
    }

    int rc = connectEndpoint(fd, res->ai_addr, res->ai_addrlen, proto, to_ms);
    ::freeaddrinfo(res);
    if (rc != HOST_OK) {
        ::close(fd);
        *slot = SocketSlot{};
        return rc;
    }

    slot->fd    = fd;
    slot->proto = proto;
    return slot_id;
}

int host_socket_write(int handle, const uint8_t* data, size_t len, uint32_t timeout_ms)
{
    if (!socketIoAllowed()) return HOST_ERR_NO_CAPABILITY;
    auto* slot = slotFor(handle);
    if (!slot || slot->fd < 0) return HOST_ERR_INVALID_ARG;
    if (len == 0) return 0;
    if (!data)    return HOST_ERR_INVALID_ARG;

    applyTimeout(slot->fd, SO_SNDTIMEO, timeout_ms ? timeout_ms : kDefaultTimeoutMs);
    int n = static_cast<int>(::send(slot->fd, data, len, 0));
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? HOST_ERR_TIMEOUT
                                                         : HOST_ERR_GENERIC;
    }
    return n;
}

int host_socket_read(int handle, uint8_t* out, size_t cap, uint32_t timeout_ms)
{
    if (!socketIoAllowed()) return HOST_ERR_NO_CAPABILITY;
    auto* slot = slotFor(handle);
    if (!slot || slot->fd < 0) return HOST_ERR_INVALID_ARG;
    if (cap == 0) return 0;
    if (!out)     return HOST_ERR_INVALID_ARG;

    applyTimeout(slot->fd, SO_RCVTIMEO, timeout_ms ? timeout_ms : kDefaultTimeoutMs);
    int n = static_cast<int>(::recv(slot->fd, out, cap, 0));
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? HOST_ERR_TIMEOUT
                                                         : HOST_ERR_GENERIC;
    }
    return n;  // 0 == peer closed the connection (TCP EOF)
}

int host_socket_close(int handle)
{
    auto* slot = slotFor(handle);
    if (!slot) return HOST_ERR_INVALID_ARG;
    closeSlot(*slot);
    return HOST_OK;
}

// Adopt an already-connected TCP fd (e.g. accepted by the net listener) into a
// socket slot so the plugin can drive it via host_socket_read/write/close.
// Returns a 1-based handle, or a negative HOST_ERR_* code. Not WASM-exported.
int plg_socket_adopt(int fd, void* owner)
{
    if (fd < 0) return HOST_ERR_INVALID_ARG;
    int slot_id = 0;
    SocketSlot* slot = s_slots.allocate(slot_id);
    if (!slot) return HOST_ERR_NO_MEMORY;
    *slot       = SocketSlot{};
    slot->used  = true;
    slot->owner = owner;
    slot->fd    = fd;
    slot->proto = HOST_SOCK_TCP;
    return slot_id;
}

void plg_socket_on_unload(void* plugin)
{
    if (!plugin) return;
    for (size_t i = 0; i < MAX_SOCKET_SLOTS; ++i) {
        SocketSlot& slot = s_slots.slots[i];
        if (!slot.used || slot.owner != plugin) continue;
        LOG_W(TAG, "force-closing leaked socket slot %u", static_cast<unsigned>(i + 1));
        closeSlot(slot);
    }
}

}  // extern "C"
