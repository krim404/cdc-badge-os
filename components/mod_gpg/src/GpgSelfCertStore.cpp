#include "mod_gpg/GpgSelfCertStore.h"
#include "cdc_core/Raii.h"
#include "cdc_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

static const char* TAG = "GPG_SCERT";

namespace cdc::mod_gpg {

namespace {
constexpr const char* kNamespace = "gpg_selfcert";
constexpr const char* kKeyPrefix = "ct_";

bool hasKeyPrefix(const char* k) {
    return k && std::strncmp(k, kKeyPrefix, 3) == 0;
}
} // namespace

GpgSelfCertStore& GpgSelfCertStore::instance() {
    static GpgSelfCertStore inst;
    return inst;
}

void GpgSelfCertStore::deriveKeyName(const uint8_t issuer_fp_v4[20], char out[16]) {
    std::snprintf(out, 16, "%s%02x%02x%02x%02x",
                  kKeyPrefix, issuer_fp_v4[0], issuer_fp_v4[1],
                  issuer_fp_v4[2], issuer_fp_v4[3]);
}

bool GpgSelfCertStore::readByName(const char* nvs_key, gpg_self_cert_t* out) {
    if (!nvs_key || !out) return false;
    ::cdc::core::NvsScope nvs(kNamespace, NVS_READONLY);
    if (!nvs) return false;
    size_t expected = sizeof(gpg_self_cert_t);
    if (nvs_get_blob(nvs, nvs_key, out, &expected) != ESP_OK) return false;
    return expected == sizeof(gpg_self_cert_t);
}

bool GpgSelfCertStore::writeByName(const char* nvs_key, const gpg_self_cert_t& cert) {
    if (!nvs_key) return false;
    ::cdc::core::NvsScope nvs(kNamespace, NVS_READWRITE);
    if (!nvs) return false;
    if (nvs_set_blob(nvs, nvs_key, &cert, sizeof(cert)) != ESP_OK) return false;
    return nvs.commit() == ESP_OK;
}

bool GpgSelfCertStore::addCert(const gpg_self_cert_t& cert) {
    if (cert.sig_pkt_len == 0 || cert.sig_pkt_len > kGpgSelfCertSigMax) return false;

    char name[16];
    deriveKeyName(cert.issuer_fp_v4, name);

    gpg_self_cert_t probe;
    bool exists = readByName(name, &probe);
    if (!exists && count() >= kMaxCerts) {
        LOG_W(TAG, "Store full (%u certs)", static_cast<unsigned>(kMaxCerts));
        return false;
    }
    return writeByName(name, cert);
}

uint8_t GpgSelfCertStore::count() {
    nvs_iterator_t it = nullptr;
    if (nvs_entry_find("nvs", kNamespace, NVS_TYPE_BLOB, &it) != ESP_OK || !it) {
        return 0;
    }
    uint8_t n = 0;
    while (it != nullptr) {
        nvs_entry_info_t info = {};
        nvs_entry_info(it, &info);
        if (hasKeyPrefix(info.key)) ++n;
        if (n >= kMaxCerts) break;
        if (nvs_entry_next(&it) != ESP_OK) break;
    }
    nvs_release_iterator(it);
    return n;
}

uint8_t GpgSelfCertStore::listIndex(gpg_self_cert_index_entry_t* out, uint8_t max) {
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
            gpg_self_cert_t c;
            if (readByName(info.key, &c)) {
                std::snprintf(out[n].nvs_key, sizeof(out[n].nvs_key), "%s", info.key);
                out[n].received_at = c.received_at;
                ++n;
            }
        }
        if (nvs_entry_next(&it) != ESP_OK) break;
    }
    nvs_release_iterator(it);

    std::sort(out, out + n,
              [](const gpg_self_cert_index_entry_t& a, const gpg_self_cert_index_entry_t& b) {
                  return a.received_at < b.received_at;
              });
    return n;
}

bool GpgSelfCertStore::resolveKeyName(uint8_t index, char out[16]) {
    auto buf = ::cdc::core::psramAlloc<gpg_self_cert_index_entry_t>(kMaxCerts);
    if (!buf) return false;
    uint8_t n = listIndex(buf.get(), kMaxCerts);
    if (index >= n) return false;
    std::snprintf(out, 16, "%s", buf[index].nvs_key);
    return true;
}

bool GpgSelfCertStore::getCert(uint8_t index, gpg_self_cert_t* out) {
    if (!out) return false;
    char name[16];
    if (!resolveKeyName(index, name)) return false;
    return readByName(name, out);
}

bool GpgSelfCertStore::deleteCert(uint8_t index) {
    char name[16];
    if (!resolveKeyName(index, name)) return false;

    ::cdc::core::NvsScope nvs(kNamespace, NVS_READWRITE);
    if (!nvs) return false;
    if (nvs_erase_key(nvs, name) != ESP_OK) return false;
    return nvs.commit() == ESP_OK;
}

} // namespace cdc::mod_gpg
