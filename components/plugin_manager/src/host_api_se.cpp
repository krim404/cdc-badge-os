/**
 * \file host_api_se.cpp
 * \brief TROPIC01 SecureElement host API with per-call slot capability checks.
 */

#include "cdc_hal/ISecureElement.h"
#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginManager.h"
#include "cdc_core/TropicSlotMap.h"
#include "nvs.h"

#include <cstdio>
#include <cstring>
#include <string>

extern "C" void* plg_get_active_plugin(void);

using cdc::hal::ISecureElement;
using cdc::hal::SeResult;
using cdc::hal::EccCurve;
using cdc::hal::getSecureElementInstance;

namespace {

// Plugin pool module id (matches MODULE_ID_PLUGIN_POOL in main/tropic_slot_map.h).
// The ECC and RMEM slot RANGES are fetched from the central TropicSlotMap
// (authority) via plugin_pool() below, never hardcoded here, so they cannot
// drift from the map. Plugin ECC/RMEM keys are addressed by name; the ECC
// name->slot mapping is persisted in NVS so a key keeps its slot across reboot.
constexpr uint8_t  PLG_POOL_MODULE_ID  = 7;
constexpr uint8_t  RMEM_HEADER_MAGIC   = 0xCD;
constexpr uint8_t  RMEM_NAME_FIELD_LEN = ISecureElement::RMEM_NAME_LEN;
constexpr char     PLG_ECC_MAP_NS[]    = "plg_ecc_map";  // NVS namespace: name->slot

// Plugin slot pool bounds, fetched from the central TropicSlotMap. An invalid
// or missing range yields an empty pool (start > end) so every caller fails
// safely (RMEM_FULL / NO_MEMORY) and never touches reserved slot 0.
struct PluginPool { uint16_t start; uint16_t end; };

PluginPool plugin_pool(cdc::core::TropicSlotMap::SlotType type) {
    cdc::core::TropicSlotMap::SlotRange r{};
    if (!cdc::core::TropicSlotMap::instance().getRangeByModuleId(PLG_POOL_MODULE_ID, type, &r)
        || !r.valid) {
        return { 1, 0 };  // empty: `slot <= end` is immediately false
    }
    return { r.start, r.end };
}

ISecureElement* se() { return getSecureElementInstance(); }

cdc::plugin_manager::Plugin* active() {
    return static_cast<cdc::plugin_manager::Plugin*>(plg_get_active_plugin());
}

bool active_declares_rmem(const std::string& name) {
    auto* p = active();
    if (!p) return false;
    for (const std::string& n : p->manifest().capabilities.rmem) {
        if (n == name) return true;
    }
    return false;
}

bool any_installed_declares_rmem(const std::string& name) {
    auto& mgr = cdc::plugin_manager::PluginManager::instance();
    for (const std::string& id : mgr.listInstalledIds()) {
        auto man = mgr.getManifest(id);
        if (!man) continue;
        for (const std::string& n : man->capabilities.rmem) {
            if (n == name) return true;
        }
    }
    return false;
}

// Scan the plugin pool. Output `out_existing` is the slot holding `name`, or
// -1 if none. Output `out_reclaimable` is a slot that is empty or stale (not
// claimed by any installed plugin), or -1 if pool is fully populated by
// live names.
void scan_plugin_pool(const std::string& name, int& out_existing, int& out_reclaimable) {
    out_existing = -1;
    out_reclaimable = -1;
    auto* s = se();
    if (!s) return;

    PluginPool pool = plugin_pool(cdc::core::TropicSlotMap::SlotType::RMEM);
    for (uint16_t slot = pool.start; slot <= pool.end; ++slot) {
        ISecureElement::RMemHeader hdr{};
        SeResult rr = s->rmemReadWithHeader(slot, &hdr, nullptr, 0, nullptr);
        if (rr != SeResult::OK || hdr.magic != RMEM_HEADER_MAGIC) {
            if (out_reclaimable < 0) out_reclaimable = slot;
            continue;
        }
        std::string hdr_name(hdr.name, ::strnlen(hdr.name, RMEM_NAME_FIELD_LEN));
        if (hdr_name == name) {
            out_existing = slot;
            return;
        }
        if (out_reclaimable < 0 && !any_installed_declares_rmem(hdr_name)) {
            out_reclaimable = slot;
        }
    }
}

bool active_declares_ecc(const std::string& name) {
    auto* p = active();
    if (!p) return false;
    for (const std::string& n : p->manifest().capabilities.ecc) {
        if (n == name) return true;
    }
    return false;
}

bool any_installed_declares_ecc(const std::string& name) {
    auto& mgr = cdc::plugin_manager::PluginManager::instance();
    for (const std::string& id : mgr.listInstalledIds()) {
        auto man = mgr.getManifest(id);
        if (!man) continue;
        for (const std::string& n : man->capabilities.ecc) {
            if (n == name) return true;
        }
    }
    return false;
}

// Map an ECC key name to a physical pool slot. Returns the slot, or -1 if the
// name is not mapped (and either assignment was not requested or the pool is
// full). When `assign` is set, claims a free or reclaimable slot, persists the
// name->slot mapping in NVS, and wipes any stale key on a reclaimed slot.
int resolve_ecc_slot(const std::string& name, bool assign) {
    nvs_handle_t h;
    if (nvs_open(PLG_ECC_MAP_NS, assign ? NVS_READWRITE : NVS_READONLY, &h) != ESP_OK) {
        return -1;  // namespace absent (no mappings yet) or open failure
    }

    int existing = -1, reclaimable = -1;
    char key[16];
    char stored[HOST_ECC_NAME_MAX + 1];
    PluginPool pool = plugin_pool(cdc::core::TropicSlotMap::SlotType::ECC);
    for (uint16_t slot = pool.start; slot <= pool.end; ++slot) {
        std::snprintf(key, sizeof(key), "s%u", slot);
        size_t sz = sizeof(stored);
        if (nvs_get_str(h, key, stored, &sz) != ESP_OK) {
            if (reclaimable < 0) reclaimable = slot;
            continue;
        }
        if (name == stored) { existing = slot; break; }
        if (reclaimable < 0 && !any_installed_declares_ecc(stored)) reclaimable = slot;
    }

    int result = -1;
    if (existing >= 0) {
        result = existing;
    } else if (assign && reclaimable >= 0) {
        std::snprintf(key, sizeof(key), "s%u", reclaimable);
        if (nvs_set_str(h, key, name.c_str()) == ESP_OK && nvs_commit(h) == ESP_OK) {
            auto* s = se();
            if (s) s->eccDelete(static_cast<uint8_t>(reclaimable));  // wipe stale owner's key
            result = reclaimable;
        }
    }
    nvs_close(h);
    return result;
}

// Drop the name->slot mapping so the slot returns to the pool.
void free_ecc_mapping(uint8_t slot) {
    nvs_handle_t h;
    if (nvs_open(PLG_ECC_MAP_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    std::snprintf(key, sizeof(key), "s%u", slot);
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

int se_rc(SeResult r) {
    switch (r) {
        case SeResult::OK:               return HOST_OK;
        case SeResult::INVALID_PARAM:    return HOST_ERR_INVALID_ARG;
        case SeResult::SLOT_EMPTY:       return HOST_ERR_NOT_FOUND;
        case SeResult::SESSION_REQUIRED: return HOST_ERR_BUSY;
        case SeResult::NOT_SUPPORTED:    return HOST_ERR_NOT_SUPPORTED;
        default:                         return HOST_ERR_GENERIC;
    }
}

EccCurve curve_for(uint8_t curve) {
    return curve == ECC_CURVE_ED25519 ? EccCurve::ED25519 : EccCurve::P256;
}

}  // namespace

extern "C" {

int host_rmem_read_named(const char* name, uint8_t* buf, size_t* len)
{
    if (!name || !len) return HOST_ERR_INVALID_ARG;
    std::string name_s(name);
    if (!active_declares_rmem(name_s)) return HOST_ERR_NO_CAPABILITY;
    auto* s = se();
    if (!s) return HOST_ERR_NOT_FOUND;

    int existing = -1, reclaimable = -1;
    scan_plugin_pool(name_s, existing, reclaimable);
    if (existing < 0) return HOST_ERR_NOT_FOUND;

    ISecureElement::RMemHeader hdr{};
    uint16_t payload_len = 0;
    SeResult rr = s->rmemReadWithHeader(static_cast<uint16_t>(existing), &hdr,
                                         buf, static_cast<uint16_t>(*len), &payload_len);
    if (rr != SeResult::OK) return se_rc(rr);
    *len = payload_len;
    return HOST_OK;
}

int host_rmem_write_named(const char* name, const uint8_t* buf, size_t len)
{
    if (!name || (!buf && len > 0)) return HOST_ERR_INVALID_ARG;
    std::string name_s(name);
    if (name_s.empty() || name_s.size() > HOST_RMEM_NAME_MAX) return HOST_ERR_INVALID_ARG;
    if (!active_declares_rmem(name_s)) return HOST_ERR_NO_CAPABILITY;
    auto* s = se();
    if (!s) return HOST_ERR_NOT_FOUND;

    int existing = -1, reclaimable = -1;
    scan_plugin_pool(name_s, existing, reclaimable);
    int target = (existing >= 0) ? existing : reclaimable;
    if (target < 0) return HOST_ERR_RMEM_FULL;

    return se_rc(s->rmemWriteWithHeader(static_cast<uint16_t>(target),
                                        PLG_POOL_MODULE_ID,
                                        name_s.c_str(),
                                        /*flags=*/0,
                                        buf, static_cast<uint16_t>(len)));
}

int host_rmem_erase_named(const char* name)
{
    if (!name) return HOST_ERR_INVALID_ARG;
    std::string name_s(name);
    if (!active_declares_rmem(name_s)) return HOST_ERR_NO_CAPABILITY;
    auto* s = se();
    if (!s) return HOST_ERR_NOT_FOUND;

    int existing = -1, reclaimable = -1;
    scan_plugin_pool(name_s, existing, reclaimable);
    if (existing < 0) return HOST_ERR_NOT_FOUND;

    return se_rc(s->rmemErase(static_cast<uint16_t>(existing)));
}

bool host_rmem_name_used(const char* name)
{
    if (!name) return false;
    std::string name_s(name);
    if (!active_declares_rmem(name_s)) return false;
    int existing = -1, reclaimable = -1;
    scan_plugin_pool(name_s, existing, reclaimable);
    return existing >= 0;
}

uint16_t host_rmem_slot_size(void)
{
    auto* s = se();
    return s ? s->getRmemSlotSize() : 0;
}

int host_ecc_generate(const char* name, uint8_t curve)
{
    if (!name) return HOST_ERR_INVALID_ARG;
    std::string n(name);
    if (n.empty() || n.size() > HOST_ECC_NAME_MAX) return HOST_ERR_INVALID_ARG;
    if (!active_declares_ecc(n)) return HOST_ERR_NO_CAPABILITY;
    auto* s = se();
    if (!s) return HOST_ERR_NOT_FOUND;
    int slot = resolve_ecc_slot(n, /*assign=*/true);
    if (slot < 0) return HOST_ERR_NO_MEMORY;  // plugin ECC pool exhausted
    return se_rc(s->eccGenerate(static_cast<uint8_t>(slot), curve_for(curve)));
}

int host_ecc_import(const char* /*name*/, const uint8_t* /*priv*/, uint8_t /*curve*/)
{
    return HOST_ERR_NOT_SUPPORTED;  // intentionally not exposed
}

int host_ecc_pubkey(const char* name, uint8_t* pub, uint8_t /*curve*/)
{
    if (!name || !pub) return HOST_ERR_INVALID_ARG;
    std::string n(name);
    if (!active_declares_ecc(n)) return HOST_ERR_NO_CAPABILITY;
    auto* s = se();
    if (!s) return HOST_ERR_NOT_FOUND;
    int slot = resolve_ecc_slot(n, /*assign=*/false);
    if (slot < 0) return HOST_ERR_NOT_FOUND;
    return se_rc(s->eccGetPublicKey(static_cast<uint8_t>(slot), pub, nullptr));
}

int host_ecc_delete(const char* name)
{
    if (!name) return HOST_ERR_INVALID_ARG;
    std::string n(name);
    if (!active_declares_ecc(n)) return HOST_ERR_NO_CAPABILITY;
    auto* s = se();
    if (!s) return HOST_ERR_NOT_FOUND;
    int slot = resolve_ecc_slot(n, /*assign=*/false);
    if (slot < 0) return HOST_ERR_NOT_FOUND;
    int rc = se_rc(s->eccDelete(static_cast<uint8_t>(slot)));
    free_ecc_mapping(static_cast<uint8_t>(slot));  // return the slot to the pool
    return rc;
}

bool host_ecc_exists(const char* name)
{
    if (!name) return false;
    std::string n(name);
    if (!active_declares_ecc(n)) return false;
    auto* s = se();
    if (!s) return false;
    int slot = resolve_ecc_slot(n, /*assign=*/false);
    return slot >= 0 && s->eccSlotUsed(static_cast<uint8_t>(slot));
}

int host_ecdsa_sign(const char* name, const uint8_t* msg, size_t len, uint8_t sig[64])
{
    if (!name || !msg || !sig) return HOST_ERR_INVALID_ARG;
    std::string n(name);
    if (!active_declares_ecc(n)) return HOST_ERR_NO_CAPABILITY;
    auto* s = se();
    if (!s) return HOST_ERR_NOT_FOUND;
    int slot = resolve_ecc_slot(n, /*assign=*/false);
    if (slot < 0) return HOST_ERR_NOT_FOUND;
    size_t sig_len = 64;
    return se_rc(s->ecdsaSign(static_cast<uint8_t>(slot), msg, len, sig, &sig_len));
}

int host_eddsa_sign(const char* name, const uint8_t* msg, size_t len, uint8_t sig[64])
{
    if (!name || !msg || !sig) return HOST_ERR_INVALID_ARG;
    std::string n(name);
    if (!active_declares_ecc(n)) return HOST_ERR_NO_CAPABILITY;
    auto* s = se();
    if (!s) return HOST_ERR_NOT_FOUND;
    int slot = resolve_ecc_slot(n, /*assign=*/false);
    if (slot < 0) return HOST_ERR_NOT_FOUND;
    return se_rc(s->eddsaSign(static_cast<uint8_t>(slot), msg, len, sig));
}

int host_se_chip_id(uint8_t* serial, size_t* len)
{
    if (!serial || !len) return HOST_ERR_INVALID_ARG;
    auto* s = se();
    if (!s) return HOST_ERR_NOT_FOUND;
    return s->getChipId(serial, static_cast<uint8_t>(*len)) ? HOST_OK : HOST_ERR_GENERIC;
}

int host_se_fw_version(uint8_t* riscv, uint8_t* spect)
{
    if (!riscv || !spect) return HOST_ERR_INVALID_ARG;
    auto* s = se();
    if (!s) return HOST_ERR_NOT_FOUND;
    return s->getFwVersion(riscv, spect) ? HOST_OK : HOST_ERR_GENERIC;
}

}  // extern "C"
