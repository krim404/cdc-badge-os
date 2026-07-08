/**
 * \file host_api_vcard.cpp
 * \brief vCard host API: read and manage the own card plus the received-card
 *        store, addressed by sorted position like the firmware's contact list.
 *
 * All calls run inside a WASM frame on the plugin tick task; the vcard store
 * does its own NVS-backed lazy loading, so no extra synchronisation is needed
 * here. Strings cross the WASM boundary as stored (vCard 4.0 UTF-8 text).
 */

#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "mod_vcard/vcard_store.h"
#include "cdc_log.h"

#include <cstring>

namespace pm = cdc::plugin_manager;

extern "C" void* plg_get_active_plugin(void);

namespace {

bool vcard_allowed() {
    auto* p = static_cast<pm::Plugin*>(plg_get_active_plugin());
    return p && p->manifest().capabilities.vcard;
}

// Resolve a 0-based sorted position to a store slot; returns -1 on a bad index.
int resolve_sorted_slot(uint16_t index) {
    uint16_t slots[VCARD_MAX_CARDS];
    uint16_t n = vcard_store_get_sorted(slots, VCARD_MAX_CARDS);
    if (index >= n) return -1;
    return slots[index];
}

}  // namespace

extern "C" {

int host_vcard_get_own(char* out, size_t out_size) {
    if (!out || out_size == 0) return HOST_ERR_INVALID_ARG;
    if (!vcard_allowed()) return HOST_ERR_NO_CAPABILITY;
    size_t n = vcard_store_get_own(out, out_size);
    return n > 0 ? static_cast<int>(n) : HOST_ERR_NOT_FOUND;
}

int host_vcard_received_count(void) {
    if (!vcard_allowed()) return HOST_ERR_NO_CAPABILITY;
    return vcard_store_count();
}

int host_vcard_received_get(uint16_t index, char* out, size_t out_size) {
    if (!out || out_size == 0) return HOST_ERR_INVALID_ARG;
    if (!vcard_allowed()) return HOST_ERR_NO_CAPABILITY;
    int slot = resolve_sorted_slot(index);
    if (slot < 0) return HOST_ERR_NOT_FOUND;
    size_t n = vcard_store_get(static_cast<uint16_t>(slot), out, out_size);
    return n > 0 ? static_cast<int>(n) : HOST_ERR_NOT_FOUND;
}

int host_vcard_received_display(uint16_t index, char* out, size_t out_size) {
    if (!out || out_size == 0) return HOST_ERR_INVALID_ARG;
    if (!vcard_allowed()) return HOST_ERR_NO_CAPABILITY;
    int slot = resolve_sorted_slot(index);
    if (slot < 0) return HOST_ERR_NOT_FOUND;
    if (!vcard_store_get_display(static_cast<uint16_t>(slot), out, out_size)) {
        return HOST_ERR_NOT_FOUND;
    }
    return static_cast<int>(std::strlen(out));
}

int host_vcard_set_own(const char* vcard, size_t len) {
    if (!vcard || len == 0 || len >= HOST_VCARD_MAX_LEN) return HOST_ERR_INVALID_ARG;
    if (!vcard_allowed()) return HOST_ERR_NO_CAPABILITY;
    char err[64] = {};
    if (!vcard_store_set_own(vcard, len, err, sizeof(err))) {
        LOG_W("PLG_VCARD", "set_own rejected: %s", err);
        return HOST_ERR_INVALID_ARG;
    }
    return HOST_OK;
}

int host_vcard_received_add(const char* vcard, size_t len) {
    if (!vcard || len == 0 || len >= HOST_VCARD_MAX_LEN) return HOST_ERR_INVALID_ARG;
    if (!vcard_allowed()) return HOST_ERR_NO_CAPABILITY;
    if (vcard_store_count() >= VCARD_MAX_CARDS) return HOST_ERR_NO_MEMORY;
    if (vcard_store_contains(vcard, len)) return HOST_ERR_BUSY;
    char err[64] = {};
    if (!vcard_store_add(vcard, len, err, sizeof(err))) {
        LOG_W("PLG_VCARD", "add rejected: %s", err);
        return HOST_ERR_INVALID_ARG;
    }
    return HOST_OK;
}

int host_vcard_received_update(uint16_t index, const char* vcard, size_t len) {
    if (!vcard || len == 0 || len >= HOST_VCARD_MAX_LEN) return HOST_ERR_INVALID_ARG;
    if (!vcard_allowed()) return HOST_ERR_NO_CAPABILITY;
    int slot = resolve_sorted_slot(index);
    if (slot < 0) return HOST_ERR_NOT_FOUND;
    char err[64] = {};
    if (!vcard_store_update(static_cast<uint16_t>(slot), vcard, len, err, sizeof(err))) {
        LOG_W("PLG_VCARD", "update rejected: %s", err);
        return HOST_ERR_INVALID_ARG;
    }
    return HOST_OK;
}

int host_vcard_received_delete(uint16_t index) {
    if (!vcard_allowed()) return HOST_ERR_NO_CAPABILITY;
    int slot = resolve_sorted_slot(index);
    if (slot < 0) return HOST_ERR_NOT_FOUND;
    return vcard_store_delete(static_cast<uint16_t>(slot)) ? HOST_OK : HOST_ERR_GENERIC;
}

}  // extern "C"
