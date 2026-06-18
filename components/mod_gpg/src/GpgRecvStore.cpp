#include "mod_gpg/GpgRecvStore.h"
#include "cdc_core/Raii.h"
#include "cdc_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

static const char* TAG = "GPG_RECV";

namespace cdc::mod_gpg {

namespace {
constexpr const char* kNamespace  = "gpg_recv";
constexpr const char* kKeyPrefix  = "pk_";
constexpr size_t      kNvsKeyLen  = 11; // "pk_" + 8 hex chars

bool hasKeyPrefix(const char* k) {
    return k && std::strncmp(k, kKeyPrefix, 3) == 0;
}
} // namespace

GpgRecvStore& GpgRecvStore::instance() {
    static GpgRecvStore inst;
    return inst;
}

void GpgRecvStore::deriveKeyName(const uint8_t fp_v4[20], char out[16]) {
    std::snprintf(out, 16, "%s%02x%02x%02x%02x",
                  kKeyPrefix, fp_v4[0], fp_v4[1], fp_v4[2], fp_v4[3]);
}

bool GpgRecvStore::readByName(const char* nvs_key, gpg_recv_key_t* out) {
    if (!nvs_key || !out) return false;
    ::cdc::core::NvsScope nvs(kNamespace, NVS_READONLY);
    if (!nvs) return false;
    size_t expected = sizeof(gpg_recv_key_t);
    if (nvs_get_blob(nvs, nvs_key, out, &expected) != ESP_OK) return false;
    return expected == sizeof(gpg_recv_key_t);
}

bool GpgRecvStore::writeByName(const char* nvs_key, const gpg_recv_key_t& key) {
    if (!nvs_key) return false;
    ::cdc::core::NvsScope nvs(kNamespace, NVS_READWRITE);
    if (!nvs) return false;
    if (nvs_set_blob(nvs, nvs_key, &key, sizeof(key)) != ESP_OK) return false;
    return nvs.commit() == ESP_OK;
}

bool GpgRecvStore::addKey(const gpg_recv_key_t& key) {
    char name[16];
    deriveKeyName(key.fingerprint_v4, name);

    // Refuse new inserts past the cap, but always allow overwriting
    // an entry that already exists with the same fingerprint.
    gpg_recv_key_t probe;
    bool exists = readByName(name, &probe);
    if (!exists && count() >= kMaxKeys) {
        LOG_W(TAG, "Store full (%u keys)", static_cast<unsigned>(kMaxKeys));
        return false;
    }
    return writeByName(name, key);
}

uint8_t GpgRecvStore::count() {
    nvs_iterator_t it = nullptr;
    if (nvs_entry_find("nvs", kNamespace, NVS_TYPE_BLOB, &it) != ESP_OK || !it) {
        return 0;
    }
    uint8_t n = 0;
    while (it != nullptr) {
        nvs_entry_info_t info = {};
        nvs_entry_info(it, &info);
        if (hasKeyPrefix(info.key)) ++n;
        if (n >= kMaxKeys) break;
        if (nvs_entry_next(&it) != ESP_OK) break;
    }
    nvs_release_iterator(it);
    return n;
}

uint8_t GpgRecvStore::listIndex(gpg_recv_index_entry_t* out, uint8_t max) {
    if (!out || max == 0) return 0;

    nvs_iterator_t it = nullptr;
    if (nvs_entry_find("nvs", kNamespace, NVS_TYPE_BLOB, &it) != ESP_OK || !it) {
        return 0;
    }

    uint8_t n = 0;
    while (it != nullptr && n < max) {
        nvs_entry_info_t info = {};
        nvs_entry_info(it, &info);
        if (hasKeyPrefix(info.key)) {
            gpg_recv_key_t k;
            if (readByName(info.key, &k)) {
                std::snprintf(out[n].nvs_key, sizeof(out[n].nvs_key), "%s", info.key);
                out[n].received_at = k.received_at;
                out[n].flags = k.flags;
                ++n;
            }
        }
        if (nvs_entry_next(&it) != ESP_OK) break;
    }
    nvs_release_iterator(it);

    std::sort(out, out + n,
              [](const gpg_recv_index_entry_t& a, const gpg_recv_index_entry_t& b) {
                  return a.received_at < b.received_at;
              });
    return n;
}

bool GpgRecvStore::resolveKeyName(uint8_t index, char out[16]) {
    auto buf = ::cdc::core::psramAlloc<gpg_recv_index_entry_t>(kMaxKeys);
    if (!buf) return false;
    uint8_t n = listIndex(buf.get(), kMaxKeys);
    if (index >= n) return false;
    std::snprintf(out, 16, "%s", buf[index].nvs_key);
    return true;
}

bool GpgRecvStore::getKey(uint8_t index, gpg_recv_key_t* out) {
    if (!out) return false;
    char name[16];
    if (!resolveKeyName(index, name)) return false;
    return readByName(name, out);
}

bool GpgRecvStore::deleteKey(uint8_t index) {
    char name[16];
    if (!resolveKeyName(index, name)) return false;

    ::cdc::core::NvsScope nvs(kNamespace, NVS_READWRITE);
    if (!nvs) return false;
    if (nvs_erase_key(nvs, name) != ESP_OK) return false;
    return nvs.commit() == ESP_OK;
}

bool GpgRecvStore::setSignature(uint8_t index,
                                const uint8_t* sig, uint8_t sig_len,
                                uint32_t sig_created_at,
                                uint8_t flags)
{
    if (!sig || sig_len == 0 || sig_len > sizeof(gpg_recv_key_t::my_signature)) {
        return false;
    }

    char name[16];
    if (!resolveKeyName(index, name)) return false;

    gpg_recv_key_t key;
    if (!readByName(name, &key)) return false;

    std::memset(key.my_signature, 0, sizeof(key.my_signature));
    std::memcpy(key.my_signature, sig, sig_len);
    key.sig_len = sig_len;
    key.sig_created_at = sig_created_at;
    key.flags = flags;

    return writeByName(name, key);
}

} // namespace cdc::mod_gpg
