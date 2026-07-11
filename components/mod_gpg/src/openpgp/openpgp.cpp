/**
 * \brief OpenPGP smart-card application implementation for CDC Badge.
 *
 * Based on pico-openpgp (https://github.com/polhenarejos/pico-openpgp),
 * adapted for CDC Badge and TROPIC01 secure element integration.
 * Specification target: OpenPGP Smart Card Application 3.4.1.
 */

#include "mod_gpg/openpgp/openpgp.h"
#include "cdc_scard/apdu.h"
#include "cdc_scard/applet.h"
#include "mod_gpg/openpgp/algo_attr.h"
#include "mod_gpg/openpgp/constants.h"
#include "cdc_log.h"
#include "mod_gpg/gpg.h"
#include "mod_gpg/GpgStorage.h"
#include "ecdh.h"
#include "rsa.h"
#include "mod_gpg/openpgp/kdf.h"
#include "cdc_core/pin_storage_c.h"
#include "cdc_core/PinManager.h"
#include "cdc_hal/ISecureElement.h"
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/bignum.h>
#include <esp_attr.h>
#include <string.h>
#include <time.h>
#include "cdc_log.h"
#include <esp_mac.h>       // For esp_efuse_mac_get_default()
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_random.h>

static const char *TAG = "OpenPGP";

/**
 * \brief Returns secure-element instance used by OpenPGP backend.
 * \return Pointer to secure-element abstraction.
 */
static cdc::hal::ISecureElement* get_se() {
    return cdc::hal::getSecureElementInstance();
}

/**
 * \brief Reads ECC public key from secure element and exposes curve metadata.
 * \param slot ECC slot index.
 * \param pubkey Output public key buffer.
 * \param max_len Capacity of `pubkey`.
 * \param curve_out Optional output curve identifier.
 * \return `true` when key exists and output buffer size matches curve format.
 */
static bool se_ecc_key_read(uint8_t slot, uint8_t* pubkey, size_t max_len, uint8_t* curve_out) {
    auto* se = get_se();
    if (!se || !pubkey) return false;
    cdc::hal::EccCurve curve = cdc::hal::EccCurve::P256;
    auto res = se->eccGetPublicKey(slot, pubkey, &curve);
    if (res != cdc::hal::SeResult::OK) return false;
    if (curve_out) {
        *curve_out = (curve == cdc::hal::EccCurve::ED25519) ? CDC_CURVE_ED25519 : CDC_CURVE_P256;
    }
    if (curve == cdc::hal::EccCurve::ED25519) {
        return max_len >= ED25519_PUBKEY_SIZE;
    }
    return max_len >= P256_PUBKEY_SIZE;
}

/**
 * \brief Generates ECC key material in secure element slot.
 * \param slot ECC slot index.
 * \param curve Curve identifier (`CDC_CURVE_*`).
 * \return `true` if key generation succeeded.
 */
static bool se_ecc_key_generate(uint8_t slot, uint8_t curve) {
    auto* se = get_se();
    if (!se) {
        LOG_E(TAG, "se_ecc_key_generate: SE not available (slot=%u)", slot);
        return false;
    }
    cdc::hal::EccCurve c = (curve == CDC_CURVE_ED25519) ? cdc::hal::EccCurve::ED25519
                                                        : cdc::hal::EccCurve::P256;
    // Per TROPIC01: lt_ecc_key_generate fails with SLOT_OCCUPIED if the slot
    // already holds material. A previous (incomplete) generation, or a
    // GPG_RESET that did not propagate to the SE, leaves the slot used and
    // the next attempt returns SW=6F00 to the host. Pre-wipe defensively.
    cdc::hal::SeResult res = se->eccGenerate(slot, c);
    if (res != cdc::hal::SeResult::OK) {
        LOG_W(TAG, "se_ecc_key_generate: slot %u initial fail (SeResult=%d), deleting and retrying",
              slot, static_cast<int>(res));
        se->eccDelete(slot);
        res = se->eccGenerate(slot, c);
    }
    if (res != cdc::hal::SeResult::OK) {
        LOG_E(TAG, "se_ecc_key_generate(slot=%u curve=%u) failed: SeResult=%d",
                 slot, curve, static_cast<int>(res));
        return false;
    }
    return true;
}

/**
 * \brief Signs a hash using secure-element ECDSA key.
 * \param slot ECC slot index.
 * \param hash Hash bytes to sign.
 * \param hash_len Hash length.
 * \param sig Output 64-byte signature buffer.
 * \return `true` if signing succeeded.
 */
static bool se_ecdsa_sign(uint8_t slot, const uint8_t* hash, size_t hash_len, uint8_t* sig) {
    auto* se = get_se();
    if (!se || !hash || !sig) return false;
    size_t sig_len = 64;
    return se->ecdsaSign(slot, hash, hash_len, sig, &sig_len) == cdc::hal::SeResult::OK;
}

/**
 * \brief Signs a message using secure-element EdDSA key.
 * \param slot ECC slot index.
 * \param msg Message bytes.
 * \param msg_len Message length.
 * \param sig Output signature buffer.
 * \return `true` if signing succeeded.
 */
static bool se_eddsa_sign(uint8_t slot, const uint8_t* msg, size_t msg_len, uint8_t* sig) {
    auto* se = get_se();
    if (!se || !msg || !sig) return false;
    return se->eddsaSign(slot, msg, msg_len, sig) == cdc::hal::SeResult::OK;
}

/**
 * \brief Fills buffer with secure random bytes, with ESP fallback.
 * \param buf Output buffer.
 * \param len Number of bytes to generate.
 */
static void se_random_fill(uint8_t* buf, size_t len) {
    auto* se = get_se();
    if (se && se->getRandom(buf, static_cast<uint16_t>(len))) {
        return;
    }
    esp_fill_random(buf, len);
}

/**
 * \brief OpenPGP Application ID (RID + PIX), initialized dynamically.
 *
 * `D2 76 00 01 24 01` = OpenPGP RID.
 * Structure: RID(6) + Version(2) + Manufacturer(2) + Serial(4) + RFU(2) = 16 bytes.
 */
static uint8_t s_openpgp_aid[16] = {
    0xD2, 0x76, 0x00, 0x01, 0x24, 0x01,  // RID + Application (OpenPGP)
    0x03, 0x04,                           // Version 3.4
    0x00, 0x00,                           // Manufacturer (set in init)
    0x00, 0x00, 0x00, 0x00,              // Serial number (set in init from MAC)
    0x00, 0x00                            // RFU
};
const uint8_t* OPENPGP_AID = s_openpgp_aid;
const uint8_t OPENPGP_AID_LEN = sizeof(s_openpgp_aid);

/**
 * \brief ATR is defined in `ccid.cpp` and accessed via `ccid_get_atr()`.
 */

/**
 * \brief Application session/authentication state.
 */
static bool app_selected = false;
static bool pw1_verified = false;
static bool pw3_verified = false;
static uint32_t sig_count = 0;

/**
 * \brief Resetting Code (RC) — optional per OpenPGP 3.4.1 §4.3.2. When set,
 * the host can unblock PW1 with the RC instead of PW3 (RESET RETRY COUNTER
 * with P1=0x00). We persist the configured RC bytes plus a separate retry
 * counter in NVS.
 *
 * Storage rationale: the existing PW1/PW3 path stores hashes inside the
 * TROPIC01 R-Memory; allocating extra slots there for RC is deferred until
 * the next slot-map revision. Plain NVS bytes are equivalent to plaintext
 * Yubikey behaviour and unlock the same workflow.
 */
#define OPENPGP_RC_MIN_LEN 8
static constexpr size_t RC_SALT_SIZE = 16;
static constexpr size_t RC_HASH_SIZE = 32;
static constexpr size_t RC_KDF_TOTAL_BYTES = 100000;
static uint8_t s_rc_salt[RC_SALT_SIZE] = {0};
static uint8_t s_rc_hash[RC_HASH_SIZE] = {0};
static uint8_t s_rc_len = 0;
static uint8_t s_rc_retries = 3;

/**
 * \brief Iterated-salted SHA-256 over salt||rc for resetting-code storage.
 *        Same construction as PinManager::computeKdfHash (OpenPGP S2K).
 */
static bool compute_rc_hash(const uint8_t* rc, size_t rc_len,
                            const uint8_t* salt, uint8_t* hash_out) {
    if (!rc || !salt || !hash_out || rc_len == 0 || rc_len > OPENPGP_PIN_MAX_LEN) {
        return false;
    }
    uint8_t buffer[RC_SALT_SIZE + OPENPGP_PIN_MAX_LEN];
    memcpy(buffer, salt, RC_SALT_SIZE);
    memcpy(buffer + RC_SALT_SIZE, rc, rc_len);
    const size_t combined = RC_SALT_SIZE + rc_len;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        mbedtls_platform_zeroize(buffer, sizeof(buffer));
        return false;
    }
    size_t processed = 0;
    while (processed < RC_KDF_TOTAL_BYTES) {
        const size_t chunk = (RC_KDF_TOTAL_BYTES - processed < combined)
                                 ? (RC_KDF_TOTAL_BYTES - processed)
                                 : combined;
        if (mbedtls_sha256_update(&ctx, buffer, chunk) != 0) {
            mbedtls_sha256_free(&ctx);
            mbedtls_platform_zeroize(buffer, sizeof(buffer));
            return false;
        }
        processed += chunk;
    }
    mbedtls_sha256_finish(&ctx, hash_out);
    mbedtls_sha256_free(&ctx);
    mbedtls_platform_zeroize(buffer, sizeof(buffer));
    return true;
}

/**
 * \brief Host-selected ECC curve per key role. DEC is fixed to P-256 because
 * the TROPIC01 cannot perform ECDH natively and the firmware only carries a
 * software P-256 ECDH path in `ecdh.cpp`. SIG and AUT default to Ed25519 (the
 * project's preferred curve for signing) and can be flipped to P-256 via
 * PUT DATA C1 / C3 per OpenPGP 3.4.1 §4.4.3.7-9.
 */
static uint8_t selected_curve_sig = CDC_CURVE_ED25519;
static uint8_t selected_curve_aut = CDC_CURVE_ED25519;

/**
 * \brief Per-role algorithm selection beyond the ECC curve. When
 * `role_is_rsa[r]` is set the role is an RSA software key (blob in the
 * `mod_gpg_rsa` R-Memory pool) and the rsa_* parameters carry its algorithm
 * attributes; otherwise the role is ECC and `selected_curve_*` (SIG/AUT) or the
 * fixed DEC P-256 ECDH apply. Index order matches `key_type_t`: SIG=0, DEC=1,
 * AUT=2.
 */
static bool     role_is_rsa[3]     = {false, false, false};
static uint16_t role_rsa_n_bits[3] = {0, 0, 0};
static uint16_t role_rsa_e_bits[3] = {0, 0, 0};
static uint8_t  role_rsa_fmt[3]    = {0, 0, 0};

/**
 * \brief KDF-DO (tag 0xF9) state. When `kdf_active` the host pre-hashes the
 * PINs (PBKDF2) before VERIFY / CHANGE REFERENCE DATA / RESET RETRY COUNTER;
 * PW1/PW3 are then compared through the binary PinManager path. The raw DO
 * bytes are echoed back on GET DATA 0xF9.
 */
static bool    kdf_active = false;
static uint8_t kdf_pin_len = 0;          // 32 (SHA-256) or 64 (SHA-512)
static uint8_t kdf_do_bytes[124] = {};
static uint8_t kdf_do_len = 0;

/**
 * \brief Card lifecycle state per OpenPGP 3.4.1 §7.2.18.
 *
 * `false` = operational, `true` = terminated. While terminated the dispatcher
 * accepts only SELECT and ACTIVATE FILE; everything else returns
 * SW_FILE_TERMINATED (0x6285). ACTIVATE FILE wipes keys, DOs and PINs back to
 * factory defaults.
 */
static bool card_terminated = false;

/**
 * \brief Buffered remainder of an APDU response that did not fit into the
 * caller-supplied Le window. Drained one chunk at a time via GET RESPONSE
 * (INS 0xC0) per ISO 7816-4 §5.3.4. Lifetime is bound to the next APDU on
 * the same logical channel: the buffer is invalidated when any non-GET
 * RESPONSE command arrives.
 *
 * 4 kB cap covers the largest OpenPGP DOs (Cardholder Certificate, Application
 * Related Data with full key info) without claiming PSRAM.
 */
EXT_RAM_BSS_ATTR static uint8_t g_resp_buffer[4096];
static size_t   g_resp_remaining = 0;
static size_t   g_resp_pos = 0;

/**
 * \brief Command-chaining accumulator (ISO 7816-4 §5.1.1).
 *
 * The host sets the CLA chaining bit (0x10) on every intermediate APDU and
 * clears it on the last one. We accumulate the command data here until the
 * final chunk arrives, then dispatch a single synthetic APDU. Triggered by
 * gpg key import (RSA / large ECC), large PUT DATA (e.g. cardholder cert),
 * and PSO:DECIPHER with extended Cipher DOs.
 */
EXT_RAM_BSS_ATTR static uint8_t g_chain_buffer[4096];
static size_t   g_chain_len = 0;
static bool     g_chain_active = false;
static uint8_t  g_chain_ins = 0;
static uint8_t  g_chain_p1 = 0;
static uint8_t  g_chain_p2 = 0;

static void chain_reset(void) {
    g_chain_len = 0;
    g_chain_active = false;
    g_chain_ins = 0;
    g_chain_p1 = 0;
    g_chain_p2 = 0;
}

/**
 * \brief Session PIN cache for DEC key decryption (temporary after VERIFY for PSO:DECIPHER).
 */
static char s_session_pin[OPENPGP_PIN_MAX_LEN + 1] = {};

/**
 * \brief Wipes the cached session PIN and the storage session state.
 */
static void session_wipe(void) {
    mbedtls_platform_zeroize(s_session_pin, sizeof(s_session_pin));
    gpg_storage_clear_session();
}

/**
 * \brief NVS namespace used for OpenPGP persistent data.
 */
#define NVS_NAMESPACE "openpgp"
#define NVS_STATE_KEY "state"

/**
 * \brief Single-blob persistent OpenPGP runtime state.
 *
 * One nvs_set_blob per save keeps NVS page fragmentation bounded: the
 * default 20 KB NVS partition is shared with every other module and the
 * old per-field layout (~21 entries) silently filled up under load.
 */
struct __attribute__((packed)) OpenpgpNvsState {
    uint8_t  schema_version;
    uint8_t  card_terminated;
    uint8_t  selected_curve_sig;
    uint8_t  selected_curve_aut;
    uint8_t  rc_len;
    uint8_t  rc_retries;
    uint8_t  rc_salt[RC_SALT_SIZE];
    uint8_t  rc_hash[RC_HASH_SIZE];
    uint32_t sig_count;
    uint8_t  fingerprint_sig[OPENPGP_FINGERPRINT_SIZE];
    uint8_t  fingerprint_dec[OPENPGP_FINGERPRINT_SIZE];
    uint8_t  fingerprint_aut[OPENPGP_FINGERPRINT_SIZE];
    uint8_t  ca_fp_1[OPENPGP_FINGERPRINT_SIZE];
    uint8_t  ca_fp_2[OPENPGP_FINGERPRINT_SIZE];
    uint8_t  ca_fp_3[OPENPGP_FINGERPRINT_SIZE];
    uint8_t  gen_time_sig[4];
    uint8_t  gen_time_dec[4];
    uint8_t  gen_time_aut[4];
    uint8_t  cardholder_sex;
    char     cardholder_name[40];
    char     cardholder_lang[8];
    char     cardholder_url[64];
    char     cardholder_login[32];
    uint8_t  role_is_rsa[3];
    uint16_t role_rsa_n_bits[3];
    uint16_t role_rsa_e_bits[3];
    uint8_t  role_rsa_fmt[3];
    uint8_t  kdf_active;
    uint8_t  kdf_pin_len;
    uint8_t  kdf_do_len;
    uint8_t  kdf_do_bytes[124];
};

static constexpr uint8_t OPENPGP_NVS_SCHEMA_V3 = 3;

/**
 * \brief Data object storage buffers (fingerprints and related metadata).
 */
static uint8_t fingerprint_sig[OPENPGP_FINGERPRINT_SIZE] = {0};
static uint8_t fingerprint_dec[OPENPGP_FINGERPRINT_SIZE] = {0};
static uint8_t fingerprint_aut[OPENPGP_FINGERPRINT_SIZE] = {0};

/**
 * \brief Key-generation timestamps (4-byte big-endian Unix time each).
 */
static uint8_t gen_time_sig[4] = {0};
static uint8_t gen_time_dec[4] = {0};
static uint8_t gen_time_aut[4] = {0};

/**
 * \brief Optional CA fingerprints for trust-chain metadata.
 */
static uint8_t ca_fp_1[OPENPGP_FINGERPRINT_SIZE] = {0};
static uint8_t ca_fp_2[OPENPGP_FINGERPRINT_SIZE] = {0};
static uint8_t ca_fp_3[OPENPGP_FINGERPRINT_SIZE] = {0};

/**
 * \brief Cardholder profile data stored in NVS.
 */
static char cardholder_name[40] = {0};    // "Surname<<Firstname"
static char cardholder_lang[8] = "en";     // ISO 639-1 language
static uint8_t cardholder_sex = 0x39;      // '9' = not specified
static char cardholder_url[64] = {0};     // URL for public key retrieval
static char cardholder_login[32] = {0};   // Login data

/**
 * \brief Historical bytes used in OpenPGP ATR-related data objects.
 */
static const uint8_t HIST_BYTES[] = {
    0x00,       // Category indicator: card has no indication of services
    0x31,       // Card capabilities (card can process T=1)
    0xC5,       // Tag: card issuer data follows
    0x73, 0xC0, 0x01, 0x80,  // Card issuer proprietary
    0x05,       // Tag: card capabilities
    0x90, 0x00  // Card status: OK
};

/**
 * \brief Algorithm attributes for Ed25519 (EdDSA with curve25519).
 *
 * Format: Algorithm ID (1) + OID bytes (no length prefix per OpenPGP 3.4.1).
 */
static const uint8_t ALGO_ATTR_ED25519[] = {
    ALGO_EDDSA,                                         // Algorithm: EdDSA (0x16)
    0x2B, 0x06, 0x01, 0x04, 0x01, 0xDA, 0x47, 0x0F, 0x01  // OID 1.3.6.1.4.1.11591.15.1 (ed25519)
};

/**
 * \brief Algorithm attributes for P-256 ECDSA (signature/authentication roles).
 *
 * Format: Algorithm ID (1) + OID bytes (no length prefix per OpenPGP 3.4.1).
 */
static const uint8_t ALGO_ATTR_P256_ECDSA[] = {
    ALGO_ECDSA,                                         // Algorithm: ECDSA (0x13)
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07      // OID 1.2.840.10045.3.1.7 (secp256r1)
};

/**
 * \brief Algorithm attributes for P-256 ECDH (decryption role).
 *
 * Format: Algorithm ID (1) + OID bytes (no length prefix per OpenPGP 3.4.1).
 */
static const uint8_t ALGO_ATTR_P256_ECDH[] = {
    ALGO_ECDH,                                          // Algorithm: ECDH (0x12)
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07      // OID 1.2.840.10045.3.1.7 (secp256r1)
};

/**
 * \brief Extended capabilities object per OpenPGP 3.4.1 section 4.2.1.
 */
static const uint8_t EXT_CAPABILITIES[] = {
    0x7D,
    0x00,       // SM Algorithm: none
    0x00, 0x80, // Max GET CHALLENGE length: 128 bytes
    0x08, 0x00, // Max Cardholder Certificate length: 2048 bytes
    0x01, 0x00, // Max special DO length: 256 bytes
    0x00,       // PIN block 2 format not supported
    0x00,       // MSE for key selection not supported
};

/**
 * \brief TLV builder helper functions.
 */

/**
 * \brief Writes a TLV tag using one or two bytes.
 * \param buf Output buffer receiving the tag bytes.
 * \param tag TLV tag value.
 * \return Number of bytes written to `buf`.
 */
static size_t tlv_write_tag(uint8_t *buf, uint16_t tag) {
    if (tag > 0xFF) {
        buf[0] = (tag >> 8) & 0xFF;
        buf[1] = tag & 0xFF;
        return 2;
    }
    buf[0] = tag & 0xFF;
    return 1;
}

/**
 * \brief Writes a TLV length field using DER length encoding.
 * \param buf Output buffer receiving the encoded length.
 * \param len Length value to encode.
 * \return Number of bytes written to `buf`.
 */
static size_t tlv_write_len(uint8_t *buf, size_t len) {
    if (len < 128) {
        buf[0] = len;
        return 1;
    } else if (len < 256) {
        buf[0] = 0x81;
        buf[1] = len;
        return 2;
    } else {
        buf[0] = 0x82;
        buf[1] = (len >> 8) & 0xFF;
        buf[2] = len & 0xFF;
        return 3;
    }
}

/**
 * \brief Builds complete TLV object and returns total encoded length.
 * \param buf Output buffer.
 * \param buf_max Maximum size of `buf`.
 * \param tag TLV tag.
 * \param value Optional value bytes.
 * \param value_len Value length.
 * \return Total bytes written to `buf`.
 */
static size_t tlv_build(uint8_t *buf, size_t buf_max, uint16_t tag,
                        const uint8_t *value, size_t value_len) {
    size_t pos = 0;
    pos += tlv_write_tag(buf + pos, tag);
    pos += tlv_write_len(buf + pos, value_len);
    if (value && value_len > 0) {
        memcpy(buf + pos, value, value_len);
        pos += value_len;
    }
    return pos;
}

/**
 * \brief Builders for OpenPGP application-related data objects.
 */

/**
 * \brief Key role discriminator used for algorithm-attribute selection.
 */
typedef enum {
    KEY_TYPE_SIG = 0,  // Signature (ECDSA/EdDSA)
    KEY_TYPE_DEC = 1,  // Decryption (ECDH)
    KEY_TYPE_AUT = 2   // Authentication (ECDSA/EdDSA)
} key_type_t;

/**
 * \brief Returns algorithm attributes for a key role based on stored key type.
 * \param key_type Key role (signature, decryption, authentication).
 * \param len Output pointer receiving the attribute length.
 * \return Pointer to the selected algorithm-attribute byte array.
 */
static const uint8_t* get_algo_attr(key_type_t key_type, size_t *len) {
    const int r = static_cast<int>(key_type);  // SIG=0, DEC=1, AUT=2
    // RSA roles advertise the RSA attribute: 01 || N-bits || E-bits || import-fmt.
    if (role_is_rsa[r]) {
        static uint8_t s_rsa_attr[6];
        const uint16_t eb = role_rsa_e_bits[r] ? role_rsa_e_bits[r] : 32;
        s_rsa_attr[0] = ALGO_RSA;
        s_rsa_attr[1] = static_cast<uint8_t>((role_rsa_n_bits[r] >> 8) & 0xFF);
        s_rsa_attr[2] = static_cast<uint8_t>(role_rsa_n_bits[r] & 0xFF);
        s_rsa_attr[3] = static_cast<uint8_t>((eb >> 8) & 0xFF);
        s_rsa_attr[4] = static_cast<uint8_t>(eb & 0xFF);
        s_rsa_attr[5] = role_rsa_fmt[r];
        *len = sizeof(s_rsa_attr);
        return s_rsa_attr;
    }
    // ECC: DEC is P-256 ECDH (software path); SIG / AUT follow the configured
    // curve which PUT DATA C1 / C3 may override.
    if (key_type == KEY_TYPE_DEC) {
        *len = sizeof(ALGO_ATTR_P256_ECDH);
        return ALGO_ATTR_P256_ECDH;
    }
    const uint8_t curve = (key_type == KEY_TYPE_AUT) ? selected_curve_aut
                                                     : selected_curve_sig;
    if (curve == CDC_CURVE_P256) {
        *len = sizeof(ALGO_ATTR_P256_ECDSA);
        return ALGO_ATTR_P256_ECDSA;
    }
    *len = sizeof(ALGO_ATTR_ED25519);
    return ALGO_ATTR_ED25519;
}

/**
 * \brief Builds OpenPGP DO `0x6E` (Application Related Data).
 * \param buf Output buffer for the encoded TLV object.
 * \param buf_max Maximum size of `buf`.
 * \return Encoded length on success, or a negative error code.
 */
static int build_do_app_related(uint8_t *buf, size_t buf_max) {
    uint8_t inner[512];
    size_t inner_len = 0;

    // 4F: AID
    inner_len += tlv_build(inner + inner_len, sizeof(inner) - inner_len,
                           DO_AID, OPENPGP_AID, OPENPGP_AID_LEN);

    // 5F52: Historical bytes
    inner_len += tlv_build(inner + inner_len, sizeof(inner) - inner_len,
                           DO_HIST_BYTES, HIST_BYTES, sizeof(HIST_BYTES));

    // 73: Discretionary data objects (nested)
    uint8_t discret[384];
    size_t discret_len = 0;

    // C0: Extended capabilities
    discret_len += tlv_build(discret + discret_len, sizeof(discret) - discret_len,
                             DO_EXT_CAP, EXT_CAPABILITIES, sizeof(EXT_CAPABILITIES));

    // C1: Algorithm attributes - Signature (ECDSA/EdDSA)
    size_t algo_len;
    const uint8_t *algo = get_algo_attr(KEY_TYPE_SIG, &algo_len);
    discret_len += tlv_build(discret + discret_len, sizeof(discret) - discret_len,
                             DO_ALGO_SIG, algo, algo_len);

    // C2: Algorithm attributes - Decryption (ECDH)
    algo = get_algo_attr(KEY_TYPE_DEC, &algo_len);
    discret_len += tlv_build(discret + discret_len, sizeof(discret) - discret_len,
                             DO_ALGO_DEC, algo, algo_len);

    // C3: Algorithm attributes - Authentication (ECDSA/EdDSA)
    algo = get_algo_attr(KEY_TYPE_AUT, &algo_len);
    discret_len += tlv_build(discret + discret_len, sizeof(discret) - discret_len,
                             DO_ALGO_AUT, algo, algo_len);

    // C4: PW Status Bytes (retries from TROPIC01 storage)
    // Note: Max lengths limited for practical use on hardware keypad
    uint8_t pw_status[7] = {
        0x01,                       // PW1 valid for multiple signatures
        OPENPGP_PIN_MAX_LEN,        // Max length PW1 (practical limit)
        OPENPGP_PIN_MAX_LEN,        // Max length RC
        OPENPGP_PIN_MAX_LEN,        // Max length PW3
        pin_storage_openpgp_pw1_retries(),
        s_rc_len > 0 ? s_rc_retries : static_cast<uint8_t>(0),
        pin_storage_openpgp_pw3_retries()
    };
    discret_len += tlv_build(discret + discret_len, sizeof(discret) - discret_len,
                             DO_PW_STATUS, pw_status, sizeof(pw_status));

    // C5: Fingerprints (3 * 20 bytes: SIG + DEC + AUT)
    uint8_t fps[3 * OPENPGP_FINGERPRINT_SIZE];
    memcpy(fps + 0 * OPENPGP_FINGERPRINT_SIZE, fingerprint_sig, OPENPGP_FINGERPRINT_SIZE);
    memcpy(fps + 1 * OPENPGP_FINGERPRINT_SIZE, fingerprint_dec, OPENPGP_FINGERPRINT_SIZE);
    memcpy(fps + 2 * OPENPGP_FINGERPRINT_SIZE, fingerprint_aut, OPENPGP_FINGERPRINT_SIZE);
    discret_len += tlv_build(discret + discret_len, sizeof(discret) - discret_len,
                             0xC5, fps, sizeof(fps));  // 0xC5 = combined fingerprints (no separate constant)

    // C6: CA Fingerprints (3 * 20 bytes)
    uint8_t ca_fps[3 * OPENPGP_FINGERPRINT_SIZE];
    memcpy(ca_fps + 0 * OPENPGP_FINGERPRINT_SIZE, ca_fp_1, OPENPGP_FINGERPRINT_SIZE);
    memcpy(ca_fps + 1 * OPENPGP_FINGERPRINT_SIZE, ca_fp_2, OPENPGP_FINGERPRINT_SIZE);
    memcpy(ca_fps + 2 * OPENPGP_FINGERPRINT_SIZE, ca_fp_3, OPENPGP_FINGERPRINT_SIZE);
    discret_len += tlv_build(discret + discret_len, sizeof(discret) - discret_len,
                             0xC6, ca_fps, sizeof(ca_fps));

    // CD: Generation dates (12 bytes: SIG + DEC + AUT)
    uint8_t gen_times[12];
    memcpy(gen_times, gen_time_sig, 4);
    memcpy(gen_times + 4, gen_time_dec, 4);
    memcpy(gen_times + 8, gen_time_aut, 4);
    discret_len += tlv_build(discret + discret_len, sizeof(discret) - discret_len,
                             0xCD, gen_times, 12);

    // Add discretionary DOs to inner
    inner_len += tlv_build(inner + inner_len, sizeof(inner) - inner_len,
                           0x73, discret, discret_len);

    // Build final 6E response
    size_t total = 0;
    total += tlv_write_tag(buf + total, 0x6E);
    total += tlv_write_len(buf + total, inner_len);
    memcpy(buf + total, inner, inner_len);
    total += inner_len;

    return total;
}

/**
 * \brief Builds OpenPGP DO `0x65` (Cardholder Related Data).
 * \param buf Output buffer for the encoded TLV object.
 * \param buf_max Maximum size of `buf`.
 * \return Encoded length on success, or a negative error code.
 */
static int build_do_cardholder(uint8_t *buf, size_t buf_max) {
    uint8_t inner[128];
    size_t inner_len = 0;

    // 5B: Name
    size_t name_len = strlen(cardholder_name);
    inner_len += tlv_build(inner + inner_len, sizeof(inner) - inner_len,
                           DO_NAME, (const uint8_t *)cardholder_name, name_len);

    // 5F2D: Language preference
    size_t lang_len = strlen(cardholder_lang);
    inner_len += tlv_build(inner + inner_len, sizeof(inner) - inner_len,
                           DO_LANG_PREF, (const uint8_t *)cardholder_lang, lang_len);

    // 5F35: Sex
    inner_len += tlv_build(inner + inner_len, sizeof(inner) - inner_len,
                           DO_SEX, &cardholder_sex, 1);

    // Build final 65 response
    size_t total = 0;
    total += tlv_write_tag(buf + total, DO_CARDHOLDER);
    total += tlv_write_len(buf + total, inner_len);
    memcpy(buf + total, inner, inner_len);
    total += inner_len;

    return total;
}

static constexpr uint8_t ATTESTATION_ECC_SLOT = 0;
static constexpr size_t OPENPGP_STATE_SIG_SIZE = 64;

/**
 * \brief Verifies the P-256 ECDSA attestation signature over an OpenPGP state
 *        payload. Same construction as PinManager.
 */
static bool verify_state_signature(cdc::hal::ISecureElement* se,
                                   const uint8_t* payload, size_t payload_len,
                                   const uint8_t* sig, size_t sig_len) {
    if (!se || sig_len != OPENPGP_STATE_SIG_SIZE) return false;
    uint8_t pub_raw[64];
    cdc::hal::EccCurve curve = cdc::hal::EccCurve::P256;
    if (se->eccGetPublicKey(ATTESTATION_ECC_SLOT, pub_raw, &curve) != cdc::hal::SeResult::OK) {
        return false;
    }
    if (curve != cdc::hal::EccCurve::P256) return false;

    uint8_t pub_sec1[65];
    pub_sec1[0] = 0x04;
    memcpy(pub_sec1 + 1, pub_raw, 64);

    uint8_t hash[32];
    mbedtls_sha256(payload, payload_len, hash, 0);

    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
        if (mbedtls_ecp_point_read_binary(&grp, &Q, pub_sec1, sizeof(pub_sec1)) != 0) break;
        if (mbedtls_mpi_read_binary(&r, sig + 0, 32) != 0) break;
        if (mbedtls_mpi_read_binary(&s, sig + 32, 32) != 0) break;
        ok = (mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &Q, &r, &s) == 0);
    } while (0);

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

/**
 * \brief Loads persistent OpenPGP runtime state from NVS.
 */
static void load_state_from_nvs(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    constexpr size_t BLOB_SIZE = sizeof(OpenpgpNvsState) + OPENPGP_STATE_SIG_SIZE;
    uint8_t blob[BLOB_SIZE];
    size_t len = BLOB_SIZE;
    esp_err_t err = nvs_get_blob(nvs, NVS_STATE_KEY, blob, &len);
    nvs_close(nvs);

    if (err != ESP_OK || len != BLOB_SIZE) {
        return;
    }

    OpenpgpNvsState state = {};
    memcpy(&state, blob, sizeof(state));
    if (state.schema_version != OPENPGP_NVS_SCHEMA_V3) {
        return;
    }
    if (!verify_state_signature(get_se(), blob, sizeof(state),
                                blob + sizeof(state), OPENPGP_STATE_SIG_SIZE)) {
        LOG_W(TAG, "OpenPGP state signature invalid - re-initialising");
        return;
    }

    card_terminated = state.card_terminated != 0;
    if (state.selected_curve_sig == CDC_CURVE_P256 ||
        state.selected_curve_sig == CDC_CURVE_ED25519) {
        selected_curve_sig = state.selected_curve_sig;
    }
    if (state.selected_curve_aut == CDC_CURVE_P256 ||
        state.selected_curve_aut == CDC_CURVE_ED25519) {
        selected_curve_aut = state.selected_curve_aut;
    }
    if (state.rc_len > 0 && state.rc_len <= OPENPGP_PIN_MAX_LEN) {
        memcpy(s_rc_salt, state.rc_salt, RC_SALT_SIZE);
        memcpy(s_rc_hash, state.rc_hash, RC_HASH_SIZE);
        s_rc_len = state.rc_len;
    }
    s_rc_retries = state.rc_retries;
    sig_count = state.sig_count;
    memcpy(fingerprint_sig, state.fingerprint_sig, OPENPGP_FINGERPRINT_SIZE);
    memcpy(fingerprint_dec, state.fingerprint_dec, OPENPGP_FINGERPRINT_SIZE);
    memcpy(fingerprint_aut, state.fingerprint_aut, OPENPGP_FINGERPRINT_SIZE);
    memcpy(ca_fp_1, state.ca_fp_1, OPENPGP_FINGERPRINT_SIZE);
    memcpy(ca_fp_2, state.ca_fp_2, OPENPGP_FINGERPRINT_SIZE);
    memcpy(ca_fp_3, state.ca_fp_3, OPENPGP_FINGERPRINT_SIZE);
    memcpy(gen_time_sig, state.gen_time_sig, 4);
    memcpy(gen_time_dec, state.gen_time_dec, 4);
    memcpy(gen_time_aut, state.gen_time_aut, 4);
    cardholder_sex = state.cardholder_sex;
    memcpy(cardholder_name,  state.cardholder_name,  sizeof(cardholder_name));
    memcpy(cardholder_lang,  state.cardholder_lang,  sizeof(cardholder_lang));
    memcpy(cardholder_url,   state.cardholder_url,   sizeof(cardholder_url));
    memcpy(cardholder_login, state.cardholder_login, sizeof(cardholder_login));
    cardholder_name[sizeof(cardholder_name) - 1]   = '\0';
    cardholder_lang[sizeof(cardholder_lang) - 1]   = '\0';
    cardholder_url[sizeof(cardholder_url) - 1]     = '\0';
    cardholder_login[sizeof(cardholder_login) - 1] = '\0';

    for (int r = 0; r < 3; ++r) {
        role_is_rsa[r]     = state.role_is_rsa[r] != 0;
        role_rsa_n_bits[r] = state.role_rsa_n_bits[r];
        role_rsa_e_bits[r] = state.role_rsa_e_bits[r];
        role_rsa_fmt[r]    = state.role_rsa_fmt[r];
    }
    kdf_active  = state.kdf_active != 0;
    kdf_pin_len = state.kdf_pin_len;
    kdf_do_len  = (state.kdf_do_len <= sizeof(kdf_do_bytes)) ? state.kdf_do_len : 0;
    memcpy(kdf_do_bytes, state.kdf_do_bytes, sizeof(kdf_do_bytes));
}

/**
 * \brief Persists OpenPGP runtime state to NVS.
 * \return void
 */
static void save_state_to_nvs(void) {
    OpenpgpNvsState state = {};
    state.schema_version     = OPENPGP_NVS_SCHEMA_V3;
    state.card_terminated    = card_terminated ? 1 : 0;
    state.selected_curve_sig = selected_curve_sig;
    state.selected_curve_aut = selected_curve_aut;
    state.rc_len             = s_rc_len;
    state.rc_retries         = s_rc_retries;
    if (s_rc_len > 0) {
        memcpy(state.rc_salt, s_rc_salt, RC_SALT_SIZE);
        memcpy(state.rc_hash, s_rc_hash, RC_HASH_SIZE);
    }
    state.sig_count = sig_count;
    memcpy(state.fingerprint_sig, fingerprint_sig, OPENPGP_FINGERPRINT_SIZE);
    memcpy(state.fingerprint_dec, fingerprint_dec, OPENPGP_FINGERPRINT_SIZE);
    memcpy(state.fingerprint_aut, fingerprint_aut, OPENPGP_FINGERPRINT_SIZE);
    memcpy(state.ca_fp_1, ca_fp_1, OPENPGP_FINGERPRINT_SIZE);
    memcpy(state.ca_fp_2, ca_fp_2, OPENPGP_FINGERPRINT_SIZE);
    memcpy(state.ca_fp_3, ca_fp_3, OPENPGP_FINGERPRINT_SIZE);
    memcpy(state.gen_time_sig, gen_time_sig, 4);
    memcpy(state.gen_time_dec, gen_time_dec, 4);
    memcpy(state.gen_time_aut, gen_time_aut, 4);
    state.cardholder_sex = cardholder_sex;
    memcpy(state.cardholder_name,  cardholder_name,  sizeof(state.cardholder_name));
    memcpy(state.cardholder_lang,  cardholder_lang,  sizeof(state.cardholder_lang));
    memcpy(state.cardholder_url,   cardholder_url,   sizeof(state.cardholder_url));
    memcpy(state.cardholder_login, cardholder_login, sizeof(state.cardholder_login));
    for (int r = 0; r < 3; ++r) {
        state.role_is_rsa[r]     = role_is_rsa[r] ? 1 : 0;
        state.role_rsa_n_bits[r] = role_rsa_n_bits[r];
        state.role_rsa_e_bits[r] = role_rsa_e_bits[r];
        state.role_rsa_fmt[r]    = role_rsa_fmt[r];
    }
    state.kdf_active  = kdf_active ? 1 : 0;
    state.kdf_pin_len = kdf_pin_len;
    state.kdf_do_len  = kdf_do_len;
    memcpy(state.kdf_do_bytes, kdf_do_bytes, sizeof(state.kdf_do_bytes));

    constexpr size_t BLOB_SIZE = sizeof(OpenpgpNvsState) + OPENPGP_STATE_SIG_SIZE;
    uint8_t blob[BLOB_SIZE];
    memcpy(blob, &state, sizeof(state));

    auto* se = get_se();
    if (!se) {
        LOG_E(TAG, "save_state: no SE");
        return;
    }
    size_t sig_len = OPENPGP_STATE_SIG_SIZE;
    cdc::hal::SeResult sign_res = se->ecdsaSign(ATTESTATION_ECC_SLOT,
                                                blob, sizeof(state),
                                                blob + sizeof(state), &sig_len);
    if (sign_res != cdc::hal::SeResult::OK || sig_len != OPENPGP_STATE_SIG_SIZE) {
        LOG_E(TAG, "save_state: attestation sign failed (%d)",
              static_cast<int>(sign_res));
        return;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        LOG_E(TAG, "save_state: nvs_open %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(nvs, NVS_STATE_KEY, blob, BLOB_SIZE);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        LOG_E(TAG, "save_state: %s", esp_err_to_name(err));
    }
}

/**
 * \brief NVS key holding the (public) cardholder certificate (DO 0x7F21).
 */
#define NVS_CERT_KEY "cardcert"
static constexpr size_t CARDHOLDER_CERT_MAX = 2048;

/**
 * \brief Persists the cardholder certificate as a standalone NVS blob.
 *        `len == 0` erases it. Stored unsigned: a certificate is public data.
 */
static bool save_cardholder_cert(const uint8_t* data, size_t len) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err;
    if (len == 0) {
        err = nvs_erase_key(nvs, NVS_CERT_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_blob(nvs, NVS_CERT_KEY, data, len);
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

/**
 * \brief Loads the cardholder certificate into `out`.
 * \return Number of bytes read, or 0 when absent / too large for `cap`.
 */
static size_t load_cardholder_cert(uint8_t* out, size_t cap) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return 0;
    size_t len = cap;
    esp_err_t err = nvs_get_blob(nvs, NVS_CERT_KEY, out, &len);
    nvs_close(nvs);
    return (err == ESP_OK) ? len : 0;
}

/**
 * \brief Returns the on-device byte length of an SHA-256/512 KDF pre-hash.
 */
static uint8_t kdf_hash_len(kdf_hash_t hash) {
    return (hash == KDF_HASH_SHA512) ? 64 : 32;
}

/**
 * \brief Applies a KDF-DO payload written via PUT DATA 0xF9.
 *
 * On enable, PW1/PW3 references are initialised from the host-supplied initial
 * hashes (tags 0x87 / 0x88). On disable, the PINs are reset to the cleartext
 * defaults so the regular VERIFY path keeps working.
 * \return An OpenPGP status word.
 */
static uint16_t apply_kdf_do(const uint8_t* data, size_t len) {
    kdf_do_t parsed;
    if (kdf_do_parse(data, len, &parsed) != KDF_OK) {
        return SW_WRONG_DATA;
    }
    if (len > sizeof(kdf_do_bytes)) {
        return SW_WRONG_LENGTH;
    }

    if (parsed.algo == KDF_ALGO_NONE) {
        kdf_active = false;
        kdf_pin_len = 0;
        kdf_do_len = static_cast<uint8_t>(len);
        memcpy(kdf_do_bytes, data, len);
        pin_storage_openpgp_change_pw1(cdc::core::PinManager::DEFAULT_PW1);
        pin_storage_openpgp_change_pw3(cdc::core::PinManager::DEFAULT_PW3);
        save_state_to_nvs();
        LOG_I(TAG, "KDF disabled, PINs reset to defaults");
        return SW_OK;
    }

    if (parsed.hash != KDF_HASH_SHA256 && parsed.hash != KDF_HASH_SHA512) {
        return SW_WRONG_DATA;
    }
    kdf_active = true;
    kdf_pin_len = kdf_hash_len(parsed.hash);
    kdf_do_len = static_cast<uint8_t>(len);
    memcpy(kdf_do_bytes, data, len);
    if (parsed.has_pw1_initial) {
        pin_storage_openpgp_set_pw1_raw(parsed.pw1_initial, parsed.pw1_initial_len);
    }
    if (parsed.has_pw3_initial) {
        pin_storage_openpgp_set_pw3_raw(parsed.pw3_initial, parsed.pw3_initial_len);
    }
    save_state_to_nvs();
    LOG_I(TAG, "KDF enabled (hash len %u)", kdf_pin_len);
    return SW_OK;
}

/**
 * \brief Maps an OpenPGP key reference (B6/B8/A4) to a 0-based role index.
 * \return 0 = SIG, 1 = DEC, 2 = AUT, or -1 if unknown.
 */
static int role_index_for_key_ref(uint8_t key_ref) {
    switch (key_ref) {
        case KEY_SIG: return 0;
        case KEY_DEC: return 1;
        case KEY_AUT: return 2;
        default:      return -1;
    }
}

/**
 * \brief Initializes the OpenPGP AID serial section from the ESP32 MAC address.
 * \return void
 */
static void init_aid_from_mac(void) {
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        // Use last 4 bytes of MAC as serial number (big-endian)
        // MAC format: [0][1][2][3][4][5] - use [2][3][4][5] for better uniqueness
        s_openpgp_aid[10] = mac[2];
        s_openpgp_aid[11] = mac[3];
        s_openpgp_aid[12] = mac[4];
        s_openpgp_aid[13] = mac[5];

        // Set manufacturer: CDC Badge = 0x4344 ("CD" in ASCII)
        s_openpgp_aid[8] = 0x43;   // 'C'
        s_openpgp_aid[9] = 0x44;   // 'D'

        LOG_I(TAG, "AID initialized: Manufacturer=0x%02X%02X Serial=%02X%02X%02X%02X",
                 s_openpgp_aid[8], s_openpgp_aid[9],
                 s_openpgp_aid[10], s_openpgp_aid[11],
                 s_openpgp_aid[12], s_openpgp_aid[13]);
    } else {
        LOG_W(TAG, "Failed to read MAC, using default AID");
        // Keep defaults: FFFE / 00000001
        s_openpgp_aid[8] = 0xFF;
        s_openpgp_aid[9] = 0xFE;
        s_openpgp_aid[10] = 0x00;
        s_openpgp_aid[11] = 0x00;
        s_openpgp_aid[12] = 0x00;
        s_openpgp_aid[13] = 0x01;
    }
}

bool openpgp_init(void) {
    // Initialize AID with device-unique serial number
    init_aid_from_mac();

    // Initialize GPG component (TROPIC01 backend)
    if (!gpg_init()) {
        LOG_E(TAG, "Failed to initialize GPG/TROPIC01");
        return false;
    }

    // Initialize OpenPGP PIN storage (loads PINs from TROPIC01)
    pin_storage_openpgp_init();

    load_state_from_nvs();

    LOG_I(TAG, "OpenPGP application initialized, sig_count=%lu", sig_count);
    return true;
}

bool openpgp_is_selected(void) {
    return app_selected;
}

uint32_t openpgp_get_sig_count(void) {
    return sig_count;
}

bool openpgp_get_fingerprint(uint8_t key_type, uint8_t *fp_out) {
    if (!fp_out) return false;
    switch (key_type) {
        case KEY_SIG: memcpy(fp_out, fingerprint_sig, OPENPGP_FINGERPRINT_SIZE); return true;
        case KEY_DEC: memcpy(fp_out, fingerprint_dec, OPENPGP_FINGERPRINT_SIZE); return true;
        case KEY_AUT: memcpy(fp_out, fingerprint_aut, OPENPGP_FINGERPRINT_SIZE); return true;
        default: return false;
    }
}

static bool fp_is_set(const uint8_t fp[OPENPGP_FINGERPRINT_SIZE]) {
    for (size_t i = 0; i < OPENPGP_FINGERPRINT_SIZE; i++) {
        if (fp[i] != 0) return true;
    }
    return false;
}

bool openpgp_has_any_key(void) {
    return fp_is_set(fingerprint_sig) ||
           fp_is_set(fingerprint_dec) ||
           fp_is_set(fingerprint_aut);
}

size_t openpgp_get_cardholder_name(char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    size_t len = strlen(cardholder_name);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, cardholder_name, len);
    out[len] = '\0';
    return len;
}

uint32_t openpgp_get_gen_time(uint8_t key_type) {
    const uint8_t *src = nullptr;
    switch (key_type) {
        case KEY_SIG: src = gen_time_sig; break;
        case KEY_DEC: src = gen_time_dec; break;
        case KEY_AUT: src = gen_time_aut; break;
        default: return 0;
    }
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8)  |
            static_cast<uint32_t>(src[3]);
}

bool openpgp_set_cardholder_name(const char *name) {
    if (!name) return false;
    size_t len = strlen(name);
    if (len >= sizeof(cardholder_name)) len = sizeof(cardholder_name) - 1;
    memcpy(cardholder_name, name, len);
    cardholder_name[len] = '\0';
    if (len + 1 < sizeof(cardholder_name)) {
        memset(cardholder_name + len + 1, 0, sizeof(cardholder_name) - len - 1);
    }
    save_state_to_nvs();
    return true;
}

bool openpgp_set_key_fingerprint(uint8_t key_type, const uint8_t *fingerprint,
                                  uint32_t gen_time) {
    if (!fingerprint) return false;

    // Convert gen_time to big-endian bytes
    uint8_t ts[4] = {
        (uint8_t)((gen_time >> 24) & 0xFF),
        (uint8_t)((gen_time >> 16) & 0xFF),
        (uint8_t)((gen_time >> 8) & 0xFF),
        (uint8_t)(gen_time & 0xFF)
    };

    switch (key_type) {
        case KEY_SIG:
            memcpy(fingerprint_sig, fingerprint, OPENPGP_FINGERPRINT_SIZE);
            memcpy(gen_time_sig, ts, 4);
            break;
        case KEY_DEC:
            memcpy(fingerprint_dec, fingerprint, OPENPGP_FINGERPRINT_SIZE);
            memcpy(gen_time_dec, ts, 4);
            break;
        case KEY_AUT:
            memcpy(fingerprint_aut, fingerprint, OPENPGP_FINGERPRINT_SIZE);
            memcpy(gen_time_aut, ts, 4);
            break;
        default:
            LOG_E(TAG, "Invalid key type: 0x%02X", key_type);
            return false;
    }

    save_state_to_nvs();
    LOG_I(TAG, "Fingerprint set for key type 0x%02X", key_type);
    return true;
}

/**
 * \brief Handles APDU `SELECT` command processing.
 * \param apdu Parsed APDU request.
 * \param resp Output response buffer.
 * \param resp_max Maximum size of `resp`.
 * \return APDU status/response length result.
 */
static int cmd_select(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    if (apdu->lc >= 6 && memcmp(apdu->data, OPENPGP_AID, 6) == 0) {
        app_selected = true;
        pw1_verified = false;
        pw3_verified = false;
        session_wipe();
        LOG_I(TAG, "OpenPGP application selected");
        return apdu_sw(resp, SW_OK);
    }

    if (app_selected) {
        session_wipe();
    }
    return apdu_sw(resp, SW_FILE_NOT_FOUND);
}

/**
 * \brief Returns a payload larger than the response window using response
 *        chaining, priming the GET RESPONSE buffer directly.
 *
 * Unlike `apply_response_chaining` (which trims a fully-built response) this
 * handles payloads that exceed `resp_max` by emitting the first chunk plus a
 * `61xx` status word and stashing the remainder. The dispatcher's trailing
 * `apply_response_chaining` is a no-op afterwards (61xx is not SW_OK).
 */
static int respond_chunked(const uint8_t *payload, size_t payload_len, uint32_t le,
                           uint8_t *resp, size_t resp_max) {
    size_t first = (le > 0 && le < payload_len) ? le : payload_len;
    if (first + 2 > resp_max) first = resp_max - 2;
    memcpy(resp, payload, first);
    const size_t remainder = payload_len - first;
    if (remainder == 0) {
        resp[first] = 0x90;
        resp[first + 1] = 0x00;
        return static_cast<int>(first + 2);
    }
    if (remainder > sizeof(g_resp_buffer)) {
        return apdu_sw(resp, SW_WRONG_LENGTH);
    }
    memcpy(g_resp_buffer, payload + first, remainder);
    g_resp_remaining = remainder;
    g_resp_pos = 0;
    resp[first] = 0x61;
    resp[first + 1] = (remainder > 0xFF) ? 0x00 : static_cast<uint8_t>(remainder);
    return static_cast<int>(first + 2);
}

/**
 * \brief Handles APDU `GET DATA` command processing.
 * \param apdu Parsed APDU request.
 * \param resp Output response buffer.
 * \param resp_max Maximum size of `resp`.
 * \return APDU status/response length result.
 */
static int cmd_get_data(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    uint16_t tag = (apdu->p1 << 8) | apdu->p2;

    switch (tag) {
        case DO_AID:  // 0x4F: Full AID
            return apdu_build_response(resp, resp_max, OPENPGP_AID, OPENPGP_AID_LEN, SW_OK);

        case DO_APP_RELATED: {  // 0x6E: Application Related Data
            uint8_t data[512];
            int len = build_do_app_related(data, sizeof(data));
            if (len <= 0) {
                return apdu_sw(resp, SW_UNKNOWN);
            }
            return apdu_build_response(resp, resp_max, data, len, SW_OK);
        }

        case DO_CARDHOLDER: {  // 0x65: Cardholder Related Data
            uint8_t data[128];
            int len = build_do_cardholder(data, sizeof(data));
            if (len <= 0) {
                return apdu_sw(resp, SW_UNKNOWN);
            }
            return apdu_build_response(resp, resp_max, data, len, SW_OK);
        }

        case DO_HIST_BYTES:  // 0x5F52: Historical bytes
            return apdu_build_response(resp, resp_max, HIST_BYTES, sizeof(HIST_BYTES), SW_OK);

        case DO_EXT_CAP:  // 0xC0: Extended Capabilities
            return apdu_build_response(resp, resp_max, EXT_CAPABILITIES, sizeof(EXT_CAPABILITIES), SW_OK);

        case DO_ALGO_SIG: { // 0xC1: Algorithm Attributes - Signature
            size_t algo_len;
            const uint8_t *algo = get_algo_attr(KEY_TYPE_SIG, &algo_len);
            return apdu_build_response(resp, resp_max, algo, algo_len, SW_OK);
        }

        case DO_ALGO_DEC: { // 0xC2: Algorithm Attributes - Decryption
            size_t algo_len;
            const uint8_t *algo = get_algo_attr(KEY_TYPE_DEC, &algo_len);
            return apdu_build_response(resp, resp_max, algo, algo_len, SW_OK);
        }

        case DO_ALGO_AUT: { // 0xC3: Algorithm Attributes - Authentication
            size_t algo_len;
            const uint8_t *algo = get_algo_attr(KEY_TYPE_AUT, &algo_len);
            return apdu_build_response(resp, resp_max, algo, algo_len, SW_OK);
        }

        case DO_PW_STATUS: {  // 0xC4: PW Status Bytes
            uint8_t status[7] = {
                0x01,                           // PW1 valid for multiple signatures
                OPENPGP_PIN_MAX_LEN,            // Max length PW1
                OPENPGP_PIN_MAX_LEN,            // Max length RC
                OPENPGP_PIN_MAX_LEN,            // Max length PW3
                pin_storage_openpgp_pw1_retries(),
                s_rc_len > 0 ? s_rc_retries : static_cast<uint8_t>(0),
                pin_storage_openpgp_pw3_retries()
            };
            return apdu_build_response(resp, resp_max, status, 7, SW_OK);
        }

        case DO_FP_SIG:  // 0xC7: Fingerprint SIG
            return apdu_build_response(resp, resp_max, fingerprint_sig, OPENPGP_FINGERPRINT_SIZE, SW_OK);

        case DO_FP_DEC:  // 0xC8: Fingerprint DEC
            return apdu_build_response(resp, resp_max, fingerprint_dec, OPENPGP_FINGERPRINT_SIZE, SW_OK);

        case DO_FP_AUT:  // 0xC9: Fingerprint AUT
            return apdu_build_response(resp, resp_max, fingerprint_aut, OPENPGP_FINGERPRINT_SIZE, SW_OK);

        case DO_CA_FP_1:  // 0xCA: CA Fingerprint 1
            return apdu_build_response(resp, resp_max, ca_fp_1, OPENPGP_FINGERPRINT_SIZE, SW_OK);

        case DO_CA_FP_2:  // 0xCB: CA Fingerprint 2
            return apdu_build_response(resp, resp_max, ca_fp_2, OPENPGP_FINGERPRINT_SIZE, SW_OK);

        case DO_CA_FP_3:  // 0xCC: CA Fingerprint 3
            return apdu_build_response(resp, resp_max, ca_fp_3, OPENPGP_FINGERPRINT_SIZE, SW_OK);

        case DO_GEN_TIME_SIG:  // 0xCE: Generation time - Signature
            return apdu_build_response(resp, resp_max, gen_time_sig, 4, SW_OK);

        case DO_GEN_TIME_DEC:  // 0xCF: Generation time - Decryption
            return apdu_build_response(resp, resp_max, gen_time_dec, 4, SW_OK);

        case DO_GEN_TIME_AUT:  // 0xD0: Generation time - Authentication
            return apdu_build_response(resp, resp_max, gen_time_aut, 4, SW_OK);

        case DO_SIG_COUNT: {  // 0x93: Signature counter
            uint8_t count[3] = {
                (uint8_t)((sig_count >> 16) & 0xFF),
                (uint8_t)((sig_count >> 8) & 0xFF),
                (uint8_t)(sig_count & 0xFF)
            };
            return apdu_build_response(resp, resp_max, count, 3, SW_OK);
        }

        // URL for public key retrieval
        case DO_URL: {      // 0x5F50
            size_t len = strlen(cardholder_url);
            return apdu_build_response(resp, resp_max, (const uint8_t*)cardholder_url, len, SW_OK);
        }

        // Login data
        case DO_LOGIN: {    // 0x5E
            size_t len = strlen(cardholder_login);
            return apdu_build_response(resp, resp_max, (const uint8_t*)cardholder_login, len, SW_OK);
        }

        // These are already in build_do_cardholder (0x65), but GPG may query them directly too
        case DO_NAME:       // 0x5B: Cardholder name
            return apdu_build_response(resp, resp_max, (const uint8_t*)cardholder_name, strlen(cardholder_name), SW_OK);

        case DO_LANG_PREF:  // 0x5F2D: Language preference
            return apdu_build_response(resp, resp_max, (const uint8_t*)cardholder_lang, strlen(cardholder_lang), SW_OK);

        case DO_SEX:        // 0x5F35: Sex
            return apdu_build_response(resp, resp_max, &cardholder_sex, 1, SW_OK);

        // UIF (User Interaction Flag) - 2 bytes: mode + features
        case DO_UIF_SIG:    // 0xD6: UIF Signature
        case DO_UIF_DEC:    // 0xD7: UIF Decryption
        case DO_UIF_AUT: {  // 0xD8: UIF Authentication
            uint8_t uif[2] = { 0x00, 0x20 };  // Disabled, button available
            return apdu_build_response(resp, resp_max, uif, 2, SW_OK);
        }

        // Key Information - 6 bytes (status of 3 keys)
        // Format: key_ref, status (0x00=generated, 0x01=imported, 0x02=not present)
        case DO_KEY_INFO: {
            uint8_t key_info[6];
            uint8_t pubkey[P256_PUBKEY_SIZE], curve;

            // Check SIG key (RSA blob or SE ECC slot)
            key_info[0] = 0x01;  // Key reference for SIG
            key_info[1] = (role_is_rsa[0] ? gpg_storage_has_rsa_key(0)
                                          : se_ecc_key_read(gpg_storage_sig_slot(), pubkey, sizeof(pubkey), &curve))
                          ? 0x00  // present
                          : 0x02;  // Not present

            // Check DEC key (RSA blob or software ECDH key)
            key_info[2] = 0x02;  // Key reference for DEC
            key_info[3] = (role_is_rsa[1] ? gpg_storage_has_rsa_key(1)
                                          : gpg_storage_has_dec_privkey())
                          ? 0x00
                          : 0x02;

            // Check AUT key (RSA blob or SE ECC slot)
            key_info[4] = 0x03;  // Key reference for AUT
            key_info[5] = (role_is_rsa[2] ? gpg_storage_has_rsa_key(2)
                                          : se_ecc_key_read(gpg_storage_aut_slot(), pubkey, sizeof(pubkey), &curve))
                          ? 0x00
                          : 0x02;

            return apdu_build_response(resp, resp_max, key_info, 6, SW_OK);
        }

        // Security Support Template - contains signature counter
        case DO_SEC_TPL: {
            // Format: 7A <len> { 93 03 <sig_count[3]> }
            uint8_t sec_tpl[7] = {
                0x93, 0x03,  // Tag + length for signature counter
                (uint8_t)((sig_count >> 16) & 0xFF),
                (uint8_t)((sig_count >> 8) & 0xFF),
                (uint8_t)(sig_count & 0xFF)
            };
            return apdu_build_response(resp, resp_max, sec_tpl, 5, SW_OK);
        }

        // KDF-DO (Key Derivation Function). Returns the stored DO bytes, or the
        // "disabled" body (81 01 00) when KDF has never been configured.
        case DO_KDF: {
            if (kdf_do_len > 0) {
                return apdu_build_response(resp, resp_max, kdf_do_bytes, kdf_do_len, SW_OK);
            }
            uint8_t disabled[3];
            size_t disabled_len = 0;
            kdf_do_build_disabled(disabled, sizeof(disabled), &disabled_len);
            return apdu_build_response(resp, resp_max, disabled, disabled_len, SW_OK);
        }

        case DO_CARDHOLDER_CERT: {
            static EXT_RAM_BSS_ATTR uint8_t cert_buf[CARDHOLDER_CERT_MAX];
            size_t cert_len = load_cardholder_cert(cert_buf, sizeof(cert_buf));
            if (cert_len == 0) {
                return apdu_sw(resp, SW_REFERENCED_DATA_NOT_FOUND);
            }
            return respond_chunked(cert_buf, cert_len, apdu->le, resp, resp_max);
        }

        default:
            LOG_W(TAG, "GET DATA: Unknown tag 0x%04X", tag);
            return apdu_sw(resp, SW_REFERENCED_DATA_NOT_FOUND);
    }
}

/**
 * \brief Storage kind for PUT DATA descriptor entries.
 *
 * `BLOB_FIXED` requires `apdu->lc == max_size`; `STRING_BOUNDED` requires
 * `apdu->lc < max_size` and writes a trailing NUL terminator.
 */
typedef enum {
    PUT_KIND_BLOB_FIXED = 0,
    PUT_KIND_STRING_BOUNDED = 1,
} put_data_kind_t;

/**
 * \brief Descriptor entry for table-driven PUT DATA processing.
 *
 * Cases that follow the simple "validate length, memcpy, save" pattern are
 * looked up in this table; cases with custom logic remain handled inline.
 */
typedef struct {
    uint16_t          tag;        /**< OpenPGP DO tag value. */
    void             *buffer;     /**< Destination buffer pointer. */
    size_t            max_size;   /**< Buffer capacity in bytes. */
    put_data_kind_t   kind;       /**< Storage kind. */
    const char       *log_label;  /**< Optional log label, may be NULL. */
} put_data_desc_t;

/**
 * \brief Returns descriptor for an OpenPGP PUT DATA tag.
 * \param tag OpenPGP DO tag.
 * \return Pointer to descriptor or NULL if tag is not handled by the table.
 */
static const put_data_desc_t* find_put_data_desc(uint16_t tag) {
    static const put_data_desc_t k_put_data_table[] = {
        // Cardholder profile (string-bounded, written with trailing NUL)
        { DO_NAME,          cardholder_name,    sizeof(cardholder_name),    PUT_KIND_STRING_BOUNDED, "Cardholder name" },
        { DO_LANG_PREF,     cardholder_lang,    sizeof(cardholder_lang),    PUT_KIND_STRING_BOUNDED, NULL },
        { DO_URL,           cardholder_url,     sizeof(cardholder_url),     PUT_KIND_STRING_BOUNDED, "URL" },
        { DO_LOGIN,         cardholder_login,   sizeof(cardholder_login),   PUT_KIND_STRING_BOUNDED, "Login" },

        // Fixed-size fingerprints (SHA-1 size)
        { DO_FP_SIG,        fingerprint_sig,    OPENPGP_FINGERPRINT_SIZE,   PUT_KIND_BLOB_FIXED, "Fingerprint SIG" },
        { DO_FP_DEC,        fingerprint_dec,    OPENPGP_FINGERPRINT_SIZE,   PUT_KIND_BLOB_FIXED, "Fingerprint DEC" },
        { DO_FP_AUT,        fingerprint_aut,    OPENPGP_FINGERPRINT_SIZE,   PUT_KIND_BLOB_FIXED, "Fingerprint AUT" },
        { DO_CA_FP_1,       ca_fp_1,            OPENPGP_FINGERPRINT_SIZE,   PUT_KIND_BLOB_FIXED, NULL },
        { DO_CA_FP_2,       ca_fp_2,            OPENPGP_FINGERPRINT_SIZE,   PUT_KIND_BLOB_FIXED, NULL },
        { DO_CA_FP_3,       ca_fp_3,            OPENPGP_FINGERPRINT_SIZE,   PUT_KIND_BLOB_FIXED, NULL },

        // Fixed-size 4-byte big-endian generation timestamps
        { DO_GEN_TIME_SIG,  gen_time_sig,       sizeof(gen_time_sig),       PUT_KIND_BLOB_FIXED, NULL },
        { DO_GEN_TIME_DEC,  gen_time_dec,       sizeof(gen_time_dec),       PUT_KIND_BLOB_FIXED, NULL },
        { DO_GEN_TIME_AUT,  gen_time_aut,       sizeof(gen_time_aut),       PUT_KIND_BLOB_FIXED, NULL },
    };

    const size_t n = sizeof(k_put_data_table) / sizeof(k_put_data_table[0]);
    for (size_t i = 0; i < n; ++i) {
        if (k_put_data_table[i].tag == tag) {
            return &k_put_data_table[i];
        }
    }
    return NULL;
}

/**
 * \brief Applies a PUT DATA descriptor to the request payload.
 * \param desc Descriptor from `find_put_data_desc`.
 * \param apdu Parsed APDU containing the new value.
 * \param resp Response buffer.
 * \return APDU status/response length result.
 */
static int apply_put_data_desc(const put_data_desc_t *desc, const apdu_t *apdu, uint8_t *resp) {
    if (desc->kind == PUT_KIND_STRING_BOUNDED) {
        if (apdu->lc >= desc->max_size) {
            return apdu_sw(resp, SW_WRONG_LENGTH);
        }
        char *str = (char *)desc->buffer;
        memcpy(str, apdu->data, apdu->lc);
        str[apdu->lc] = '\0';
        save_state_to_nvs();
        if (desc->log_label) {
            LOG_I(TAG, "%s set: %s", desc->log_label, str);
        }
        return apdu_sw(resp, SW_OK);
    }

    // BLOB_FIXED: exact length match required
    if (apdu->lc != desc->max_size) {
        return apdu_sw(resp, SW_WRONG_LENGTH);
    }
    memcpy(desc->buffer, apdu->data, desc->max_size);
    save_state_to_nvs();
    if (desc->log_label) {
        LOG_I(TAG, "%s stored", desc->log_label);
    }
    return apdu_sw(resp, SW_OK);
}

/**
 * \brief Handles APDU `PUT DATA` command processing.
 * \param apdu Parsed APDU request.
 * \param resp Output response buffer.
 * \param resp_max Maximum size of `resp`.
 * \return APDU status/response length result.
 */
/**
 * \brief Parses and applies a PUT DATA payload addressed to one of the
 * algorithm-attribute Data Objects (C1, C2, C3).
 *
 * For SIG (C1) and AUT (C3) the selected curve is updated and the existing
 * key in the corresponding slot is wiped to force the host to regenerate it.
 * DEC (C2) is constrained to P-256 ECDH (software path), so we accept the
 * documented attribute and reject everything else.
 */
/**
 * \brief Invalidates all stored key material for a role (ECC slot, RSA blob,
 *        software DEC key, fingerprint and generation time). Used when the
 *        algorithm attributes change so the next GENERATE / import is clean.
 * \param r Role index: 0 = SIG, 1 = DEC, 2 = AUT.
 */
static void wipe_role_key(int r) {
    auto* se = get_se();
    gpg_storage_delete_rsa_key(static_cast<uint8_t>(r));
    switch (r) {
        case 0:
            if (se) se->eccDelete(gpg_storage_sig_slot());
            memset(fingerprint_sig, 0, sizeof(fingerprint_sig));
            memset(gen_time_sig, 0, sizeof(gen_time_sig));
            break;
        case 1:
            gpg_storage_delete_dec_privkey();
            memset(fingerprint_dec, 0, sizeof(fingerprint_dec));
            memset(gen_time_dec, 0, sizeof(gen_time_dec));
            break;
        case 2:
            if (se) se->eccDelete(gpg_storage_aut_slot());
            memset(fingerprint_aut, 0, sizeof(fingerprint_aut));
            memset(gen_time_aut, 0, sizeof(gen_time_aut));
            break;
        default:
            break;
    }
}

static int put_data_algo_attr(uint16_t tag, const apdu_t *apdu, uint8_t *resp) {
    algo_attr_t attr;
    if (algo_attr_parse(apdu->data, apdu->lc, &attr) != ALGO_ATTR_OK) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }
    algo_attr_role_t role;
    int r;
    switch (tag) {
        case DO_ALGO_SIG: role = ALGO_ATTR_ROLE_SIG; r = 0; break;
        case DO_ALGO_AUT: role = ALGO_ATTR_ROLE_AUT; r = 2; break;
        case DO_ALGO_DEC: role = ALGO_ATTR_ROLE_DEC; r = 1; break;
        default:          return apdu_sw(resp, SW_FILE_NOT_FOUND);
    }
    if (algo_attr_validate_role(&attr, role) != ALGO_ATTR_OK) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }
    if (algo_attr_validate_capability(&attr, /*rsa_supported=*/true) != ALGO_ATTR_OK) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }

    if (attr.is_rsa) {
        if (role_is_rsa[r] && role_rsa_n_bits[r] == attr.rsa_n_bits) {
            return apdu_sw(resp, SW_OK);
        }
        wipe_role_key(r);
        role_is_rsa[r]     = true;
        role_rsa_n_bits[r] = attr.rsa_n_bits;
        role_rsa_e_bits[r] = attr.rsa_e_bits ? attr.rsa_e_bits : 32;
        role_rsa_fmt[r]    = attr.rsa_import_fmt;
        save_state_to_nvs();
        LOG_I(TAG, "Algorithm attributes for role %d set to RSA-%u", r, attr.rsa_n_bits);
        return apdu_sw(resp, SW_OK);
    }

    if (role == ALGO_ATTR_ROLE_DEC) {
        // DEC ECC is constrained to P-256 ECDH (software path).
        if (attr.curve != ALGO_ATTR_CURVE_P256 || attr.algo_id != ALGO_ATTR_ID_ECDH) {
            return apdu_sw(resp, SW_WRONG_DATA);
        }
        if (!role_is_rsa[r]) {
            return apdu_sw(resp, SW_OK);
        }
        wipe_role_key(r);
        role_is_rsa[r] = false;
        save_state_to_nvs();
        LOG_I(TAG, "DEC role reverted to P-256 ECDH");
        return apdu_sw(resp, SW_OK);
    }

    // SIG / AUT ECC: honour the selected curve.
    const uint8_t new_curve = (attr.curve == ALGO_ATTR_CURVE_ED25519) ? CDC_CURVE_ED25519
                                                                       : CDC_CURVE_P256;
    uint8_t* target = (tag == DO_ALGO_SIG) ? &selected_curve_sig : &selected_curve_aut;
    if (!role_is_rsa[r] && *target == new_curve) {
        return apdu_sw(resp, SW_OK);
    }
    wipe_role_key(r);
    role_is_rsa[r] = false;
    *target = new_curve;
    save_state_to_nvs();
    LOG_I(TAG, "Algorithm attributes for %s updated to curve %u",
             (tag == DO_ALGO_SIG) ? "SIG" : "AUT", new_curve);
    return apdu_sw(resp, SW_OK);
}

static int cmd_put_data(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    (void)resp_max;
    if (!pw3_verified) {
        return apdu_sw(resp, SW_SECURITY_NOT_SATISFIED);
    }

    uint16_t tag = (apdu->p1 << 8) | apdu->p2;

    // Special-case DOs that need custom handling stay inline.
    switch (tag) {
        case DO_SEX:
            if (apdu->lc == 1) {
                cardholder_sex = apdu->data[0];
                save_state_to_nvs();
                return apdu_sw(resp, SW_OK);
            }
            return apdu_sw(resp, SW_WRONG_LENGTH);
        case DO_ALGO_SIG:
        case DO_ALGO_DEC:
        case DO_ALGO_AUT:
            return put_data_algo_attr(tag, apdu, resp);
        case DO_RC:
            // Set or clear the Resetting Code. Lc==0 clears the RC entirely.
            if (apdu->lc == 0) {
                mbedtls_platform_zeroize(s_rc_salt, sizeof(s_rc_salt));
                mbedtls_platform_zeroize(s_rc_hash, sizeof(s_rc_hash));
                s_rc_len = 0;
                s_rc_retries = 3;
                save_state_to_nvs();
                LOG_I(TAG, "Resetting Code cleared");
                return apdu_sw(resp, SW_OK);
            }
            if (apdu->lc < OPENPGP_RC_MIN_LEN || apdu->lc > OPENPGP_PIN_MAX_LEN) {
                return apdu_sw(resp, SW_WRONG_LENGTH);
            }
            {
                uint8_t new_salt[RC_SALT_SIZE];
                uint8_t new_hash[RC_HASH_SIZE];
                se_random_fill(new_salt, RC_SALT_SIZE);
                if (!compute_rc_hash(apdu->data, apdu->lc, new_salt, new_hash)) {
                    mbedtls_platform_zeroize(new_salt, sizeof(new_salt));
                    mbedtls_platform_zeroize(new_hash, sizeof(new_hash));
                    return apdu_sw(resp, SW_UNKNOWN);
                }
                memcpy(s_rc_salt, new_salt, RC_SALT_SIZE);
                memcpy(s_rc_hash, new_hash, RC_HASH_SIZE);
                mbedtls_platform_zeroize(new_salt, sizeof(new_salt));
                mbedtls_platform_zeroize(new_hash, sizeof(new_hash));
            }
            s_rc_len = static_cast<uint8_t>(apdu->lc);
            s_rc_retries = 3;
            save_state_to_nvs();
            LOG_I(TAG, "Resetting Code configured (length=%u)", s_rc_len);
            return apdu_sw(resp, SW_OK);
        case DO_AES_KEY: {
            if (apdu->lc != 16 && apdu->lc != 32) {
                return apdu_sw(resp, SW_WRONG_LENGTH);
            }
            const char* pin = s_session_pin[0] ? s_session_pin : nullptr;
            bool ok = gpg_storage_save_aes_key(apdu->data, apdu->lc, pin);
            return apdu_sw(resp, ok ? SW_OK : SW_UNKNOWN);
        }
        case DO_KDF:
            return apdu_sw(resp, apply_kdf_do(apdu->data, apdu->lc));
        case DO_CARDHOLDER_CERT:
            if (apdu->lc > CARDHOLDER_CERT_MAX) {
                return apdu_sw(resp, SW_WRONG_LENGTH);
            }
            if (!save_cardholder_cert(apdu->data, apdu->lc)) {
                return apdu_sw(resp, SW_UNKNOWN);
            }
            LOG_I(TAG, "Cardholder certificate stored (%u bytes)", apdu->lc);
            return apdu_sw(resp, SW_OK);
        default:
            break;
    }

    // Table-driven simple cases (validate, memcpy, persist).
    const put_data_desc_t *desc = find_put_data_desc(tag);
    if (desc) {
        return apply_put_data_desc(desc, apdu, resp);
    }

    LOG_W(TAG, "PUT DATA: Unknown tag 0x%04X", tag);
    return apdu_sw(resp, SW_FILE_NOT_FOUND);
}

static void update_generation_timestamp(uint8_t key_ref);

/**
 * \brief Parse one BER-TLV field at \p pos.
 *
 * Supports 1- or 2-byte tags and short-form / 0x81 / 0x82 length encodings,
 * which covers everything the OpenPGP 3.4.1 PUT DATA odd payload uses.
 *
 * \return true on success; \p pos is advanced past the parsed value.
 */
static bool ehl_parse_one(const uint8_t *buf, size_t buf_len, size_t *pos,
                          uint16_t *tag_out, const uint8_t **value_out,
                          size_t *value_len_out) {
    if (!buf || !pos || *pos >= buf_len) return false;
    size_t p = *pos;

    uint16_t tag = buf[p++];
    if ((tag & 0x1F) == 0x1F) {
        if (p >= buf_len) return false;
        tag = (tag << 8) | buf[p++];
    }
    if (p >= buf_len) return false;

    size_t len;
    uint8_t lb = buf[p++];
    if (lb < 0x80) {
        len = lb;
    } else if (lb == 0x81) {
        if (p >= buf_len) return false;
        len = buf[p++];
    } else if (lb == 0x82) {
        if (p + 1 >= buf_len) return false;
        len = (static_cast<size_t>(buf[p]) << 8) | buf[p + 1];
        p += 2;
    } else {
        return false;
    }
    if (p + len > buf_len) return false;

    *tag_out = tag;
    *value_out = buf + p;
    *value_len_out = len;
    *pos = p + len;
    return true;
}

/**
 * \brief Splits the RSA key material (5F48) into e / p / q using the lengths
 *        declared in the Cardholder Private Key Template (7F48).
 *
 * The template is a sequence of `<tag><length>` pairs with no inline value;
 * the 5F48 concatenation carries the values in the same order. RSA tags:
 * 0x91 = e, 0x92 = p, 0x93 = q (0x94-0x97 are recomputed by mbedTLS).
 * \return `true` when e, p and q were all located.
 */
static bool parse_rsa_import(const uint8_t* tmpl, size_t tmpl_len,
                             const uint8_t* concat, size_t concat_len,
                             const uint8_t** e, size_t* e_len,
                             const uint8_t** p, size_t* p_len,
                             const uint8_t** q, size_t* q_len) {
    size_t tpos = 0, cpos = 0;
    *e = *p = *q = nullptr;
    *e_len = *p_len = *q_len = 0;
    while (tpos < tmpl_len) {
        const uint8_t t = tmpl[tpos++];
        if (tpos >= tmpl_len) return false;
        size_t clen;
        const uint8_t lb = tmpl[tpos++];
        if (lb < 0x80) {
            clen = lb;
        } else if (lb == 0x81) {
            if (tpos >= tmpl_len) return false;
            clen = tmpl[tpos++];
        } else if (lb == 0x82) {
            if (tpos + 1 >= tmpl_len) return false;
            clen = (static_cast<size_t>(tmpl[tpos]) << 8) | tmpl[tpos + 1];
            tpos += 2;
        } else {
            return false;
        }
        if (cpos + clen > concat_len) return false;
        const uint8_t* val = concat + cpos;
        cpos += clen;
        switch (t) {
            case 0x91: *e = val; *e_len = clen; break;
            case 0x92: *p = val; *p_len = clen; break;
            case 0x93: *q = val; *q_len = clen; break;
            default: break;
        }
    }
    return (*e && *p && *q);
}

/**
 * \brief Handles APDU `PUT DATA (odd INS, 0xDB)` for keypair import.
 *
 * Used by `gpg --card-edit` → `keytocard` / off-card backup import: GnuPG ships
 * a key as an OpenPGP 3.4.1 §7.2.8 Extended Header List
 *
 *     4D LL
 *        <CRT>          // B6 00 (SIG) / B8 00 (DEC) / A4 00 (AUT)
 *        7F48 LL        // Cardholder Private Key Template (tag list)
 *        5F48 LL        // Concatenation of values
 *
 * ECC roles: the 32-byte private scalar is imported into the TROPIC01 ECC slot
 * (SIG / AUT) or the software ECDH store (DEC). RSA roles: e / p / q are parsed
 * per the template and stored as an encrypted blob in the RSA R-Memory pool.
 */
static int cmd_put_data_odd(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    (void)resp_max;
    if (!pw3_verified) {
        return apdu_sw(resp, SW_SECURITY_NOT_SATISFIED);
    }
    if (apdu->p1 != 0x3F || apdu->p2 != 0xFF) {
        return apdu_sw(resp, SW_INCORRECT_P1P2);
    }
    if (apdu->lc == 0 || !apdu->data) {
        return apdu_sw(resp, SW_WRONG_LENGTH);
    }

    size_t pos = 0;
    uint16_t tag = 0;
    const uint8_t *outer_val = nullptr;
    size_t outer_len = 0;
    if (!ehl_parse_one(apdu->data, apdu->lc, &pos, &tag, &outer_val, &outer_len) ||
        tag != 0x4D) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }

    // First inner element is the CRT identifying the target role.
    size_t inner = 0;
    const uint8_t *crt_val = nullptr;
    size_t crt_len = 0;
    if (!ehl_parse_one(outer_val, outer_len, &inner, &tag, &crt_val, &crt_len)) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }
    const uint8_t key_ref = static_cast<uint8_t>(tag);
    const int r = role_index_for_key_ref(key_ref);
    if (r < 0) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }

    // Collect the private-key template (7F48) and the value concatenation (5F48).
    const uint8_t *tmpl = nullptr;   size_t tmpl_len = 0;
    const uint8_t *concat = nullptr; size_t concat_len = 0;
    while (inner < outer_len) {
        const uint8_t *v = nullptr;
        size_t vlen = 0;
        if (!ehl_parse_one(outer_val, outer_len, &inner, &tag, &v, &vlen)) {
            return apdu_sw(resp, SW_WRONG_DATA);
        }
        if (tag == 0x7F48)      { tmpl = v;   tmpl_len = vlen; }
        else if (tag == 0x5F48) { concat = v; concat_len = vlen; }
    }
    if (!concat || concat_len == 0) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }

    if (role_is_rsa[r]) {
        if (!tmpl) {
            return apdu_sw(resp, SW_WRONG_DATA);
        }
        const uint8_t *e = nullptr, *p = nullptr, *q = nullptr;
        size_t e_len = 0, p_len = 0, q_len = 0;
        if (!parse_rsa_import(tmpl, tmpl_len, concat, concat_len,
                              &e, &e_len, &p, &p_len, &q, &q_len)) {
            return apdu_sw(resp, SW_WRONG_DATA);
        }
        static EXT_RAM_BSS_ATTR uint8_t blob[GPG_RSA_BLOB_MAX];
        size_t blob_len = 0;
        bool built = gpg_rsa_blob_build(role_rsa_n_bits[r], e, e_len, p, p_len, q, q_len,
                                        blob, sizeof(blob), &blob_len);
        bool saved = built && gpg_storage_save_rsa_key(static_cast<uint8_t>(r), blob, blob_len, nullptr);
        mbedtls_platform_zeroize(blob, sizeof(blob));
        if (!saved) {
            return apdu_sw(resp, built ? SW_UNKNOWN : SW_WRONG_DATA);
        }
        update_generation_timestamp(key_ref);
        LOG_I(TAG, "Imported RSA key for role %d", r);
        return apdu_sw(resp, SW_OK);
    }

    // ECC import: the first 32 bytes of the concatenation are the private scalar.
    if (concat_len < P256_PRIVKEY_SIZE) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }
    uint8_t scalar[P256_PRIVKEY_SIZE];
    memcpy(scalar, concat, P256_PRIVKEY_SIZE);
    bool ok = false;
    if (r == 1) {
        // DEC: software ECDH key.
        ok = gpg_storage_save_dec_privkey(scalar, nullptr);
    } else {
        // SIG / AUT: inject into the TROPIC01 ECC slot.
        auto* se = get_se();
        const uint8_t slot = (r == 0) ? gpg_storage_sig_slot() : gpg_storage_aut_slot();
        const uint8_t curve = (r == 0) ? selected_curve_sig : selected_curve_aut;
        const cdc::hal::EccCurve ec = (curve == CDC_CURVE_ED25519) ? cdc::hal::EccCurve::ED25519
                                                                   : cdc::hal::EccCurve::P256;
        ok = se && (se->eccImport(slot, scalar, ec) == cdc::hal::SeResult::OK);
    }
    mbedtls_platform_zeroize(scalar, sizeof(scalar));
    if (!ok) {
        return apdu_sw(resp, SW_UNKNOWN);
    }
    update_generation_timestamp(key_ref);
    LOG_I(TAG, "Imported ECC key for role %d", r);
    return apdu_sw(resp, SW_OK);
}

/**
 * \brief Handles APDU `VERIFY` command for PIN verification.
 * \param apdu Parsed APDU request.
 * \param resp Output response buffer.
 * \param resp_max Maximum size of `resp`.
 * \return APDU status/response length result.
 */
static int cmd_verify(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    uint8_t pw_ref = apdu->p2;

    // Check remaining retries (Lc=0 means query)
    if (apdu->lc == 0) {
        uint8_t retries;
        if (pw_ref == PW1_CODE_1 || pw_ref == PW1_CODE_2) {
            if (pin_storage_openpgp_pw1_blocked()) {
                return apdu_sw(resp, SW_AUTH_METHOD_BLOCKED);
            }
            retries = pin_storage_openpgp_pw1_retries();
        } else if (pw_ref == PW3_CODE) {
            if (pin_storage_openpgp_pw3_blocked()) {
                return apdu_sw(resp, SW_AUTH_METHOD_BLOCKED);
            }
            retries = pin_storage_openpgp_pw3_retries();
        } else {
            return apdu_sw(resp, SW_INCORRECT_P1P2);
        }
        return apdu_sw(resp, 0x63C0 | retries);
    }

    // In KDF mode the host sends a PBKDF2 pre-hash (binary, 32/64 bytes); the
    // cleartext path caps at OPENPGP_PIN_MAX_LEN and NUL-terminates.
    bool verified = false;
    uint8_t retries;

    if (pw_ref == PW1_CODE_1 || pw_ref == PW1_CODE_2) {
        // A 32/64-byte field is a KDF pre-hash (a cleartext PIN never reaches
        // that length): verify it raw even before PUT DATA 0xF9 flips kdf_active
        // on, so gpg's kdf-setup VERIFY succeeds during the enable transition.
        if (kdf_active || apdu->lc == 32 || apdu->lc == 64) {
            verified = pin_storage_openpgp_verify_pw1_raw(apdu->data, apdu->lc);
            if (verified) {
                pw1_verified = true;
                mbedtls_platform_zeroize(s_session_pin, sizeof(s_session_pin));
                gpg_storage_clear_session();
            }
        } else {
            if (apdu->lc > OPENPGP_PIN_MAX_LEN) {
                return apdu_sw(resp, SW_WRONG_LENGTH);
            }
            char pin_str[OPENPGP_PIN_MAX_LEN + 1];
            memcpy(pin_str, apdu->data, apdu->lc);
            pin_str[apdu->lc] = '\0';
            verified = pin_storage_openpgp_verify_pw1(pin_str);
            if (verified) {
                pw1_verified = true;
                strncpy(s_session_pin, pin_str, OPENPGP_PIN_MAX_LEN);
                s_session_pin[OPENPGP_PIN_MAX_LEN] = '\0';
                gpg_storage_set_session_pin(pin_str);
            }
            mbedtls_platform_zeroize(pin_str, sizeof(pin_str));
        }
        if (verified) {
            LOG_I(TAG, "PW1 verified successfully");
        }
        retries = pin_storage_openpgp_pw1_retries();
    } else if (pw_ref == PW3_CODE) {
        if (kdf_active || apdu->lc == 32 || apdu->lc == 64) {
            verified = pin_storage_openpgp_verify_pw3_raw(apdu->data, apdu->lc);
        } else {
            if (apdu->lc > OPENPGP_PIN_MAX_LEN) {
                return apdu_sw(resp, SW_WRONG_LENGTH);
            }
            char pin_str[OPENPGP_PIN_MAX_LEN + 1];
            memcpy(pin_str, apdu->data, apdu->lc);
            pin_str[apdu->lc] = '\0';
            verified = pin_storage_openpgp_verify_pw3(pin_str);
            mbedtls_platform_zeroize(pin_str, sizeof(pin_str));
        }
        if (verified) {
            pw3_verified = true;
            LOG_I(TAG, "PW3 verified successfully");
        }
        retries = pin_storage_openpgp_pw3_retries();
    } else {
        return apdu_sw(resp, SW_INCORRECT_P1P2);
    }

    if (verified) {
        return apdu_sw(resp, SW_OK);
    }

    // Verification failed
    if (retries == 0) {
        LOG_W(TAG, "PIN blocked after too many failures");
        return apdu_sw(resp, SW_AUTH_METHOD_BLOCKED);
    }
    LOG_W(TAG, "PIN verification failed, %d retries left", retries);
    return apdu_sw(resp, 0x63C0 | retries);
}

/**
 * \brief PIN slot identifier used by PIN helper routines.
 */
typedef enum {
    PIN_SLOT_PW1 = 0,
    PIN_SLOT_PW3 = 1
} pin_slot_t;

/**
 * \brief Computes the iterated-salted S2K hash (OpenPGP KDF) for a PIN candidate.
 * \param pin PIN string.
 * \param salt 8-byte salt.
 * \param iterations Iteration count from PinManager.
 * \param hash_out 32-byte output buffer.
 * \return `true` on success.
 */
static bool compute_kdf_hash(const char* pin, const uint8_t* salt, uint32_t iterations,
                             uint8_t hash_out[32]) {
    if (!pin || !salt || !hash_out) return false;
    size_t pin_len = strlen(pin);
    if (pin_len > OPENPGP_PIN_MAX_LEN) return false;
    size_t combined = 8 + pin_len;
    if (combined == 0) return false;

    uint8_t buffer[8 + OPENPGP_PIN_MAX_LEN];
    memcpy(buffer, salt, 8);
    memcpy(buffer + 8, pin, pin_len);

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        mbedtls_platform_zeroize(buffer, sizeof(buffer));
        return false;
    }
    size_t processed = 0;
    size_t total_bytes = iterations;
    while (processed < total_bytes) {
        size_t chunk = (total_bytes - processed < combined) ? (total_bytes - processed) : combined;
        if (mbedtls_sha256_update(&ctx, buffer, chunk) != 0) {
            mbedtls_sha256_free(&ctx);
            mbedtls_platform_zeroize(buffer, sizeof(buffer));
            return false;
        }
        processed += chunk;
    }
    int rc = mbedtls_sha256_finish(&ctx, hash_out);
    mbedtls_sha256_free(&ctx);
    mbedtls_platform_zeroize(buffer, sizeof(buffer));
    return rc == 0;
}

/**
 * \brief Constant-time comparison of two equal-length byte buffers.
 * \param a First buffer.
 * \param b Second buffer.
 * \param n Number of bytes to compare.
 * \return `true` if identical.
 */
static bool const_time_equal(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

/**
 * \brief Compares a candidate PIN against the stored hash without touching retry counters.
 * \param slot PIN slot to query.
 * \param pin Candidate PIN string.
 * \return `true` if PIN matches the stored hash.
 */
static bool peek_verify_pin(pin_slot_t slot, const char* pin) {
    auto& mgr = cdc::core::PinManager::instance();
    uint8_t salt[8] = {};
    uint8_t stored[32] = {};
    uint8_t candidate[32] = {};
    bool ok = false;

    if (slot == PIN_SLOT_PW1) {
        if (!mgr.getPW1Salt(salt)) goto done;
        if (!mgr.getPW1Hash(stored)) goto done;
    } else {
        if (!mgr.getPW3Salt(salt)) goto done;
        if (!mgr.getPW3Hash(stored)) goto done;
    }

    if (!compute_kdf_hash(pin, salt, mgr.getIterationCount(), candidate)) goto done;
    ok = const_time_equal(candidate, stored, sizeof(stored));

done:
    mbedtls_platform_zeroize(stored, sizeof(stored));
    mbedtls_platform_zeroize(candidate, sizeof(candidate));
    return ok;
}

/**
 * \brief Type alias for PIN change callbacks.
 */
typedef bool (*pin_change_fn_t)(const char *pin);

/**
 * \brief Searches the split point for `CHANGE REFERENCE DATA` without consuming retries.
 *
 * Iterates over candidate old-PIN lengths and uses a non-decrementing hash
 * comparison. Only the matched split is applied through `change_fn`; the
 * underlying retry counter is left untouched until the caller consumes one
 * retry on overall failure.
 *
 * \param data Concatenated old||new PIN bytes.
 * \param len Total length of `data`.
 * \param min_len Minimum PIN length for the target slot.
 * \param slot PIN slot identifier.
 * \param change_fn Callback that applies the new PIN.
 * \return `true` if a split was found and the change succeeded.
 */
static bool try_change_pin(const uint8_t *data, size_t len, size_t min_len,
                           pin_slot_t slot, pin_change_fn_t change_fn) {
    if (len < min_len * 2) {
        return false;
    }
    for (size_t old_len = min_len; old_len <= len - min_len; ++old_len) {
        size_t new_len = len - old_len;
        if (old_len > OPENPGP_PIN_MAX_LEN || new_len > OPENPGP_PIN_MAX_LEN) {
            continue;
        }
        char old_pin[OPENPGP_PIN_MAX_LEN + 1];
        char new_pin[OPENPGP_PIN_MAX_LEN + 1];
        memcpy(old_pin, data, old_len);
        old_pin[old_len] = '\0';
        memcpy(new_pin, data + old_len, new_len);
        new_pin[new_len] = '\0';

        if (peek_verify_pin(slot, old_pin) && change_fn(new_pin)) {
            mbedtls_platform_zeroize(old_pin, sizeof(old_pin));
            mbedtls_platform_zeroize(new_pin, sizeof(new_pin));
            return true;
        }
        mbedtls_platform_zeroize(old_pin, sizeof(old_pin));
        mbedtls_platform_zeroize(new_pin, sizeof(new_pin));
    }
    return false;
}

/**
 * \brief Handles APDU `CHANGE REFERENCE DATA` command for PIN updates.
 * \param apdu Parsed APDU request.
 * \param resp Output response buffer.
 * \param resp_max Maximum size of `resp`.
 * \return APDU status/response length result.
 */
static int cmd_change_reference_data(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    (void)resp_max;
    uint8_t pw_ref = apdu->p2;

    if (apdu->lc == 0) {
        return apdu_sw(resp, SW_WRONG_LENGTH);
    }

    pin_change_fn_t change_fn = NULL;
    size_t min_len = 0;
    uint8_t (*retries_fn)(void) = NULL;
    pin_slot_t slot;
    const char *log_label = NULL;

    if (pw_ref == PW1_CODE_1) {
        slot = PIN_SLOT_PW1;
        change_fn = pin_storage_openpgp_change_pw1;
        retries_fn = pin_storage_openpgp_pw1_retries;
        min_len = OPENPGP_PW1_MIN_LEN;
        log_label = "PW1";
    } else if (pw_ref == PW3_CODE) {
        slot = PIN_SLOT_PW3;
        change_fn = pin_storage_openpgp_change_pw3;
        retries_fn = pin_storage_openpgp_pw3_retries;
        min_len = OPENPGP_PW3_MIN_LEN;
        log_label = "PW3";
    } else {
        return apdu_sw(resp, SW_INCORRECT_P1P2);
    }

    // KDF mode: payload is old-prehash || new-prehash, each kdf_pin_len bytes.
    if (kdf_active) {
        if (apdu->lc != static_cast<uint16_t>(2 * kdf_pin_len)) {
            return apdu_sw(resp, SW_WRONG_LENGTH);
        }
        const uint8_t* old_h = apdu->data;
        const uint8_t* new_h = apdu->data + kdf_pin_len;
        bool old_ok = (slot == PIN_SLOT_PW1)
                          ? pin_storage_openpgp_verify_pw1_raw(old_h, kdf_pin_len)
                          : pin_storage_openpgp_verify_pw3_raw(old_h, kdf_pin_len);
        if (!old_ok) {
            uint8_t retries = retries_fn();
            if (retries == 0) {
                return apdu_sw(resp, SW_AUTH_METHOD_BLOCKED);
            }
            return apdu_sw(resp, 0x63C0 | retries);
        }
        bool set_ok = (slot == PIN_SLOT_PW1)
                          ? pin_storage_openpgp_set_pw1_raw(new_h, kdf_pin_len)
                          : pin_storage_openpgp_set_pw3_raw(new_h, kdf_pin_len);
        if (!set_ok) {
            return apdu_sw(resp, SW_WRONG_DATA);
        }
        LOG_I(TAG, "%s changed successfully (KDF)", log_label);
        return apdu_sw(resp, SW_OK);
    }

    // KDF-enable transition: gpg sends `cleartext-old || raw-new` here (before
    // PUT DATA 0xF9 turns KDF on), where the new value is a 32- or 64-byte KDF
    // pre-hash. Verify the cleartext old normally, then store the raw new ref.
    {
        size_t new_raw = 0;
        if (apdu->lc >= min_len + 64 && apdu->lc - 64 <= OPENPGP_PIN_MAX_LEN) {
            new_raw = 64;
        } else if (apdu->lc >= min_len + 32 && apdu->lc - 32 <= OPENPGP_PIN_MAX_LEN) {
            new_raw = 32;
        }
        if (new_raw) {
            const size_t old_len = apdu->lc - new_raw;
            char old_pin[OPENPGP_PIN_MAX_LEN + 1];
            memcpy(old_pin, apdu->data, old_len);
            old_pin[old_len] = '\0';
            const bool old_ok = (slot == PIN_SLOT_PW1)
                                    ? pin_storage_openpgp_verify_pw1(old_pin)
                                    : pin_storage_openpgp_verify_pw3(old_pin);
            mbedtls_platform_zeroize(old_pin, sizeof(old_pin));
            if (!old_ok) {
                uint8_t retries = retries_fn();
                if (retries == 0) {
                    return apdu_sw(resp, SW_AUTH_METHOD_BLOCKED);
                }
                return apdu_sw(resp, 0x63C0 | retries);
            }
            const bool set_ok = (slot == PIN_SLOT_PW1)
                                    ? pin_storage_openpgp_set_pw1_raw(apdu->data + old_len, new_raw)
                                    : pin_storage_openpgp_set_pw3_raw(apdu->data + old_len, new_raw);
            if (!set_ok) {
                return apdu_sw(resp, SW_WRONG_DATA);
            }
            LOG_I(TAG, "%s changed (KDF enable transition)", log_label);
            return apdu_sw(resp, SW_OK);
        }
    }

    if (apdu->lc < min_len * 2) {
        return apdu_sw(resp, SW_WRONG_LENGTH);
    }

    if (try_change_pin(apdu->data, apdu->lc, min_len, slot, change_fn)) {
        LOG_I(TAG, "%s changed successfully", log_label);
        return apdu_sw(resp, SW_OK);
    }

    pin_slot_t slot_for_decrement = slot;
    char dummy_pin[OPENPGP_PIN_MAX_LEN + 1] = {};
    // Trigger a single retry decrement via the regular path to keep the
    // remote counter in sync with the failed CHANGE attempt.
    if (slot_for_decrement == PIN_SLOT_PW1) {
        pin_storage_openpgp_verify_pw1(dummy_pin);
    } else {
        pin_storage_openpgp_verify_pw3(dummy_pin);
    }

    uint8_t retries = retries_fn();
    if (retries == 0) {
        return apdu_sw(resp, SW_AUTH_METHOD_BLOCKED);
    }
    return apdu_sw(resp, 0x63C0 | retries);
}

/**
 * \brief Handles APDU `PSO: COMPUTE DIGITAL SIGNATURE`.
 * \param apdu Parsed APDU request.
 * \param resp Output response buffer.
 * \param resp_max Maximum size of `resp`.
 * \return APDU status/response length result.
 */
static int cmd_pso_cds(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    if (!pw1_verified) {
        return apdu_sw(resp, SW_SECURITY_NOT_SATISFIED);
    }

    if (role_is_rsa[0]) {
        static EXT_RAM_BSS_ATTR uint8_t blob[GPG_RSA_BLOB_MAX];
        size_t blob_len = 0;
        if (!gpg_storage_load_rsa_key(0, blob, sizeof(blob), &blob_len, nullptr)) {
            return apdu_sw(resp, SW_CONDITIONS_NOT_SATISFIED);
        }
        static EXT_RAM_BSS_ATTR uint8_t rsa_sig[GPG_RSA_MAX_MODULUS_BYTES];
        size_t rsa_sig_len = 0;
        bool ok = gpg_rsa_sign(blob, blob_len, apdu->data, apdu->lc,
                               rsa_sig, sizeof(rsa_sig), &rsa_sig_len);
        mbedtls_platform_zeroize(blob, sizeof(blob));
        if (!ok) {
            return apdu_sw(resp, SW_UNKNOWN);
        }
        sig_count++;
        save_state_to_nvs();
        return apdu_build_response(resp, resp_max, rsa_sig, rsa_sig_len, SW_OK);
    }

    // Check if signature key exists by trying to read it
    uint8_t pubkey[P256_PUBKEY_SIZE];
    uint8_t curve;
    if (!se_ecc_key_read(gpg_storage_sig_slot(), pubkey, sizeof(pubkey), &curve)) {
        LOG_E(TAG, "No signature key configured");
        return apdu_sw(resp, SW_CONDITIONS_NOT_SATISFIED);
    }

    // Sign hash directly using TROPIC01
    // P-256 uses ECDSA, Ed25519 uses EdDSA
    uint8_t signature[64];  // R (32 bytes) || S (32 bytes)

    bool success;
    if (curve == CDC_CURVE_P256) {
        if (apdu->lc != SHA256_DIGEST_SIZE) {
            return apdu_sw(resp, SW_WRONG_DATA);
        }
        success = se_ecdsa_sign(gpg_storage_sig_slot(), apdu->data, apdu->lc, signature);
    } else {
        success = se_eddsa_sign(gpg_storage_sig_slot(), apdu->data, apdu->lc, signature);
    }

    if (!success) {
        LOG_E(TAG, "Signature failed");
        return apdu_sw(resp, SW_UNKNOWN);
    }

    // Increment signature counter
    sig_count++;
    save_state_to_nvs();

    LOG_I(TAG, "Signature created, count=%lu", sig_count);
    return apdu_build_response(resp, resp_max, signature, 64, SW_OK);
}

/**
 * \brief Handles APDU `PSO: DECIPHER` for ECDH key agreement.
 * \param apdu Parsed APDU request.
 * \param resp Output response buffer.
 * \param resp_max Maximum size of `resp`.
 * \return APDU status/response length result.
 *
 * \details
 * PSO:DECIPHER - ECDH Decryption.
 *
 * SECURITY NOTE:
 * The TROPIC01 secure element does NOT support native ECDH operations.
 * Therefore, the DEC private key is stored encrypted in R-Memory and
 * temporarily decrypted in RAM for the ECDH computation.
 *
 * This is a necessary trade-off for GPG compatibility.
 * See docs/GPG_ECDH_SECURITY.md for details.
 *
 * Mitigations:
 * - Key is cleared from RAM immediately after use
 * - Encrypted with PIN-derived key (brute-force protected)
 * - MbedTLS uses constant-time ECDH implementation
 *
 * OpenPGP 3.4.1, Section 7.2.11:
 * Command: 00 2A 80 86 <Lc> <data> <Le>
 * Data format for ECDH:
 *   7F49 <len>         -- Cipher DO
 *      A6 <len>        -- External Public Key template
 *         86 <len>     -- External Public Key point (04||X||Y for P-256)
 *            <65 bytes ephemeral pubkey>
 * Response: Shared Secret (32 bytes for P-256)
 */
/**
 * \brief Decrypts an AES-CFB128 payload using the stored symmetric key (DO 0xD5).
 * \param apdu Parsed APDU request, payload[0] = 0x02 padding indicator.
 * \param resp Output response buffer.
 * \param resp_max Maximum size of `resp`.
 * \return APDU status/response length result.
 *
 * Payload format: 0x02 || IV(16) || ciphertext.
 */
static int cmd_pso_decipher_aes(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    if (!gpg_storage_has_aes_key()) {
        return apdu_sw(resp, SW_CONDITIONS_NOT_SATISFIED);
    }
    if (apdu->lc < 1 + 16 + 1) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }
    const uint8_t* iv_in = apdu->data + 1;
    const uint8_t* ct = apdu->data + 1 + 16;
    size_t ct_len = apdu->lc - 1 - 16;
    if (ct_len > resp_max - 2) {
        return apdu_sw(resp, SW_WRONG_LENGTH);
    }

    uint8_t aes_key[32] = {};
    size_t aes_key_len = 0;
    if (!gpg_storage_load_aes_key(aes_key, &aes_key_len, s_session_pin[0] ? s_session_pin : nullptr)) {
        mbedtls_platform_zeroize(aes_key, sizeof(aes_key));
        return apdu_sw(resp, SW_SECURITY_NOT_SATISFIED);
    }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc = mbedtls_aes_setkey_enc(&aes, aes_key, static_cast<unsigned int>(aes_key_len * 8));
    if (rc != 0) {
        mbedtls_aes_free(&aes);
        mbedtls_platform_zeroize(aes_key, sizeof(aes_key));
        return apdu_sw(resp, SW_UNKNOWN);
    }

    uint8_t iv[16];
    memcpy(iv, iv_in, sizeof(iv));
    size_t iv_off = 0;
    uint8_t plain[256];
    if (ct_len > sizeof(plain)) {
        mbedtls_aes_free(&aes);
        mbedtls_platform_zeroize(aes_key, sizeof(aes_key));
        return apdu_sw(resp, SW_WRONG_LENGTH);
    }
    rc = mbedtls_aes_crypt_cfb128(&aes, MBEDTLS_AES_DECRYPT, ct_len, &iv_off, iv, ct, plain);
    mbedtls_aes_free(&aes);
    mbedtls_platform_zeroize(aes_key, sizeof(aes_key));
    mbedtls_platform_zeroize(iv, sizeof(iv));
    if (rc != 0) {
        mbedtls_platform_zeroize(plain, sizeof(plain));
        return apdu_sw(resp, SW_UNKNOWN);
    }
    size_t n = apdu_build_response(resp, resp_max, plain, ct_len, SW_OK);
    mbedtls_platform_zeroize(plain, sizeof(plain));
    return n;
}

static int cmd_pso_decipher(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    if (!pw1_verified) {
        return apdu_sw(resp, SW_SECURITY_NOT_SATISFIED);
    }
    if (apdu->lc < 1) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }

    // OpenPGP 3.4.1 §7.2.11: padding indicator 0x02 = AES decryption.
    if (apdu->data[0] == 0x02) {
        return cmd_pso_decipher_aes(apdu, resp, resp_max);
    }

    if (role_is_rsa[1]) {
        // RSA decipher: data[0] = 0x00 padding indicator, remainder = cryptogram.
        if (apdu->lc < 2) {
            return apdu_sw(resp, SW_WRONG_DATA);
        }
        static EXT_RAM_BSS_ATTR uint8_t blob[GPG_RSA_BLOB_MAX];
        size_t blob_len = 0;
        if (!gpg_storage_load_rsa_key(1, blob, sizeof(blob), &blob_len, nullptr)) {
            return apdu_sw(resp, SW_CONDITIONS_NOT_SATISFIED);
        }
        static EXT_RAM_BSS_ATTR uint8_t pt[GPG_RSA_MAX_MODULUS_BYTES];
        size_t pt_len = 0;
        bool ok = gpg_rsa_decrypt(blob, blob_len, apdu->data + 1, apdu->lc - 1,
                                  pt, sizeof(pt), &pt_len);
        mbedtls_platform_zeroize(blob, sizeof(blob));
        if (!ok) {
            mbedtls_platform_zeroize(pt, sizeof(pt));
            return apdu_sw(resp, SW_UNKNOWN);
        }
        int n = apdu_build_response(resp, resp_max, pt, pt_len, SW_OK);
        mbedtls_platform_zeroize(pt, sizeof(pt));
        return n;
    }

    if (!gpg_storage_has_dec_privkey()) {
        return apdu_sw(resp, SW_CONDITIONS_NOT_SATISFIED);
    }

    // Parse Cipher DO (A6 -> 7F49 -> 86), per OpenPGP 3.4.1 §7.2.11.
    // Minimum: A6 <len1> 7F49 <len2> 86 <len3> <65 bytes pubkey>
    // With single-byte lengths: A6 46 7F49 43 86 41 <65 bytes> = 72 bytes
    if (apdu->lc < 70) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }

    const uint8_t* p = apdu->data;
    const uint8_t* end = apdu->data + apdu->lc;

    if (p >= end || *p != 0xA6) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }
    p++;

    if (p >= end) return apdu_sw(resp, SW_WRONG_DATA);
    if (*p < 0x80) {
        p += 1;
    } else if (*p == 0x81 && p + 1 < end) {
        p += 2;
    } else if (*p == 0x82 && p + 2 < end) {
        p += 3;
    } else {
        return apdu_sw(resp, SW_WRONG_DATA);
    }

    if (p + 2 > end || p[0] != 0x7F || p[1] != 0x49) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }
    p += 2;

    if (p >= end) return apdu_sw(resp, SW_WRONG_DATA);
    if (*p < 0x80) {
        p += 1;
    } else if (*p == 0x81 && p + 1 < end) {
        p += 2;
    } else if (*p == 0x82 && p + 2 < end) {
        p += 3;
    } else {
        return apdu_sw(resp, SW_WRONG_DATA);
    }

    if (p >= end || *p != 0x86) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }
    p++;

    if (p >= end) return apdu_sw(resp, SW_WRONG_DATA);
    size_t pubkey_len;
    if (*p < 0x80) {
        pubkey_len = *p++;
    } else if (*p == 0x81 && p + 1 < end) {
        pubkey_len = p[1];
        p += 2;
    } else if (*p == 0x82 && p + 2 < end) {
        pubkey_len = (static_cast<size_t>(p[1]) << 8) | p[2];
        p += 3;
    } else {
        return apdu_sw(resp, SW_WRONG_DATA);
    }

    if (pubkey_len != P256_PUBKEY_SIZE || p + pubkey_len > end) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }
    if (p[0] != 0x04) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }
    const uint8_t* peer_pubkey = p;

    uint8_t dec_privkey[P256_PRIVKEY_SIZE];
    if (!gpg_storage_load_dec_privkey(dec_privkey, nullptr)) {
        return apdu_sw(resp, SW_SECURITY_NOT_SATISFIED);
    }

    uint8_t shared_secret[P256_ECDH_SECRET_SIZE];
    bool ok = ecdh_p256_compute_shared_secret(dec_privkey, peer_pubkey, shared_secret);
    mbedtls_platform_zeroize(dec_privkey, sizeof(dec_privkey));
    if (!ok) {
        mbedtls_platform_zeroize(shared_secret, sizeof(shared_secret));
        return apdu_sw(resp, SW_UNKNOWN);
    }
    int n = apdu_build_response(resp, resp_max, shared_secret, P256_ECDH_SECRET_SIZE, SW_OK);
    mbedtls_platform_zeroize(shared_secret, sizeof(shared_secret));
    return n;
}

/**
 * \brief Handles APDU MANAGE SECURITY ENVIRONMENT (INS 0x22).
 *
 * Per OpenPGP 3.4.1 §7.2.10: gpg / scd issue MSE before PSO to point the
 * key reference into a different slot. Two combinations matter in
 * practice:
 *   P1=0x41 P2=0xB6 + tag 83 01 01 → use SIG key for DSI (signature)
 *   P1=0x41 P2=0xA4 + tag 83 01 03 → use AUT key for INTERNAL AUTHENTICATE
 *
 * Our key-slot mapping is fixed by role (SIG=B6, DEC=B8, AUT=A4) so MSE is
 * effectively a no-op as long as the requested reference matches the role
 * encoded in P2. We accept the documented combinations and return SW_OK,
 * which is enough for gpg-card / ssh workflows that issue MSE defensively.
 */
static int cmd_manage_security_env(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    (void)resp_max;
    if (apdu->p1 != 0x41) {
        return apdu_sw(resp, SW_INCORRECT_P1P2);
    }
    if (apdu->p2 != KEY_SIG && apdu->p2 != KEY_DEC && apdu->p2 != KEY_AUT) {
        return apdu_sw(resp, SW_INCORRECT_P1P2);
    }
    if (apdu->lc == 0 || apdu->data == nullptr) {
        return apdu_sw(resp, SW_WRONG_LENGTH);
    }
    // Parse the Cryptographic Reference Template: expect 83 01 <ref>.
    if (apdu->lc < 3 || apdu->data[0] != 0x83 || apdu->data[1] != 0x01) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }
    const uint8_t ref = apdu->data[2];
    if (ref != 0x01 && ref != 0x02 && ref != 0x03) {
        return apdu_sw(resp, SW_WRONG_DATA);
    }
    // Cross-check: tag-83 reference must agree with P2 role.
    if ((apdu->p2 == KEY_SIG && ref != 0x01) ||
        (apdu->p2 == KEY_DEC && ref != 0x02) ||
        (apdu->p2 == KEY_AUT && ref != 0x03)) {
        return apdu_sw(resp, SW_CONDITIONS_NOT_SATISFIED);
    }
    return apdu_sw(resp, SW_OK);
}

/**
 * \brief Handles APDU INTERNAL AUTHENTICATE (INS 0x88).
 *
 * Signs the supplied authentication challenge with the AUT key
 * (ECC slot 3). Used by gpg-agent / ssh for client authentication.
 *
 * Per OpenPGP 3.4.1 §7.2.13:
 *   P1=0x00 P2=0x00, Lc = challenge length, Le = signature length.
 *   PW1 (reference 0x82) must be verified; we reuse the PW1 session flag.
 *
 * Signature format matches PSO:CDS: raw R||S for ECDSA, 64-byte EdDSA sig.
 */
static int cmd_internal_authenticate(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    if (apdu->p1 != 0x00 || apdu->p2 != 0x00) {
        return apdu_sw(resp, SW_INCORRECT_P1P2);
    }
    if (!pw1_verified) {
        return apdu_sw(resp, SW_SECURITY_NOT_SATISFIED);
    }
    if (apdu->lc == 0 || apdu->data == nullptr) {
        return apdu_sw(resp, SW_WRONG_LENGTH);
    }

    if (role_is_rsa[2]) {
        static EXT_RAM_BSS_ATTR uint8_t blob[GPG_RSA_BLOB_MAX];
        size_t blob_len = 0;
        if (!gpg_storage_load_rsa_key(2, blob, sizeof(blob), &blob_len, nullptr)) {
            return apdu_sw(resp, SW_CONDITIONS_NOT_SATISFIED);
        }
        static EXT_RAM_BSS_ATTR uint8_t rsa_sig[GPG_RSA_MAX_MODULUS_BYTES];
        size_t rsa_sig_len = 0;
        bool ok = gpg_rsa_sign(blob, blob_len, apdu->data, apdu->lc,
                               rsa_sig, sizeof(rsa_sig), &rsa_sig_len);
        mbedtls_platform_zeroize(blob, sizeof(blob));
        if (!ok) {
            return apdu_sw(resp, SW_UNKNOWN);
        }
        return apdu_build_response(resp, resp_max, rsa_sig, rsa_sig_len, SW_OK);
    }

    uint8_t pubkey[P256_PUBKEY_SIZE];
    uint8_t curve = 0;
    if (!se_ecc_key_read(gpg_storage_aut_slot(), pubkey, sizeof(pubkey), &curve)) {
        LOG_E(TAG, "No AUT key configured");
        return apdu_sw(resp, SW_CONDITIONS_NOT_SATISFIED);
    }

    uint8_t signature[64];
    bool ok = false;
    if (curve == CDC_CURVE_P256) {
        ok = se_ecdsa_sign(gpg_storage_aut_slot(), apdu->data, apdu->lc, signature);
    } else {
        ok = se_eddsa_sign(gpg_storage_aut_slot(), apdu->data, apdu->lc, signature);
    }
    if (!ok) {
        LOG_E(TAG, "INTERNAL AUTHENTICATE: signing failed");
        return apdu_sw(resp, SW_UNKNOWN);
    }
    return apdu_build_response(resp, resp_max, signature, sizeof(signature), SW_OK);
}

/**
 * \brief Handles APDU TERMINATE DF (INS 0xE6).
 *
 * Per OpenPGP 3.4.1 §7.2.18: takes the card into the *terminated* lifecycle
 * state. Allowed under PW3 verification, or unconditionally when both PW1
 * and PW3 retry counters have reached zero (last-resort recovery).
 *
 * After TERMINATE only SELECT and ACTIVATE FILE are accepted; every other
 * command returns SW_FILE_TERMINATED (0x6285).
 */
static int cmd_terminate_df(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    (void)resp_max;
    if (apdu->p1 != 0x00 || apdu->p2 != 0x00) {
        return apdu_sw(resp, SW_INCORRECT_P1P2);
    }
    const bool both_blocked = pin_storage_openpgp_pw1_blocked() &&
                              pin_storage_openpgp_pw3_blocked();
    if (!pw3_verified && !both_blocked) {
        return apdu_sw(resp, SW_SECURITY_NOT_SATISFIED);
    }
    card_terminated = true;
    pw1_verified = false;
    pw3_verified = false;
    save_state_to_nvs();
    LOG_W(TAG, "Card moved to TERMINATED state");
    return apdu_sw(resp, SW_OK);
}

/**
 * \brief Handles APDU ACTIVATE FILE (INS 0x44).
 *
 * Per OpenPGP 3.4.1 §7.2.18: in operational state ACTIVATE FILE is a no-op
 * (SW_OK). In terminated state it wipes every persistent OpenPGP artefact
 * (keys, fingerprints, generation times, cardholder data, PINs) and returns
 * the card to operational state.
 */
static int cmd_activate_file(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    (void)resp_max;
    if (apdu->p1 != 0x00 || apdu->p2 != 0x00) {
        return apdu_sw(resp, SW_INCORRECT_P1P2);
    }
    if (!card_terminated) {
        return apdu_sw(resp, SW_OK);
    }
    openpgp_factory_reset();
    LOG_W(TAG, "ACTIVATE FILE: card reset to factory defaults");
    return apdu_sw(resp, SW_OK);
}

void openpgp_factory_reset(void) {
    auto* se = get_se();
    if (se) {
        se->eccDelete(gpg_storage_sig_slot());
        se->eccDelete(gpg_storage_aut_slot());
    }
    gpg_storage_delete_dec_privkey();
    gpg_storage_delete_rsa_key(0);
    gpg_storage_delete_rsa_key(1);
    gpg_storage_delete_rsa_key(2);
    gpg_storage_clear_session();
    pin_storage_openpgp_reset();
    save_cardholder_cert(nullptr, 0);

    memset(fingerprint_sig, 0, sizeof(fingerprint_sig));
    memset(fingerprint_dec, 0, sizeof(fingerprint_dec));
    memset(fingerprint_aut, 0, sizeof(fingerprint_aut));
    memset(gen_time_sig, 0, sizeof(gen_time_sig));
    memset(gen_time_dec, 0, sizeof(gen_time_dec));
    memset(gen_time_aut, 0, sizeof(gen_time_aut));
    memset(ca_fp_1, 0, sizeof(ca_fp_1));
    memset(ca_fp_2, 0, sizeof(ca_fp_2));
    memset(ca_fp_3, 0, sizeof(ca_fp_3));
    memset(cardholder_name, 0, sizeof(cardholder_name));
    memset(cardholder_url, 0, sizeof(cardholder_url));
    memset(cardholder_login, 0, sizeof(cardholder_login));
    snprintf(cardholder_lang, sizeof(cardholder_lang), "en");
    cardholder_sex = 0x39;
    sig_count = 0;
    selected_curve_sig = CDC_CURVE_ED25519;
    selected_curve_aut = CDC_CURVE_ED25519;
    for (int r = 0; r < 3; ++r) {
        role_is_rsa[r] = false;
        role_rsa_n_bits[r] = 0;
        role_rsa_e_bits[r] = 0;
        role_rsa_fmt[r] = 0;
    }
    kdf_active = false;
    kdf_pin_len = 0;
    kdf_do_len = 0;
    mbedtls_platform_zeroize(kdf_do_bytes, sizeof(kdf_do_bytes));
    mbedtls_platform_zeroize(s_rc_salt, sizeof(s_rc_salt));
    mbedtls_platform_zeroize(s_rc_hash, sizeof(s_rc_hash));
    s_rc_len = 0;
    s_rc_retries = 3;
    pw1_verified = false;
    pw3_verified = false;
    card_terminated = false;
    save_state_to_nvs();
}

/**
 * \brief Handles APDU RESET RETRY COUNTER (INS 0x2C).
 *
 * Per OpenPGP 3.4.1 §7.2.7:
 *   P1=0x02, P2=0x81: reset PW1 with admin authorisation. Lc = new PW1 length.
 *     Requires PW3 verified.
 *   P1=0x00, P2=0x81: reset PW1 using the Resetting Code. Lc = RC || new PW1.
 *     Not yet implemented — returns SW_INS_NOT_SUPPORTED until the RC store
 *     lands (planned in Phase D.3, full path).
 */
static int cmd_reset_retry_counter(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    (void)resp_max;
    if (apdu->p2 != PW1_CODE_1) {
        return apdu_sw(resp, SW_INCORRECT_P1P2);
    }
    if (apdu->p1 != 0x00 && apdu->p1 != 0x02) {
        return apdu_sw(resp, SW_INCORRECT_P1P2);
    }
    if (apdu->p1 == 0x00) {
        // RC path. Lc = RC || new PW1 (concatenated, no length prefix).
        if (s_rc_len == 0) {
            return apdu_sw(resp, SW_CONDITIONS_NOT_SATISFIED);
        }
        if (s_rc_retries == 0) {
            return apdu_sw(resp, SW_AUTH_METHOD_BLOCKED);
        }
        if (apdu->lc < s_rc_len + OPENPGP_PW1_MIN_LEN ||
            apdu->lc - s_rc_len > OPENPGP_PIN_MAX_LEN) {
            return apdu_sw(resp, SW_WRONG_LENGTH);
        }
        uint8_t input_hash[RC_HASH_SIZE];
        if (!compute_rc_hash(apdu->data, s_rc_len, s_rc_salt, input_hash)) {
            return apdu_sw(resp, SW_UNKNOWN);
        }
        uint8_t diff = 0;
        for (size_t i = 0; i < RC_HASH_SIZE; ++i) {
            diff |= static_cast<uint8_t>(s_rc_hash[i] ^ input_hash[i]);
        }
        mbedtls_platform_zeroize(input_hash, sizeof(input_hash));
        if (diff != 0) {
            if (s_rc_retries > 0) s_rc_retries -= 1;
            save_state_to_nvs();
            const uint8_t retries = s_rc_retries;
            if (retries == 0) {
                return apdu_sw(resp, SW_AUTH_METHOD_BLOCKED);
            }
            return apdu_sw(resp, static_cast<uint16_t>(0x63C0 | retries));
        }

        const size_t new_pw1_len = apdu->lc - s_rc_len;
        char new_pin[OPENPGP_PIN_MAX_LEN + 1] = {};
        memcpy(new_pin, apdu->data + s_rc_len, new_pw1_len);
        new_pin[new_pw1_len] = '\0';
        if (!pin_storage_openpgp_change_pw1(new_pin)) {
            mbedtls_platform_zeroize(new_pin, sizeof(new_pin));
            return apdu_sw(resp, SW_UNKNOWN);
        }
        mbedtls_platform_zeroize(new_pin, sizeof(new_pin));
        pin_storage_openpgp_reset_pw1_retries();
        s_rc_retries = 3;
        pw1_verified = false;
        save_state_to_nvs();
        LOG_I(TAG, "RESET RETRY COUNTER: PW1 reset via RC");
        return apdu_sw(resp, SW_OK);
    }
    // P1 == 0x02 — admin-driven reset.
    if (!pw3_verified) {
        return apdu_sw(resp, SW_SECURITY_NOT_SATISFIED);
    }
    // A 32/64-byte new PW1 is a KDF pre-hash, even before kdf_active flips on
    // (gpg's kdf-setup resets PW1 this way prior to PUT DATA 0xF9).
    if (kdf_active || apdu->lc == 32 || apdu->lc == 64) {
        // New PW1 arrives as a PBKDF2 pre-hash.
        if (!pin_storage_openpgp_set_pw1_raw(apdu->data, apdu->lc)) {
            return apdu_sw(resp, SW_WRONG_LENGTH);
        }
        pin_storage_openpgp_reset_pw1_retries();
        pw1_verified = false;
        LOG_I(TAG, "RESET RETRY COUNTER: PW1 reset by admin (KDF)");
        return apdu_sw(resp, SW_OK);
    }
    if (apdu->lc < OPENPGP_PW1_MIN_LEN || apdu->lc > OPENPGP_PIN_MAX_LEN) {
        return apdu_sw(resp, SW_WRONG_LENGTH);
    }
    char new_pin[OPENPGP_PIN_MAX_LEN + 1] = {};
    memcpy(new_pin, apdu->data, apdu->lc);
    new_pin[apdu->lc] = '\0';
    if (!pin_storage_openpgp_change_pw1(new_pin)) {
        mbedtls_platform_zeroize(new_pin, sizeof(new_pin));
        return apdu_sw(resp, SW_UNKNOWN);
    }
    mbedtls_platform_zeroize(new_pin, sizeof(new_pin));
    pin_storage_openpgp_reset_pw1_retries();
    pw1_verified = false;  // force re-verification with new PW1
    LOG_I(TAG, "RESET RETRY COUNTER: PW1 reset by admin");
    return apdu_sw(resp, SW_OK);
}

/**
 * \brief Returns ECC slot mapping for an OpenPGP key reference.
 * \param key_ref OpenPGP key reference value.
 * \return ECC slot index used in secure element storage.
 */
static uint8_t get_ecc_slot_for_key_ref(uint8_t key_ref) {
    switch (key_ref) {
        case KEY_SIG:  // 0xB6 - Signature
            return gpg_storage_sig_slot();
        case KEY_DEC:  // 0xB8 - Decryption
            return gpg_storage_dec_slot();
        case KEY_AUT:  // 0xA4 - Authentication
            return gpg_storage_aut_slot();
        default:
            return gpg_storage_sig_slot();  // Default to SIG
    }
}

/**
 * \brief Maps an OpenPGP key reference to an internal key type.
 * \param key_ref OpenPGP key reference value.
 * \return Internal `key_type_t` for algorithm selection.
 */
static key_type_t get_key_type_for_ref(uint8_t key_ref) {
    switch (key_ref) {
        case KEY_SIG:  return KEY_TYPE_SIG;
        case KEY_DEC:  return KEY_TYPE_DEC;
        case KEY_AUT:  return KEY_TYPE_AUT;
        default:       return KEY_TYPE_SIG;
    }
}

/**
 * \brief Generates a software ECDH P-256 key pair for the DEC slot.
 *
 * The TROPIC01 secure element does not support ECDH, so the DEC private
 * key is generated and persisted by `gpg_storage` (encrypted by device
 * key). Memory holding the raw key is zeroized before returning.
 *
 * \return `SW_OK` on success or an OpenPGP status word on failure.
 */
static uint16_t generate_dec_key(uint8_t *pubkey_out) {
    uint8_t privkey[P256_PRIVKEY_SIZE];
    if (!ecdh_p256_generate_keypair(privkey, pubkey_out)) {
        return SW_UNKNOWN;
    }
    if (!gpg_storage_save_dec_privkey(privkey, nullptr)) {
        mbedtls_platform_zeroize(privkey, sizeof(privkey));
        return SW_UNKNOWN;
    }
    mbedtls_platform_zeroize(privkey, sizeof(privkey));
    return SW_OK;
}

/**
 * \brief Generates a hardware ECC key pair in the TROPIC01 secure element.
 * \param ecc_slot ECC slot index targeted by the key generation.
 * \param curve Curve identifier (`CDC_CURVE_*`).
 * \return `SW_OK` on success or an OpenPGP status word on failure.
 */
static uint16_t generate_hardware_key(uint8_t ecc_slot, uint8_t curve) {
    if (!se_ecc_key_generate(ecc_slot, curve)) {
        LOG_E(TAG, "Key generation failed for slot %d", ecc_slot);
        return SW_UNKNOWN;
    }
    LOG_I(TAG, "Key pair generated in slot %d (hardware)", ecc_slot);
    return SW_OK;
}

/**
 * \brief Updates and persists the generation timestamp for a key role.
 * \param key_ref OpenPGP key reference (`KEY_SIG`/`KEY_DEC`/`KEY_AUT`).
 */
static void update_generation_timestamp(uint8_t key_ref) {
    uint32_t now = (uint32_t)time(NULL);
    uint8_t ts[4] = {
        (uint8_t)((now >> 24) & 0xFF),
        (uint8_t)((now >> 16) & 0xFF),
        (uint8_t)((now >> 8) & 0xFF),
        (uint8_t)(now & 0xFF)
    };

    switch (key_ref) {
        case KEY_SIG: memcpy(gen_time_sig, ts, 4); break;
        case KEY_DEC: memcpy(gen_time_dec, ts, 4); break;
        case KEY_AUT: memcpy(gen_time_aut, ts, 4); break;
        default: break;
    }
    save_state_to_nvs();
}

/**
 * \brief Reads the public key for a given key role.
 *
 * For SIG/AUT keys this reads from the TROPIC01 ECC slot. For DEC keys,
 * the public key is derived from the encrypted software-stored private
 * key; the temporary private-key buffer is zeroized after use.
 *
 * \param key_type Logical key role.
 * \param ecc_slot ECC slot index for hardware-backed keys.
 * \param pubkey Output buffer (must be at least `P256_PUBKEY_SIZE` bytes).
 * \param curve_out Curve detected for the loaded public key.
 * \return `true` on success, otherwise `false`.
 */
static bool read_public_key(key_type_t key_type, uint8_t ecc_slot,
                            uint8_t *pubkey, uint8_t *curve_out) {
    if (key_type == KEY_TYPE_DEC) {
        LOG_I(TAG, "read_public_key DEC: checking has_dec_privkey");
        if (!gpg_storage_has_dec_privkey()) {
            LOG_W(TAG, "read_public_key DEC: no privkey");
            return false;
        }
        LOG_I(TAG, "read_public_key DEC: loading privkey");
        uint8_t privkey[P256_PRIVKEY_SIZE];
        if (!gpg_storage_load_dec_privkey(privkey, nullptr)) {
            LOG_W(TAG, "read_public_key DEC: load_dec_privkey failed");
            return false;
        }
        LOG_I(TAG, "read_public_key DEC: deriving pubkey");
        bool ok = ecdh_p256_derive_pubkey(privkey, pubkey);
        mbedtls_platform_zeroize(privkey, sizeof(privkey));
        if (curve_out) {
            *curve_out = CDC_CURVE_P256;
        }
        LOG_I(TAG, "read_public_key DEC: derive ok=%d", ok);
        return ok;
    }

    return se_ecc_key_read(ecc_slot, pubkey, P256_PUBKEY_SIZE, curve_out);
}

/**
 * \brief Encodes the public key with the OpenPGP/SEC1 uncompressed prefix.
 *
 * P-256 keys are normalized to `0x04 || X || Y`. Ed25519 keys are returned
 * as their raw 32-byte encoding without prefix.
 *
 * \param pubkey Source public key buffer.
 * \param curve Curve identifier (`CDC_CURVE_*`).
 * \param out Destination buffer (must hold at least `P256_PUBKEY_SIZE` bytes).
 * \param out_len Receives the encoded length.
 */
static void encode_pubkey_with_prefix(const uint8_t *pubkey, uint8_t curve,
                                      uint8_t *out, size_t *out_len) {
    if (curve == CDC_CURVE_P256) {
        if (pubkey[0] == 0x04) {
            memcpy(out, pubkey, P256_PUBKEY_SIZE);
        } else {
            out[0] = 0x04;
            memcpy(out + 1, pubkey, P256_PUBKEY_SIZE - 1);
        }
        *out_len = P256_PUBKEY_SIZE;
    } else {
        // Ed25519: raw 32-byte encoding, no SEC1 prefix.
        memcpy(out, pubkey, ED25519_PUBKEY_SIZE);
        *out_len = ED25519_PUBKEY_SIZE;
    }
}

/**
 * \brief Builds the `7F49 { 81 <modulus> 82 <exponent> }` public-key response
 *        for an RSA role from its stored private-key blob.
 * \param r Role index (0 = SIG, 1 = DEC, 2 = AUT).
 * \return APDU status/response length result.
 */
static int build_rsa_pubkey_from_storage(int r, uint8_t *resp, size_t resp_max) {
    static EXT_RAM_BSS_ATTR uint8_t blob[GPG_RSA_BLOB_MAX];
    size_t blob_len = 0;
    if (!gpg_storage_load_rsa_key(static_cast<uint8_t>(r), blob, sizeof(blob), &blob_len, nullptr)) {
        return apdu_sw(resp, SW_REFERENCED_DATA_NOT_FOUND);
    }
    static EXT_RAM_BSS_ATTR uint8_t n_buf[GPG_RSA_MAX_MODULUS_BYTES];
    uint8_t e_buf[8];
    size_t n_len = 0, e_len = 0;
    bool ok = gpg_rsa_blob_public(blob, blob_len, n_buf, sizeof(n_buf), &n_len,
                                  e_buf, sizeof(e_buf), &e_len);
    mbedtls_platform_zeroize(blob, sizeof(blob));
    if (!ok) {
        return apdu_sw(resp, SW_UNKNOWN);
    }
    // Inner public-key DO: 81 <modulus> 82 <exponent>.
    static EXT_RAM_BSS_ATTR uint8_t inner[GPG_RSA_MAX_MODULUS_BYTES + 16];
    size_t inner_len = 0;
    inner_len += tlv_build(inner + inner_len, sizeof(inner) - inner_len, 0x81, n_buf, n_len);
    inner_len += tlv_build(inner + inner_len, sizeof(inner) - inner_len, 0x82, e_buf, e_len);
    // Wrap in 7F49 (Public Key DO).
    static EXT_RAM_BSS_ATTR uint8_t outbuf[GPG_RSA_MAX_MODULUS_BYTES + 32];
    size_t pos = 0;
    pos += tlv_write_tag(outbuf + pos, 0x7F49);
    pos += tlv_write_len(outbuf + pos, inner_len);
    memcpy(outbuf + pos, inner, inner_len);
    pos += inner_len;
    return apdu_build_response(resp, resp_max, outbuf, pos, SW_OK);
}

/**
 * \brief Handles APDU `GENERATE ASYMMETRIC KEY PAIR`.
 * \param apdu Parsed APDU request.
 * \param resp Output response buffer.
 * \param resp_max Maximum size of `resp`.
 * \return APDU status/response length result.
 */
static int cmd_generate_keypair(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    // Parse control reference template (CRT) from data.
    // Format: B6 00 (SIG) / B8 00 (DEC) / A4 00 (AUT)
    uint8_t key_ref = KEY_SIG;  // Default to Signature key

    if (apdu->lc >= 2) {
        key_ref = apdu->data[0];
        LOG_I(TAG, "Key ref from CRT: 0x%02X", key_ref);
    }

    const uint8_t ecc_slot = get_ecc_slot_for_key_ref(key_ref);
    const key_type_t key_type = get_key_type_for_ref(key_ref);

    LOG_I(TAG, "GENERATE_KEYPAIR: P1=0x%02X, key_ref=0x%02X, slot=%d, type=%d",
             apdu->p1, key_ref, ecc_slot, key_type);

    // RSA roles are software keys (slower, less secure than the SE-backed ECC
    // path); generation and public-key read go through the mbedTLS backend.
    const int role_idx = role_index_for_key_ref(key_ref);
    if (role_idx >= 0 && role_is_rsa[role_idx]) {
        if (apdu->p1 == 0x80) {
            if (!pw3_verified) {
                return apdu_sw(resp, SW_SECURITY_NOT_SATISFIED);
            }
            static EXT_RAM_BSS_ATTR uint8_t blob[GPG_RSA_BLOB_MAX];
            size_t blob_len = 0;
            LOG_W(TAG, "Generating RSA-%u key (software fallback, slow) for role %d",
                  role_rsa_n_bits[role_idx], role_idx);
            if (!gpg_rsa_generate(role_rsa_n_bits[role_idx], blob, sizeof(blob), &blob_len)) {
                return apdu_sw(resp, SW_UNKNOWN);
            }
            bool saved = gpg_storage_save_rsa_key(static_cast<uint8_t>(role_idx), blob, blob_len, nullptr);
            mbedtls_platform_zeroize(blob, sizeof(blob));
            if (!saved) {
                return apdu_sw(resp, SW_UNKNOWN);
            }
            update_generation_timestamp(key_ref);
            return build_rsa_pubkey_from_storage(role_idx, resp, resp_max);
        }
        // P1 == 0x81: read existing public key.
        return build_rsa_pubkey_from_storage(role_idx, resp, resp_max);
    }

    if (apdu->p1 == 0x80) {
        // P1=0x80: Generate new key (admin PIN required).
        if (!pw3_verified) {
            return apdu_sw(resp, SW_SECURITY_NOT_SATISFIED);
        }

        // Curve choice follows the configured algorithm attributes:
        // SIG / AUT honour PUT DATA C1 / C3; DEC is fixed to P-256 (the only
        // curve the software-ECDH path supports).
        uint8_t curve = CDC_CURVE_P256;
        if (key_type == KEY_TYPE_SIG) curve = selected_curve_sig;
        else if (key_type == KEY_TYPE_AUT) curve = selected_curve_aut;

        LOG_I(TAG, "Generating key in slot %d (curve=%d, type=%s)",
                 ecc_slot, curve,
                 key_type == KEY_TYPE_SIG ? "SIG" :
                 key_type == KEY_TYPE_DEC ? "DEC" : "AUT");

        uint8_t fresh_pubkey[P256_PUBKEY_SIZE] = {};
        uint16_t gen_sw;
        if (key_type == KEY_TYPE_DEC) {
            gen_sw = generate_dec_key(fresh_pubkey);
        } else {
            gen_sw = generate_hardware_key(ecc_slot, curve);
        }
        if (gen_sw != SW_OK) {
            return apdu_sw(resp, gen_sw);
        }

        update_generation_timestamp(key_ref);

        if (key_type == KEY_TYPE_DEC) {
            uint8_t pubkey_with_prefix[P256_PUBKEY_SIZE];
            size_t pubkey_len = 0;
            encode_pubkey_with_prefix(fresh_pubkey, CDC_CURVE_P256,
                                      pubkey_with_prefix, &pubkey_len);

            uint8_t tlv_data[128];
            size_t pos = tlv_build(tlv_data, sizeof(tlv_data), 0x86,
                                   pubkey_with_prefix, pubkey_len);

            uint8_t final_resp[140];
            size_t final_len = 0;
            final_resp[final_len++] = 0x7F;
            final_resp[final_len++] = 0x49;
            final_len += tlv_write_len(final_resp + final_len, pos);
            memcpy(final_resp + final_len, tlv_data, pos);
            final_len += pos;
            return apdu_build_response(resp, resp_max, final_resp, final_len, SW_OK);
        }
    }

    // Read public key (P1=0x81 read, or after key generation above).
    uint8_t pubkey[P256_PUBKEY_SIZE];
    uint8_t read_curve = CDC_CURVE_P256;
    if (!read_public_key(key_type, ecc_slot, pubkey, &read_curve)) {
        LOG_W(TAG, "Public key read failed: empty slot=%d type=%d", ecc_slot, key_type);
        // Per OpenPGP 3.4.1 §7.2.14 GET DATA / GENERATE ASYMMETRIC KEY PAIR
        // with P1=0x81 on an unpopulated slot must signal "referenced data
        // not found" (SW=6A88). gpg / scdaemon treats 6A88 as "no key yet",
        // which is the natural state for a freshly-activated card and the
        // only way `gpg --card-status` completes without aborting.
        return apdu_sw(resp, SW_REFERENCED_DATA_NOT_FOUND);
    }

    // Build TLV response according to OpenPGP 3.4.1: 7F49 <len> { 86 <len> <pubkey> }.
    // P-256: 0x04 || X || Y (65 bytes); Ed25519: 32 raw bytes.
    uint8_t pubkey_with_prefix[P256_PUBKEY_SIZE];
    size_t pubkey_len = 0;
    encode_pubkey_with_prefix(pubkey, read_curve, pubkey_with_prefix, &pubkey_len);

    uint8_t tlv_data[128];
    size_t pos = 0;
    pos += tlv_build(tlv_data + pos, sizeof(tlv_data) - pos, 0x86, pubkey_with_prefix, pubkey_len);

    // Wrap in 7F49 (Public Key DO).
    uint8_t final_resp[140];
    size_t final_len = 0;
    final_resp[final_len++] = 0x7F;
    final_resp[final_len++] = 0x49;
    final_len += tlv_write_len(final_resp + final_len, pos);
    memcpy(final_resp + final_len, tlv_data, pos);
    final_len += pos;

    LOG_I(TAG, "Public key exported (%zu bytes, curve=%d, slot=%d)",
             pubkey_len, read_curve, ecc_slot);
    return apdu_build_response(resp, resp_max, final_resp, final_len, SW_OK);
}

/**
 * \brief Trim an APDU response to the host-requested Le window.
 *
 * If the dispatcher produced more payload than the host asked for, we keep
 * the head (Le bytes) in \p resp and stash the rest in g_resp_buffer so the
 * host can retrieve it via GET RESPONSE (INS 0xC0). The status word becomes
 * 61xx where xx = remaining payload size (capped at 0xFF, 0x00 means "more
 * than 255 still pending"). Responses that already fit pass through.
 */
static int apply_response_chaining(uint32_t le, uint8_t *resp, size_t resp_max,
                                   int result_len) {
    if (result_len < 2) return result_len;
    const size_t payload_len = static_cast<size_t>(result_len - 2);
    const uint16_t sw = static_cast<uint16_t>((resp[result_len - 2] << 8) |
                                              resp[result_len - 1]);
    // Only chain on successful payloads; error SWs must surface verbatim.
    if (sw != SW_OK) return result_len;
    if (le == 0 || payload_len <= le) {
        return result_len;
    }
    const size_t remainder = payload_len - le;
    if (remainder > sizeof(g_resp_buffer)) {
        LOG_W(TAG, "Response remainder %zu > %zu, truncating", remainder, sizeof(g_resp_buffer));
        return result_len;  // Best-effort: caller gets the truncated head.
    }
    memcpy(g_resp_buffer, resp + le, remainder);
    g_resp_remaining = remainder;
    g_resp_pos = 0;

    if (resp_max < le + 2) return result_len;  // Defensive: caller's buf too small.
    const uint8_t sw2 = (remainder > 0xFF) ? 0x00 : static_cast<uint8_t>(remainder);
    resp[le]     = 0x61;
    resp[le + 1] = sw2;
    return static_cast<int>(le + 2);
}

/**
 * \brief Handles INS GET RESPONSE (0xC0) — drains the chained response buffer.
 */
static int cmd_get_response(const apdu_t *apdu, uint8_t *resp, size_t resp_max) {
    if (apdu->p1 != 0x00 || apdu->p2 != 0x00) {
        return apdu_sw(resp, SW_INCORRECT_P1P2);
    }
    if (g_resp_remaining == 0) {
        return apdu_sw(resp, SW_CONDITIONS_NOT_SATISFIED);
    }
    size_t want = (apdu->le > 0) ? apdu->le : 256;
    if (want > g_resp_remaining) want = g_resp_remaining;
    if (want + 2 > resp_max) want = resp_max - 2;

    memcpy(resp, g_resp_buffer + g_resp_pos, want);
    // Wipe the bytes we just handed out so the PSRAM-backed chain buffer
    // doesn't retain a copy after delivery (PSO:DECIPHER shared secret can
    // end up here when Le forces a chain).
    mbedtls_platform_zeroize(g_resp_buffer + g_resp_pos, want);
    g_resp_pos       += want;
    g_resp_remaining -= want;

    uint16_t sw = SW_OK;
    if (g_resp_remaining > 0) {
        const uint8_t sw2 = (g_resp_remaining > 0xFF) ? 0x00
                                                      : static_cast<uint8_t>(g_resp_remaining);
        sw = static_cast<uint16_t>((0x61 << 8) | sw2);
    } else {
        g_resp_pos = 0;
    }
    resp[want]     = static_cast<uint8_t>((sw >> 8) & 0xFF);
    resp[want + 1] = static_cast<uint8_t>(sw & 0xFF);
    return static_cast<int>(want + 2);
}

int openpgp_process_apdu(const uint8_t *cmd, size_t cmd_len,
                         uint8_t *resp, size_t resp_max) {
    apdu_t apdu;

    if (!apdu_parse(cmd, cmd_len, &apdu)) {
        LOG_E(TAG, "Invalid APDU");
        return apdu_sw(resp, SW_WRONG_LENGTH);
    }

    LOG_D(TAG, "APDU: CLA=%02X INS=%02X P1=%02X P2=%02X Lc=%d",
             apdu.cla, apdu.ins, apdu.p1, apdu.p2, apdu.lc);

    // Any command other than GET RESPONSE invalidates a pending chained payload.
    if (apdu.ins != INS_GET_RESPONSE) {
        g_resp_remaining = 0;
        g_resp_pos = 0;
    }

    // ISO 7816 CLA: bit 4 = chaining. Reject secure messaging and channels >0.
    if ((apdu.cla & ~0x10) != 0x00) {
        chain_reset();
        return apdu_sw(resp, SW_CLA_NOT_SUPPORTED);
    }

    // Command chaining (ISO 7816-4 §5.1.1): accumulate data while the CLA
    // chaining bit is set; dispatch once it clears. Any deviation in
    // INS/P1/P2 mid-chain is a protocol error and the chain is dropped.
    const bool is_chain_block = (apdu.cla & 0x10) != 0;
    if (is_chain_block || g_chain_active) {
        if (!g_chain_active) {
            g_chain_active = true;
            g_chain_ins = apdu.ins;
            g_chain_p1  = apdu.p1;
            g_chain_p2  = apdu.p2;
            g_chain_len = 0;
        } else if (apdu.ins != g_chain_ins ||
                   apdu.p1  != g_chain_p1  ||
                   apdu.p2  != g_chain_p2) {
            chain_reset();
            return apdu_sw(resp, SW_WRONG_DATA);
        }
        if (g_chain_len + apdu.lc > sizeof(g_chain_buffer)) {
            chain_reset();
            return apdu_sw(resp, SW_WRONG_LENGTH);
        }
        if (apdu.lc > 0 && apdu.data != nullptr) {
            memcpy(g_chain_buffer + g_chain_len, apdu.data, apdu.lc);
            g_chain_len += apdu.lc;
        }
        if (is_chain_block) {
            // Intermediate block: ACK and wait for more.
            return apdu_sw(resp, SW_OK);
        }
        // Final block — replace the parsed apdu's payload with the
        // accumulator so the per-command handlers see the full data.
        apdu.data = g_chain_buffer;
        apdu.lc   = static_cast<uint16_t>(g_chain_len);
        chain_reset();
    }

    // SELECT is always allowed, even in TERMINATED state — otherwise the host
    // could not target the application to issue ACTIVATE FILE.
    if (apdu.ins == INS_SELECT) {
        return cmd_select(&apdu, resp, resp_max);
    }

    // All other commands require application to be selected
    if (!app_selected) {
        return apdu_sw(resp, SW_CONDITIONS_NOT_SATISFIED);
    }

    // While terminated, only ACTIVATE FILE is honoured. Per OpenPGP 3.4.1
    // §7.2.18 every other INS must return SW_FILE_TERMINATED (0x6285).
    if (card_terminated && apdu.ins != INS_ACTIVATE) {
        return apdu_sw(resp, SW_FILE_TERMINATED);
    }

    int result_len = 0;
    switch (apdu.ins) {
        case INS_GET_DATA:
            result_len = cmd_get_data(&apdu, resp, resp_max);
            break;

        case INS_PUT_DATA:
            result_len = cmd_put_data(&apdu, resp, resp_max);
            break;

        case INS_PUT_DATA_ODD:
            result_len = cmd_put_data_odd(&apdu, resp, resp_max);
            break;

        case INS_VERIFY:
            result_len = cmd_verify(&apdu, resp, resp_max);
            break;

        case INS_CHANGE_PIN:
            result_len = cmd_change_reference_data(&apdu, resp, resp_max);
            break;

        case INS_RESET_RETRY:
            result_len = cmd_reset_retry_counter(&apdu, resp, resp_max);
            break;

        case INS_PSO:
            if (apdu.p1 == 0x9E && apdu.p2 == 0x9A) {
                result_len = cmd_pso_cds(&apdu, resp, resp_max);
            } else if (apdu.p1 == 0x80 && apdu.p2 == 0x86) {
                result_len = cmd_pso_decipher(&apdu, resp, resp_max);
            } else {
                result_len = apdu_sw(resp, SW_INCORRECT_P1P2);
            }
            break;

        case INS_INTERNAL_AUTH:
            result_len = cmd_internal_authenticate(&apdu, resp, resp_max);
            break;

        case INS_MSE:
            result_len = cmd_manage_security_env(&apdu, resp, resp_max);
            break;

        case INS_GENERATE_KEYPAIR:
            result_len = cmd_generate_keypair(&apdu, resp, resp_max);
            break;

        case INS_GET_CHALLENGE: {
            uint8_t challenge[255];
            size_t len = apdu.le > 0 ? apdu.le : 8;
            if (len > sizeof(challenge)) len = sizeof(challenge);
            se_random_fill(challenge, len);
            result_len = apdu_build_response(resp, resp_max, challenge, len, SW_OK);
            break;
        }

        case INS_GET_VERSION: {
            // Vendor command (pico-openpgp heritage): report the firmware version.
#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif
            const char* ver = APP_VERSION;
            result_len = apdu_build_response(resp, resp_max,
                                             reinterpret_cast<const uint8_t*>(ver),
                                             strlen(ver), SW_OK);
            break;
        }

        case INS_GET_RESPONSE:
            // GET RESPONSE is handled before chaining is applied — it owns
            // the chained buffer directly.
            return cmd_get_response(&apdu, resp, resp_max);

        case INS_TERMINATE:
            result_len = cmd_terminate_df(&apdu, resp, resp_max);
            break;

        case INS_ACTIVATE:
            result_len = cmd_activate_file(&apdu, resp, resp_max);
            break;

        default:
            LOG_W(TAG, "Unknown instruction: 0x%02X", apdu.ins);
            return apdu_sw(resp, SW_INS_NOT_SUPPORTED);
    }

    return apply_response_chaining(apdu.le, resp, resp_max, result_len);
}

/**
 * \brief Applet deselect hook for the smartcard dispatcher.
 *
 * Invoked when another applet gets selected or the (virtual) card is power
 * cycled. Clears every piece of session state: selection, PIN verification,
 * session PIN, command-chaining accumulator, and the GET RESPONSE buffer
 * (which can still hold PSO:DECIPHER plaintext).
 */
static void openpgp_deselect(void) {
    app_selected = false;
    pw1_verified = false;
    pw3_verified = false;
    session_wipe();
    chain_reset();
    mbedtls_platform_zeroize(g_resp_buffer, sizeof(g_resp_buffer));
    g_resp_remaining = 0;
    g_resp_pos = 0;
}

/**
 * \brief Returns the OpenPGP applet descriptor for scard_register_applet().
 *
 * The registered AID is the constant 6-byte OpenPGP RID, matching the
 * prefix check in cmd_select(); the full 16-byte AID is dynamic (serial
 * number from the MAC) and only reported via GET DATA / SELECT.
 */
const scard_applet_t *openpgp_applet(void) {
    static const uint8_t kOpenPgpRid[6] = {0xD2, 0x76, 0x00, 0x01, 0x24, 0x01};
    static const scard_applet_t applet = {
        .name = "openpgp",
        .aid = kOpenPgpRid,
        .aid_len = sizeof(kOpenPgpRid),
        .process_apdu = openpgp_process_apdu,
        .deselect = openpgp_deselect,
    };
    return &applet;
}
