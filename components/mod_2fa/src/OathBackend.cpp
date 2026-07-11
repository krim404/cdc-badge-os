/*
 * Target implementation of the YKOATH applet backend: bridges the applet to
 * the existing OathStore (credentials), NVS (device salt + access key) and
 * mbedtls (HMAC-SHA1 for the CCID access-key challenge/response).
 */

#include "OathApplet.h"
#include "mod_2fa/OathStore.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_log.h"

#include <nvs.h>
#include <mbedtls/md.h>
#include <cstring>

namespace cdc::mod_2fa {
namespace {

constexpr char kNvsNamespace[] = "mod_2fa";
constexpr char kDevIdKey[] = "oath_devid";
constexpr char kAccessKey[] = "oath_akey";

const char* TAG = "OATH";

OathStore& store() { return OathStore::instance(); }

uint16_t beCapacity() { return store().capacity(); }

bool beRead(uint16_t slot, oath_meta_t* out) {
    OathEntry e = {};
    if (!store().readAccount(slot, &e)) {
        out->used = false;
        return false;
    }
    out->used = true;
    strncpy(out->name, e.name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
    strncpy(out->issuer, e.issuer, sizeof(out->issuer) - 1);
    out->issuer[sizeof(out->issuer) - 1] = '\0';
    out->type = e.type;
    out->algorithm = e.algorithm;
    out->digits = e.digits;
    out->period = e.period;
    out->flags = e.flags;
    return true;
}

bool beCalculate(uint16_t slot, const uint8_t challenge[8], uint8_t trunc[4], uint8_t* digits) {
    return store().calculateForChallenge(slot, challenge, trunc, digits);
}

bool beAddRaw(uint8_t type, const char* name, const char* issuer,
              const uint8_t* key, uint8_t keyLen, uint8_t digits,
              uint32_t period, uint8_t algorithm, uint64_t counter, uint8_t flags) {
    // PUT replaces an existing same-name credential (YKOATH semantics).
    uint16_t existing = 0;
    if (store().findByName(name, &existing)) {
        store().deleteAccount(existing);
    }
    return store().addAccountRaw(type, name, issuer, key, keyLen, digits, period,
                                 algorithm, counter, flags);
}

bool beRemove(uint16_t slot) { return store().deleteAccount(slot); }

void beWipeAll() {
    uint16_t cap = store().capacity();
    for (uint16_t s = 0; s < cap; s++) {
        OathEntry e = {};
        if (store().readAccount(s, &e)) store().deleteAccount(s);
    }
}

// Loads (or first-boot generates) the 8-byte device salt used by YKOATH to
// derive the device id and the access-key PBKDF salt (host side).
void beDevId(uint8_t out[8]) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        size_t len = 8;
        if (nvs_get_blob(h, kDevIdKey, out, &len) == ESP_OK && len == 8) {
            nvs_close(h);
            return;
        }
        auto* se = cdc::hal::getSecureElementInstance();
        if (!se || !se->getRandomStrict(out, 8)) {
            for (int i = 0; i < 8; i++) out[i] = static_cast<uint8_t>(0xA0 + i);
        }
        nvs_set_blob(h, kDevIdKey, out, 8);
        nvs_commit(h);
        nvs_close(h);
        return;
    }
    for (int i = 0; i < 8; i++) out[i] = static_cast<uint8_t>(0xA0 + i);
}

bool beAkeyGet(uint8_t out[16]) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 16;
    esp_err_t err = nvs_get_blob(h, kAccessKey, out, &len);
    nvs_close(h);
    return err == ESP_OK && len == 16;
}

bool beAkeySet(const uint8_t key[16]) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, kAccessKey, key, 16);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

void beAkeyClear() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, kAccessKey);
    nvs_commit(h);
    nvs_close(h);
}

bool beHmacSha1(const uint8_t* key, size_t keyLen,
                const uint8_t* data, size_t dataLen, uint8_t out[20]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!info) return false;
    return mbedtls_md_hmac(info, key, keyLen, data, dataLen, out) == 0;
}

bool beRng(uint8_t* buf, size_t len) {
    auto* se = cdc::hal::getSecureElementInstance();
    return se && se->getRandom(buf, static_cast<uint16_t>(len));
}

const oath_backend_t g_backend = {
    beCapacity, beRead, beCalculate, beAddRaw, beRemove, beWipeAll,
    beDevId, beAkeyGet, beAkeySet, beAkeyClear,
    beHmacSha1, beRng,
};

} // namespace

void oath_backend_install() {
    oath_set_backend(&g_backend);
    LOG_I(TAG, "YKOATH backend installed");
}

} // namespace cdc::mod_2fa
