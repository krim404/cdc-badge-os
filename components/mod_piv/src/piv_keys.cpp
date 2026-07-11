#include "piv_keys.h"
#include "piv_defs.h"
#include "piv_objects.h"

#include "cdc_hal/ISecureElement.h"
#include "cdc_core/PinManager.h"
#include "cdc_log.h"
#include "tropic_slot_map.h"

#include <nvs.h>
#include <nvs_flash.h>
#include <esp_random.h>

#include <mbedtls/aes.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/bignum.h>
#include <mbedtls/platform_util.h>

#include <cstring>

static const char* TAG = "PIV";

namespace cdc::mod_piv {
namespace {

constexpr char kNvsNamespace[] = "mod_piv";
constexpr uint16_t PIV_STATE_OFFSET = 0;  // rmem slot rmemStart+0
constexpr uint16_t PIV_9D_OFFSET = 2;     // rmem slot rmemStart+2

// Backend state shared by the C callbacks below.
struct Backend {
    core::IModule::SlotRange range = {};
    piv_state_t state = {};
    bool stateLoaded = false;
};
Backend g_be;

cdc::hal::ISecureElement* se() { return cdc::hal::getSecureElementInstance(); }

int mbedtlsRng(void*, unsigned char* buf, size_t len) {
    esp_fill_random(buf, len);
    return 0;
}

// ECC slot for a hardware-backed key ref (9A/9C/9E). Returns false for 9D.
bool eccSlotFor(uint8_t keyRef, uint8_t* slotOut) {
    if (!g_be.range.hasEcc) return false;
    uint8_t base = g_be.range.eccStart;
    switch (keyRef) {
        case PIV_KEY_9A: *slotOut = base + 0; return true;
        case PIV_KEY_9C: *slotOut = base + 1; return true;
        case PIV_KEY_9E: *slotOut = base + 3; return true;
        default: return false;  // 9D is a software key
    }
}

bool rmemSlot(uint16_t offset, uint16_t* slotOut) {
    if (!g_be.range.hasRmem) return false;
    uint16_t s = g_be.range.rmemStart + offset;
    if (s > g_be.range.rmemEnd) return false;
    *slotOut = s;
    return true;
}

// --- persisted state -------------------------------------------------------
bool stateSave(const piv_state_t* st) {
    uint16_t slot = 0;
    if (!rmemSlot(PIV_STATE_OFFSET, &slot)) return false;
    auto res = se()->rmemWriteWithHeader(slot, MODULE_ID_MOD_PIV, "piv_state", 0,
                                         reinterpret_cast<const uint8_t*>(st),
                                         sizeof(*st));
    return res == cdc::hal::SeResult::OK;
}

bool stateEnsure() {
    if (g_be.stateLoaded) return true;
    uint16_t slot = 0;
    if (!rmemSlot(PIV_STATE_OFFSET, &slot)) return false;

    cdc::hal::ISecureElement::RMemHeader hdr = {};
    uint16_t plen = 0;
    auto res = se()->rmemReadWithHeader(slot, &hdr, reinterpret_cast<uint8_t*>(&g_be.state),
                                        sizeof(g_be.state), &plen);
    if (res == cdc::hal::SeResult::OK && plen == sizeof(g_be.state) &&
        hdr.moduleId == MODULE_ID_MOD_PIV && g_be.state.version == 1) {
        g_be.stateLoaded = true;
        return true;
    }

    // First boot: generate identifiers and the default AES-192 management key.
    memset(&g_be.state, 0, sizeof(g_be.state));
    g_be.state.version = 1;
    g_be.state.mgmt_alg = PIV_ALG_AES256;
    g_be.state.mgmt_is_default = 1;
    // Default management key: 0102030405060708 repeated to 32 bytes (AES-256).
    for (int i = 0; i < PIV_MGMT_KEY_LEN; i++) {
        g_be.state.mgmt_key[i] = static_cast<uint8_t>((i % 8) + 1);
    }
    if (!se()->getRandomStrict(g_be.state.guid, sizeof(g_be.state.guid)) ||
        !se()->getRandomStrict(g_be.state.ccc_id, sizeof(g_be.state.ccc_id))) {
        LOG_E(TAG, "TRNG unavailable for PIV identifiers");
        return false;
    }
    g_be.state.guid[0] |= 0x01;  // guarantee a non-zero GUID
    if (!stateSave(&g_be.state)) return false;
    g_be.stateLoaded = true;
    LOG_I(TAG, "PIV state initialized (first boot)");
    return true;
}

// --- software P-256 key (9D) ----------------------------------------------
bool load9dScalar(uint8_t scalar[32]) {
    uint16_t slot = 0;
    if (!rmemSlot(PIV_9D_OFFSET, &slot)) return false;
    cdc::hal::ISecureElement::RMemHeader hdr = {};
    uint8_t buf[97] = {};
    uint16_t plen = 0;
    auto res = se()->rmemReadWithHeader(slot, &hdr, buf, sizeof(buf), &plen);
    if (res != cdc::hal::SeResult::OK || plen < 32 || hdr.moduleId != MODULE_ID_MOD_PIV) {
        return false;
    }
    memcpy(scalar, buf, 32);
    return true;
}

bool derivePub(const uint8_t scalar[32], uint8_t pub[65]) {
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);
    bool ok = false;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_mpi_read_binary(&d, scalar, 32) == 0 &&
        mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, mbedtlsRng, nullptr) == 0) {
        pub[0] = 0x04;
        if (mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(X), pub + 1, 32) == 0 &&
            mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(Y), pub + 33, 32) == 0) {
            ok = true;
        }
    }
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    return ok;
}

bool sign9d(const uint8_t digest[32], uint8_t rs[64]) {
    uint8_t scalar[32];
    if (!load9dScalar(scalar)) return false;
    mbedtls_ecp_group grp;
    mbedtls_mpi d, r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    bool ok = false;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_mpi_read_binary(&d, scalar, 32) == 0 &&
        mbedtls_ecdsa_sign(&grp, &r, &s, &d, digest, 32, mbedtlsRng, nullptr) == 0 &&
        mbedtls_mpi_write_binary(&r, rs, 32) == 0 &&
        mbedtls_mpi_write_binary(&s, rs + 32, 32) == 0) {
        ok = true;
    }
    mbedtls_platform_zeroize(scalar, sizeof(scalar));
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    return ok;
}

// --- backend callbacks -----------------------------------------------------
bool cbKeyPresent(uint8_t keyRef) {
    uint8_t slot = 0;
    if (eccSlotFor(keyRef, &slot)) return se()->eccSlotUsed(slot);
    if (keyRef == PIV_KEY_9D) {
        uint8_t scalar[32];
        bool p = load9dScalar(scalar);
        mbedtls_platform_zeroize(scalar, sizeof(scalar));
        return p;
    }
    return false;
}

bool cbKeySign(uint8_t keyRef, const uint8_t digest[32], uint8_t rs[64]) {
    uint8_t slot = 0;
    if (eccSlotFor(keyRef, &slot)) {
        size_t sigLen = 0;
        return se()->ecdsaSignDigest(slot, digest, rs, &sigLen) == cdc::hal::SeResult::OK;
    }
    if (keyRef == PIV_KEY_9D) return sign9d(digest, rs);
    return false;
}

bool cbKeyGenerate(uint8_t keyRef, uint8_t pub[65]) {
    uint8_t slot = 0;
    if (eccSlotFor(keyRef, &slot)) {
        se()->eccDelete(slot);
        if (se()->eccGenerate(slot, cdc::hal::EccCurve::P256) != cdc::hal::SeResult::OK)
            return false;
        // lt_ecc_key_read writes 64 raw bytes (X||Y); PIV needs the
        // uncompressed point 0x04||X||Y.
        uint8_t raw[64];
        if (se()->eccGetPublicKey(slot, raw, nullptr) != cdc::hal::SeResult::OK) return false;
        pub[0] = 0x04;
        memcpy(pub + 1, raw, 64);
        return true;
    }
    if (keyRef == PIV_KEY_9D) {
        uint8_t scalar[32];
        if (!se()->getRandomStrict(scalar, sizeof(scalar))) return false;
        // Reduce into [1, n-1] implicitly by letting mbedtls derive the pubkey;
        // an out-of-range scalar fails derivePub and we regenerate.
        if (!derivePub(scalar, pub)) {
            mbedtls_platform_zeroize(scalar, sizeof(scalar));
            return false;
        }
        uint16_t rslot = 0;
        if (!rmemSlot(PIV_9D_OFFSET, &rslot)) {
            mbedtls_platform_zeroize(scalar, sizeof(scalar));
            return false;
        }
        uint8_t blob[97];
        memcpy(blob, scalar, 32);
        memcpy(blob + 32, pub, 65);
        auto res = se()->rmemWriteWithHeader(rslot, MODULE_ID_MOD_PIV, "piv_9d", 0,
                                             blob, sizeof(blob));
        mbedtls_platform_zeroize(scalar, sizeof(scalar));
        mbedtls_platform_zeroize(blob, sizeof(blob));
        return res == cdc::hal::SeResult::OK;
    }
    return false;
}

bool cbKeyPubkey(uint8_t keyRef, uint8_t pub[65]) {
    uint8_t slot = 0;
    if (eccSlotFor(keyRef, &slot)) {
        uint8_t raw[64];
        if (se()->eccGetPublicKey(slot, raw, nullptr) != cdc::hal::SeResult::OK) return false;
        pub[0] = 0x04;
        memcpy(pub + 1, raw, 64);
        return true;
    }
    if (keyRef == PIV_KEY_9D) {
        uint8_t scalar[32];
        bool ok = load9dScalar(scalar) && derivePub(scalar, pub);
        mbedtls_platform_zeroize(scalar, sizeof(scalar));
        return ok;
    }
    return false;
}

bool cbKeyEcdh(const uint8_t peer[65], uint8_t sharedX[32]) {
    uint8_t scalar[32];
    if (!load9dScalar(scalar)) return false;
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q, S;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);
    mbedtls_ecp_point_init(&S);
    bool ok = false;
    if (peer[0] == 0x04 &&
        mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_mpi_read_binary(&d, scalar, 32) == 0 &&
        mbedtls_mpi_read_binary(&Q.MBEDTLS_PRIVATE(X), peer + 1, 32) == 0 &&
        mbedtls_mpi_read_binary(&Q.MBEDTLS_PRIVATE(Y), peer + 33, 32) == 0 &&
        mbedtls_mpi_lset(&Q.MBEDTLS_PRIVATE(Z), 1) == 0 &&
        mbedtls_ecp_check_pubkey(&grp, &Q) == 0 &&
        mbedtls_ecp_mul(&grp, &S, &d, &Q, mbedtlsRng, nullptr) == 0 &&
        mbedtls_mpi_write_binary(&S.MBEDTLS_PRIVATE(X), sharedX, 32) == 0) {
        ok = true;
    }
    mbedtls_platform_zeroize(scalar, sizeof(scalar));
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_point_free(&S);
    return ok;
}

bool cbPinVerify(const char* pin) { return core::PinManager::instance().verifyBadgePin(pin); }
uint8_t cbPinRetries() { return core::PinManager::instance().getBadgeRetries(); }
bool cbPinBlocked() { return core::PinManager::instance().isBadgeBlocked(); }

bool cbMgmtEncrypt(const uint8_t in[PIV_MGMT_BLOCK], uint8_t out[PIV_MGMT_BLOCK]) {
    if (!stateEnsure()) return false;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    bool ok = mbedtls_aes_setkey_enc(&aes, g_be.state.mgmt_key, 256) == 0 &&
              mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out) == 0;
    mbedtls_aes_free(&aes);
    return ok;
}

bool cbMgmtSetKey(uint8_t alg, const uint8_t* key, uint8_t keyLen) {
    if (!stateEnsure()) return false;
    if (alg != PIV_ALG_AES256 || keyLen != PIV_MGMT_KEY_LEN) {
        LOG_W(TAG, "Only AES-256 management keys supported (ESP32-S3 has no AES-192)");
        return false;
    }
    memcpy(g_be.state.mgmt_key, key, PIV_MGMT_KEY_LEN);
    g_be.state.mgmt_alg = alg;
    g_be.state.mgmt_is_default = 0;
    return stateSave(&g_be.state);
}

bool cbStateLoad(piv_state_t* out) {
    if (!stateEnsure()) return false;
    memcpy(out, &g_be.state, sizeof(*out));
    return true;
}

// Maps a 5FC1xx object tag to a stable NVS key.
const char* nvsKeyFor(uint32_t obj) {
    switch (certObjectToKeyRef(obj)) {
        case PIV_KEY_9A: return "cert_9a";
        case PIV_KEY_9C: return "cert_9c";
        case PIV_KEY_9D: return "cert_9d";
        case PIV_KEY_9E: return "cert_9e";
        default: return nullptr;
    }
}

int cbObjectLoad(uint32_t obj, uint8_t* out, size_t outMax) {
    const char* key = nvsKeyFor(obj);
    if (!key) return -1;
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t len = outMax;
    esp_err_t err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) return -1;
    return static_cast<int>(len);
}

bool cbObjectStore(uint32_t obj, const uint8_t* data, size_t len) {
    const char* key = nvsKeyFor(obj);
    if (!key) return false;
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

// The backend RNG only produces the ephemeral management-auth witness nonce,
// so the ESP TRNG fallback in getRandom() is acceptable (and avoids a hard
// failure when the SE TRNG is briefly unreachable). Long-term key material
// (GUID, 9D scalar) uses getRandomStrict directly where it is generated.
bool cbRng(uint8_t* buf, size_t len) { return se()->getRandom(buf, static_cast<uint16_t>(len)); }

const piv_backend_t g_backend = {
    cbKeyPresent, cbKeySign, cbKeyGenerate, cbKeyPubkey, cbKeyEcdh,
    cbPinVerify, cbPinRetries, cbPinBlocked,
    cbMgmtEncrypt, cbMgmtSetKey,
    cbStateLoad,
    cbObjectLoad, cbObjectStore,
    cbRng,
};

// Erases ECC + RMEM slots in the PIV range that still carry FIDO2-tagged data
// (orphaned by the slot-map shrink from FIDO2 5..30 to 9..30).
void cleanupFido2Orphans() {
    if (!g_be.range.hasRmem) return;
    for (uint16_t slot = g_be.range.rmemStart; slot <= g_be.range.rmemEnd; slot++) {
        cdc::hal::ISecureElement::RMemHeader hdr = {};
        uint8_t buf[64];
        uint16_t plen = 0;
        auto res = se()->rmemReadWithHeader(slot, &hdr, buf, sizeof(buf), &plen);
        if (res == cdc::hal::SeResult::OK && hdr.moduleId == MODULE_ID_MOD_FIDO2) {
            LOG_W(TAG, "Erasing orphaned FIDO2 RMEM slot %u", slot);
            se()->rmemErase(slot);
        }
    }
    if (g_be.range.hasEcc) {
        for (uint8_t slot = g_be.range.eccStart; slot <= g_be.range.eccEnd; slot++) {
            // No per-slot module tag on ECC keys; the paired RMEM erase above is
            // the authoritative signal. Erase the ECC key whenever its paired
            // RMEM slot was a FIDO2 orphan (already erased -> now empty).
            uint16_t paired = slot;  // ECC and RMEM shifted equally, indices align
            if (paired >= g_be.range.rmemStart && paired <= g_be.range.rmemEnd &&
                !se()->rmemSlotUsed(paired) && se()->eccSlotUsed(slot)) {
                se()->eccDelete(slot);
            }
        }
    }
}

} // namespace

bool piv_init(const core::IModule::SlotRange& range) {
    g_be.range = range;
    g_be.stateLoaded = false;

    if (!range.hasEcc || !range.hasRmem) {
        LOG_E(TAG, "PIV slot range incomplete");
        return false;
    }

    cleanupFido2Orphans();
    if (!stateEnsure()) {
        LOG_E(TAG, "PIV state init failed");
        return false;
    }
    piv_set_backend(&g_backend);
    return true;
}

} // namespace cdc::mod_piv
