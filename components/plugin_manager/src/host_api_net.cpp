/**
 * \file host_api_net.cpp
 * \brief Inbound TCP listener for plugins (the server side the client socket API
 *        lacks). A plugin picks the port; the firmware binds/listens and accepts
 *        connections from the plugin tick task, handing each accepted socket to
 *        the plugin, which drives it through the existing host_socket_read/write/
 *        close (the fd is adopted into the shared socket-slot pool).
 *
 * No dedicated task: the listen socket is non-blocking and plg_net_pump()
 * (plugin tick task) accepts pending connections, enqueues them and fires the
 * plugin's action; the handler then calls host_net_accept(). Everything runs on
 * the tick task, so no locking is needed. The listen socket is transparently
 * re-opened after an accept error (covering a WiFi/socket drop across light
 * sleep). One listener per plugin (v1). WiFi must be up (host_wifi_request) for
 * clients to reach the badge; bind succeeds regardless and accept just waits.
 */

#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginManager.h"
#include "cdc_log.h"

#include "lwip/sockets.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace pm = cdc::plugin_manager;

extern "C" void* plg_get_active_plugin(void);
extern "C" int   plg_socket_adopt(int fd, void* owner);

namespace {

const char* TAG = "NET";

constexpr uint8_t ACCEPT_RING = 4;

struct {
    void*    plugin = nullptr;
    bool     active = false;
    uint16_t port = 0;
    uint32_t action_id = 0;
    int      listen_fd = -1;
    int      ring[ACCEPT_RING];
    uint8_t  head = 0, tail = 0;  // head=produced, tail=consumed
} net_state;

bool net_allowed() {
    auto* p = static_cast<pm::Plugin*>(plg_get_active_plugin());
    return p && p->manifest().capabilities.net_listen;
}

// Open a non-blocking listen socket on the port (INADDR_ANY). Returns fd or -1.
int open_listen(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

void close_listener() {
    if (net_state.listen_fd >= 0) { ::close(net_state.listen_fd); net_state.listen_fd = -1; }
    while (net_state.tail != net_state.head) {
        ::close(net_state.ring[net_state.tail]);
        net_state.tail = static_cast<uint8_t>((net_state.tail + 1) % ACCEPT_RING);
    }
}

}  // namespace

extern "C" {

int host_net_listen(uint16_t port, uint32_t action_id) {
    if (!net_allowed()) return HOST_ERR_NO_CAPABILITY;
    if (port == 0) return HOST_ERR_INVALID_ARG;
    void* plugin = plg_get_active_plugin();
    if (net_state.active) {
        if (net_state.plugin == plugin && net_state.port == port) {
            net_state.action_id = action_id;  // idempotent re-listen
            return HOST_OK;
        }
        return HOST_ERR_BUSY;  // one listener at a time (v1)
    }
    net_state = {};
    net_state.plugin = plugin;
    net_state.port = port;
    net_state.action_id = action_id;
    net_state.listen_fd = open_listen(port);
    net_state.active = true;
    // A failed bind (WiFi not up yet / port busy) is retried by the pump.
    if (net_state.listen_fd < 0) {
        LOG_W(TAG, "bind port %u pending (retrying)", static_cast<unsigned>(port));
    } else {
        LOG_I(TAG, "listening on port %u", static_cast<unsigned>(port));
    }
    return HOST_OK;
}

int host_net_accept(void) {
    if (!net_allowed()) return HOST_ERR_NO_CAPABILITY;
    void* plugin = plg_get_active_plugin();
    if (!net_state.active || net_state.plugin != plugin) return HOST_ERR_NOT_FOUND;
    if (net_state.tail == net_state.head) return HOST_ERR_NOT_FOUND;
    int fd = net_state.ring[net_state.tail];
    net_state.tail = static_cast<uint8_t>((net_state.tail + 1) % ACCEPT_RING);
    int handle = plg_socket_adopt(fd, plugin);
    if (handle < 0) { ::close(fd); return handle; }
    return handle;
}

int host_net_close(uint16_t port) {
    if (!net_allowed()) return HOST_ERR_NO_CAPABILITY;
    if (!net_state.active || net_state.plugin != plg_get_active_plugin() ||
        (port != 0 && net_state.port != port)) {
        return HOST_ERR_NOT_FOUND;
    }
    close_listener();
    net_state = {};
    net_state.listen_fd = -1;
    return HOST_OK;
}

// Plugin tick task: (re)open the listen socket, drain pending connections into
// the ring, and fire the owner's action while connections wait.
void plg_net_pump(void) {
    if (!net_state.active) return;
    if (net_state.listen_fd < 0) {
        net_state.listen_fd = open_listen(net_state.port);
        if (net_state.listen_fd < 0) return;  // WiFi still down / port busy
        LOG_I(TAG, "listening on port %u", static_cast<unsigned>(net_state.port));
    }
    for (;;) {
        int c = ::accept(net_state.listen_fd, nullptr, nullptr);
        if (c < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // nothing pending
            // Real error (e.g. socket dropped across sleep): reopen next tick.
            ::close(net_state.listen_fd);
            net_state.listen_fd = -1;
            return;
        }
        uint8_t next = static_cast<uint8_t>((net_state.head + 1) % ACCEPT_RING);
        if (next == net_state.tail) { ::close(c); break; }  // ring full: drop
        net_state.ring[net_state.head] = c;
        net_state.head = next;
    }
    if (net_state.tail != net_state.head && net_state.action_id && net_state.plugin) {
        pm::PluginManager::instance().dispatchActionTo(
            static_cast<pm::Plugin*>(net_state.plugin), net_state.action_id, 0, 0);
    }
}

void plg_net_on_unload(void* plugin) {
    if (net_state.active && net_state.plugin == plugin) {
        close_listener();
        net_state = {};
        net_state.listen_fd = -1;
    }
}

}  // extern "C"
