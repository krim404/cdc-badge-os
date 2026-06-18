#include "mod_gpg/gpg.h"
#include "mod_gpg/GpgStorage.h"
#include "mod_gpg/openpgp/openpgp.h"
#include "mod_gpg/openpgp/constants.h"
#include "ecdh.h"
#include "openpgp/fingerprint.h"
#include "cdc_core/KeyFingerprint.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_log.h"
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform_util.h>
#include <cstring>
#include <ctime>

namespace {

constexpr const char* TAG = "GPG";

// Sizes provided by mod_gpg/openpgp/constants.h:
// MPI_HEADER_SIZE, ED25519_PUBKEY_SIZE, P256_PUBKEY_SIZE,
// P256_PUBKEY_BITS, MPI_FULL_SIZE_ED25519, MPI_FULL_SIZE_P256

char s_pending_user_id[GPG_USER_ID_MAX] = {};

uint32_t get_unix_time(void) {
    return static_cast<uint32_t>(time(nullptr));
}

// TROPIC01 writes a fixed 64-byte buffer for both Ed25519 (first 32 bytes used)
// and P-256 (raw X||Y, no leading 0x04 indicator). Callers receive 64 bytes for
// P-256 and 32 bytes for Ed25519; any 0x04 SEC1 prefix must be added by the
// consumer.
bool se_get_pubkey(uint8_t slot, uint8_t* pubkey, size_t max_len, uint8_t* curve_out) {
    auto* se = cdc::hal::getSecureElementInstance();
    if (!se || !pubkey) return false;
    if (max_len < 64) {
        LOG_E(TAG, "se_get_pubkey: buffer too small (slot=%u max_len=%zu)", slot, max_len);
        return false;
    }
    cdc::hal::EccCurve curve = cdc::hal::EccCurve::P256;
    auto res = se->eccGetPublicKey(slot, pubkey, &curve);
    if (res != cdc::hal::SeResult::OK) {
        LOG_E(TAG, "se_get_pubkey: slot=%u failed (%d)", slot, static_cast<int>(res));
        return false;
    }
    if (curve_out) {
        *curve_out = (curve == cdc::hal::EccCurve::ED25519) ? CDC_CURVE_ED25519
                                                             : CDC_CURVE_P256;
    }
    return true;
}

bool se_generate_key(uint8_t slot, uint8_t curve) {
    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) {
        LOG_E(TAG, "se_generate_key: no SE (slot=%u)", slot);
        return false;
    }
    auto c = (curve == CDC_CURVE_ED25519) ? cdc::hal::EccCurve::ED25519
                                           : cdc::hal::EccCurve::P256;
    cdc::hal::SeResult res = se->eccGenerate(slot, c);
    if (res != cdc::hal::SeResult::OK) {
        LOG_W(TAG, "se_generate_key: slot %u initial fail (%d), retry after delete",
              slot, static_cast<int>(res));
        se->eccDelete(slot);
        res = se->eccGenerate(slot, c);
    }
    if (res != cdc::hal::SeResult::OK) {
        LOG_E(TAG, "se_generate_key: slot %u curve=%u failed (%d)",
              slot, curve, static_cast<int>(res));
        return false;
    }
    return true;
}

bool calculate_fingerprint(const uint8_t *pubkey, uint8_t curve,
                           uint32_t created_at, uint8_t *fp_out) {
    const size_t pubkey_len = (curve == CDC_CURVE_ED25519) ? ED25519_PUBKEY_SIZE : 64;
    return ::cdc::mod_gpg::calculateFingerprintV4(curve, pubkey, pubkey_len,
                                                  created_at, fp_out);
}

}  // namespace

bool gpg_init(void) {
    s_pending_user_id[0] = '\0';
    return gpg_storage_ready();
}

bool gpg_is_initialized(void) {
    return openpgp_has_any_key();
}

bool gpg_get_status(gpg_status_t *status) {
    if (!status) return false;
    memset(status, 0, sizeof(*status));
    if (!openpgp_has_any_key()) return false;

    status->initialized = true;

    uint8_t fp[GPG_FINGERPRINT_LEN] = {0};
    if (openpgp_get_fingerprint(KEY_SIG, fp)) {
        memcpy(status->fingerprint, fp, sizeof(status->fingerprint));
    }
    status->created_at = openpgp_get_gen_time(KEY_SIG);
    status->sign_count = openpgp_get_sig_count();
    // Read the actual SIG-key curve from the secure element so EdDSA/ECDSA
    // labelling and the v4 fingerprint body match the on-card key.
    uint8_t sig_pub[64] = {0};
    uint8_t sig_curve = CDC_CURVE_ED25519;
    status->curve = se_get_pubkey(gpg_storage_sig_slot(), sig_pub, sizeof(sig_pub),
                                  &sig_curve)
                        ? sig_curve
                        : CDC_CURVE_ED25519;

    char name[GPG_USER_ID_MAX] = {0};
    openpgp_get_cardholder_name(name, sizeof(name));
    if (name[0]) {
        strncpy(status->user_id, name, sizeof(status->user_id) - 1);
    }
    return true;
}

bool gpg_set_pending_user_id(const char *user_id) {
    if (!user_id || !user_id[0]) return false;
    strncpy(s_pending_user_id, user_id, sizeof(s_pending_user_id) - 1);
    s_pending_user_id[sizeof(s_pending_user_id) - 1] = '\0';
    return true;
}

bool gpg_has_pending_user_id(void) {
    return s_pending_user_id[0] != '\0';
}

bool gpg_generate_key(uint8_t curve) {
    if (!gpg_storage_ready()) {
        LOG_E(TAG, "generate: storage not ready");
        return false;
    }
    if (!gpg_has_pending_user_id() && !openpgp_has_any_key()) {
        LOG_E(TAG, "generate: missing user-id");
        return false;
    }

    uint8_t sig_slot = gpg_storage_sig_slot();
    uint8_t dec_slot = gpg_storage_dec_slot();
    uint8_t aut_slot = gpg_storage_aut_slot();
    LOG_I(TAG, "generate: curve=%u sig_slot=%u dec_slot=%u aut_slot=%u",
          curve, sig_slot, dec_slot, aut_slot);

    // SIG and AUT keys live on TROPIC01.
    if (!se_generate_key(sig_slot, curve)) {
        LOG_E(TAG, "generate: SIG key generation failed");
        return false;
    }
    if (!se_generate_key(aut_slot, curve)) {
        LOG_E(TAG, "generate: AUT key generation failed");
        return false;
    }

    // DEC key uses software ECDH P-256 (TROPIC01 has no ECDH primitive). The
    // matching ECC slot stays empty; the encrypted private key lives in
    // R-Memory via gpg_storage_save_dec_privkey().
    auto* se = cdc::hal::getSecureElementInstance();
    if (se) {
        se->eccDelete(dec_slot);
    }
    uint8_t dec_priv[P256_PRIVKEY_SIZE] = {};
    uint8_t dec_pub65[P256_PUBKEY_SIZE] = {};
    if (!ecdh_p256_generate_keypair(dec_priv, dec_pub65)) {
        LOG_E(TAG, "generate: DEC ECDH keypair failed");
        return false;
    }
    bool dec_saved = gpg_storage_save_dec_privkey(dec_priv, nullptr);
    mbedtls_platform_zeroize(dec_priv, sizeof(dec_priv));
    if (!dec_saved) {
        LOG_E(TAG, "generate: DEC privkey save failed");
        return false;
    }

    uint32_t created_at = get_unix_time();

    uint8_t pub_sig[64] = {};
    uint8_t pub_aut[64] = {};
    uint8_t curve_sig = curve;
    uint8_t curve_aut = curve;
    if (!se_get_pubkey(sig_slot, pub_sig, sizeof(pub_sig), &curve_sig)) {
        LOG_E(TAG, "generate: read SIG pubkey failed");
        return false;
    }
    if (!se_get_pubkey(aut_slot, pub_aut, sizeof(pub_aut), &curve_aut)) {
        LOG_E(TAG, "generate: read AUT pubkey failed");
        return false;
    }

    uint8_t fp_sig[GPG_FINGERPRINT_LEN] = {};
    uint8_t fp_dec[GPG_FINGERPRINT_LEN] = {};
    uint8_t fp_aut[GPG_FINGERPRINT_LEN] = {};
    if (!calculate_fingerprint(pub_sig, curve_sig, created_at, fp_sig)) {
        LOG_E(TAG, "generate: SIG fingerprint failed");
        return false;
    }
    // dec_pub65 layout is 0x04 || X || Y. calculate_fingerprint() expects raw
    // X || Y for P-256, so skip the leading byte.
    if (!calculate_fingerprint(dec_pub65 + 1, CDC_CURVE_P256, created_at, fp_dec)) {
        LOG_E(TAG, "generate: DEC fingerprint failed");
        return false;
    }
    if (!calculate_fingerprint(pub_aut, curve_aut, created_at, fp_aut)) {
        LOG_E(TAG, "generate: AUT fingerprint failed");
        return false;
    }

    openpgp_set_key_fingerprint(KEY_SIG, fp_sig, created_at);
    openpgp_set_key_fingerprint(KEY_DEC, fp_dec, created_at);
    openpgp_set_key_fingerprint(KEY_AUT, fp_aut, created_at);

    if (s_pending_user_id[0]) {
        openpgp_set_cardholder_name(s_pending_user_id);
    }

    s_pending_user_id[0] = '\0';
    LOG_I(TAG, "generate: success");
    return true;
}

bool gpg_reset(void) {
    if (!gpg_storage_ready()) return false;
    // openpgp_factory_reset() wipes ECC slots, DEC privkey, NVS state
    // (fingerprints, gen-times, counter, cardholder, RC) and PINs.
    openpgp_factory_reset();
    s_pending_user_id[0] = '\0';
    return true;
}

bool gpg_export_pubkey_pem(char *buf, size_t size, size_t *out_len) {
    if (!buf || size < 256 || !out_len) {
        LOG_W(TAG, "export: bad args");
        return false;
    }
    if (!openpgp_has_any_key() || !gpg_storage_ready()) {
        LOG_W(TAG, "export: no key configured");
        return false;
    }

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) {
        LOG_W(TAG, "export: no SE");
        return false;
    }

    // Buffer sized for the optional 0x04 SEC1 prefix that secure elements
    // can prepend on P-256 reads (so 65 bytes for P-256 vs 32 for Ed25519).
    uint8_t pubkey[P256_PUBKEY_SIZE + 1] = {0};
    cdc::hal::EccCurve eccCurve = cdc::hal::EccCurve::P256;
    uint8_t sig_slot = gpg_storage_sig_slot();
    auto res = se->eccGetPublicKey(sig_slot, pubkey, &eccCurve);
    if (res != cdc::hal::SeResult::OK) {
        LOG_W(TAG, "export: eccGetPublicKey slot=%u failed (%d)",
              sig_slot, static_cast<int>(res));
        return false;
    }
    uint8_t curve = (eccCurve == cdc::hal::EccCurve::ED25519) ? CDC_CURVE_ED25519
                                                                : CDC_CURVE_P256;
    if (curve == CDC_CURVE_P256 && pubkey[0] == 0x04) {
        memmove(pubkey, pubkey + 1, P256_PUBKEY_SIZE);
    }

    if (curve == CDC_CURVE_ED25519) {
        static const uint8_t ed25519_prefix[] = {
            0x30, 0x2a,
            0x30, 0x05,
            0x06, 0x03, 0x2b, 0x65, 0x70,
            0x03, 0x21, 0x00
        };

        uint8_t der[sizeof(ed25519_prefix) + 32];
        memcpy(der, ed25519_prefix, sizeof(ed25519_prefix));
        memcpy(der + sizeof(ed25519_prefix), pubkey, 32);

        size_t b64_len = 0;
        char b64[128];
        if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64), sizeof(b64),
                                   &b64_len, der, sizeof(der)) != 0) {
            return false;
        }

        int written = snprintf(buf, size,
                               "-----BEGIN PUBLIC KEY-----\n%s\n-----END PUBLIC KEY-----\n",
                               b64);
        if (written < 0 || static_cast<size_t>(written) >= size) return false;
        *out_len = static_cast<size_t>(written);
        return true;
    }

    static const uint8_t p256_prefix[] = {
        0x30, 0x59,
        0x30, 0x13,
        0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
        0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07,
        0x03, 0x42, 0x00
    };
    uint8_t der[sizeof(p256_prefix) + 65];
    memcpy(der, p256_prefix, sizeof(p256_prefix));
    der[sizeof(p256_prefix)] = 0x04;
    memcpy(der + sizeof(p256_prefix) + 1, pubkey, 64);

    size_t b64_len = 0;
    char b64[200];
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64), sizeof(b64),
                               &b64_len, der, sizeof(der)) != 0) {
        return false;
    }
    int written = snprintf(buf, size,
                           "-----BEGIN PUBLIC KEY-----\n%s\n-----END PUBLIC KEY-----\n",
                           b64);
    if (written < 0 || static_cast<size_t>(written) >= size) return false;
    *out_len = static_cast<size_t>(written);
    return true;
}

bool gpg_alchemy_fingerprint(char *buf, size_t len) {
    if (!buf || len < KEY_FINGERPRINT_MAX_LEN) {
        return false;
    }
    return key_fingerprint_generate(gpg_storage_sig_slot(), buf, len);
}
