/**
 * \file
 * \brief CTAP2/FIDO2 command processing and ClientPIN implementation.
 */

#include "mod_fido2/ctap2.h"
#include "mod_fido2/cbor_helpers.h"
#include "mod_fido2/fido2.h"
#include "mod_fido2/fido2_storage.h"
#include "mod_fido2/fido2_common.h"
#include "mod_fido2/ctaphid.h"
#include "mod_fido2/u2f.h"
#include "cdc_log.h"
#include "cdc_core/PinManager.h"
#include "cdc_hal/ISecureElement.h"
#include <esp_system.h>
#include <esp_random.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>
#include "cdc_core/pin_storage_c.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_attr.h>
#include <string.h>

using cdc::mod_fido2::sha256;
using cdc::mod_fido2::sha256_str;

static const char* TAG = "CTAP2";
static const char* TAG_PIN = "PIN";

/** \brief Debug configuration flags (overrideable via build flags). */
#ifndef CTAP2_DEBUG
#define CTAP2_DEBUG                 0   // Verbose CBOR/response dumps
#endif
#ifndef CTAP2_DEBUG_COMMANDS
#define CTAP2_DEBUG_COMMANDS        0   // Command logging
#endif

/** \brief Authenticator Attestation GUID for this authenticator model. */
static const uint8_t AAGUID[16] = {
    0xCD, 0xCB, 0xAD, 0x6E,  // "CDCBAD6E"
    0x39, 0xC3,              // 39C3
    0x00, 0x01,              // Version 1
    0xBA, 0xD6, 0xE0, 0x01,  // "BADGE01"
    0x00, 0x00, 0x00, 0x01   // Device type
};

/** \brief Device info strings reported by `authenticatorGetInfo`. */
static const char *INFO_TRANSPORTS[] = {"usb"};

#define USER_PRESENCE_TIMEOUT_MS    30000   // 30 seconds for user to respond

/** \brief Global CTAP2 runtime state. */

static struct {
    bool initialized;
    bool operation_pending;
    bool cancelled;

    // For getNextAssertion
    uint8_t assertion_creds[FIDO2_MAX_CREDENTIALS];
    uint8_t assertion_count;
    uint8_t assertion_index;
    uint8_t assertion_rp_id_hash[32];
    uint8_t assertion_client_data_hash[32];
    bool assertion_up_done;
    bool assertion_include_user;
    bool assertion_appid_used;
} g_ctap2 = {};

/** \brief ClientPIN constants and state for PIN protocol support. */

#define PIN_PROTOCOL_VERSION    2
#define PIN_TOKEN_SIZE          32
#define PIN_RETRIES_MAX         8
#define PIN_UV_RETRIES_MAX      3

/** \brief ClientPIN subcommand identifiers. */
#define PIN_CMD_GET_RETRIES         0x01
#define PIN_CMD_GET_KEY_AGREEMENT   0x02
#define PIN_CMD_SET_PIN             0x03
#define PIN_CMD_CHANGE_PIN          0x04
#define PIN_CMD_GET_PIN_TOKEN       0x05
#define PIN_CMD_GET_PIN_UV_TOKEN    0x09

/** \brief pinUvAuthToken permission flags (CTAP 2.1). */
#define PIN_PERM_MAKE_CREDENTIAL    0x01    // mc
#define PIN_PERM_GET_ASSERTION      0x02    // ga
#define PIN_PERM_CRED_MGMT          0x04    // cm
#define PIN_PERM_BIO_ENROLLMENT     0x08    // be
#define PIN_PERM_LARGE_BLOB_WRITE   0x10    // lbw
#define PIN_PERM_AUTHN_CONFIG       0x20    // acfg

static struct {
    bool initialized;

    // ECDH key pair (generated on init, regenerated on reset)
    mbedtls_ecp_keypair ecdh_key;
    bool ecdh_valid;

    // PIN token (regenerated on each getPinToken)
    uint8_t pin_token[PIN_TOKEN_SIZE];
    bool pin_token_valid;

    // Token permissions (CTAP 2.1) - 0 means all permissions (legacy)
    uint8_t token_permissions;
    uint8_t token_rp_id_hash[32];   // RP restriction (if any)
    bool token_rp_id_set;

    // Retry counters
    uint8_t pin_retries;
    uint8_t uv_retries;
} g_client_pin = {};

/** \brief Credential management constants and enumeration state. */
#define CRED_MGMT_GET_CREDS_METADATA            0x01
#define CRED_MGMT_ENUMERATE_RPS_BEGIN           0x02
#define CRED_MGMT_ENUMERATE_RPS_GET_NEXT        0x03
#define CRED_MGMT_ENUMERATE_CREDS_BEGIN         0x04
#define CRED_MGMT_ENUMERATE_CREDS_GET_NEXT      0x05
#define CRED_MGMT_DELETE_CREDENTIAL             0x06

static struct {
    // RP enumeration state
    uint8_t rp_slots[FIDO2_MAX_CREDENTIALS];    // Slots with unique RPs
    uint8_t rp_count;                            // Number of unique RPs
    uint8_t rp_index;                            // Current enumeration index

    // Credential enumeration state
    uint8_t cred_slots[FIDO2_MAX_CREDENTIALS];  // Slots for current RP
    uint8_t cred_count;                          // Number of credentials for RP
    uint8_t cred_index;                          // Current enumeration index
    uint8_t current_rp_id_hash[32];              // RP being enumerated
} g_cred_mgmt = {};

/**
 * \brief Fills a buffer with cryptographically secure random bytes.
 * \param out Destination buffer.
 * \param len Number of random bytes to generate.
 */
static void secure_random_fill(uint8_t* out, size_t len) {
    auto* se = cdc::hal::getSecureElementInstance();
    if (se && se->isSessionActive() && se->getRandom(out, static_cast<uint16_t>(len))) {
        return;
    }
    esp_fill_random(out, len);
}

static uint8_t build_authenticator_data(
    const uint8_t *rp_id_hash,
    uint8_t flags,
    uint32_t sign_count,
    const uint8_t *attested_cred_data,
    uint16_t attested_cred_len,
    const uint8_t *ext_data,
    uint16_t ext_len,
    uint8_t *out,
    uint16_t *out_len
);

/**
 * \brief mbedTLS RNG callback backed by secure random source.
 * \param ctx Unused context pointer.
 * \param out Destination buffer.
 * \param len Number of random bytes.
 * \return Always returns `0`.
 */
static int ctap2_random(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    // Use TROPIC01 TRNG (with ESP32 fallback)
    secure_random_fill(out, len);
    return 0;
}

/**
 * \brief Builds attested credential data (AAGUID, credential ID, COSE key).
 * \param cred_id Credential ID bytes.
 * \param cred_id_len Length of `cred_id`.
 * \param pubkey Public key bytes.
 * \param curve Public-key curve identifier.
 * \param out Destination buffer.
 * \param out_size Capacity of `out` in bytes.
 * \param out_len Output length of encoded structure.
 * \return `true` on success, otherwise `false`.
 */
static bool ctap2_build_attested_cred(const uint8_t *cred_id,
                                      uint16_t cred_id_len,
                                      const uint8_t *pubkey,
                                      uint8_t curve,
                                      uint8_t *out,
                                      size_t out_size,
                                      uint16_t *out_len) {
    if (!out || !out_len) return false;

    const size_t fixed_prefix = 16 + 2 + cred_id_len;
    if (out_size < fixed_prefix) return false;

    uint16_t off = 0;

    memcpy(out + off, AAGUID, 16);
    off += 16;

    out[off++] = (cred_id_len >> 8) & 0xFF;
    out[off++] = cred_id_len & 0xFF;

    memcpy(out + off, cred_id, cred_id_len);
    off += cred_id_len;

    cbor_writer_t cose_w;
    cbor_writer_init(&cose_w, out + off, out_size - off);
    if (curve == CDC_CURVE_ED25519) {
        cbor_encode_cose_key_ed25519(&cose_w, pubkey);
    } else {
        cbor_encode_cose_key_p256(&cose_w, pubkey, pubkey + 32);
    }
    off += cbor_writer_length(&cose_w);

    *out_len = off;
    return !cbor_writer_error(&cose_w);
}

/**
 * \brief Builds CBOR payload for the `credProtect` extension.
 * \param level Requested credProtect level.
 * \param out Output buffer for CBOR bytes.
 * \param out_size Size of `out` in bytes.
 * \return Encoded CBOR length, or `0` on failure.
 */
static uint16_t ctap2_build_cred_protect_extension(uint8_t level, uint8_t *out, size_t out_size) {
    if (level == 0 || !out || out_size < 20) return 0;
    cbor_writer_t w;
    cbor_writer_init(&w, out, out_size);
    cbor_encode_map(&w, 1);
    cbor_encode_text(&w, "credProtect");
    cbor_encode_uint(&w, level);
    if (cbor_writer_error(&w)) {
        return 0;
    }
    return (uint16_t)cbor_writer_length(&w);
}

/**
 * \brief Builds authenticator data for makeCredential with optional credProtect extension.
 * \param rp_id_hash SHA-256 hash of RP ID.
 * \param attested_cred Encoded attested credential data.
 * \param attested_len Length of `attested_cred`.
 * \param cred_protect Requested credProtect level.
 * \param auth_data Destination buffer for authenticator data.
 * \param auth_data_len Output length.
 * \return `true` on success, otherwise `false`.
 */
static bool ctap2_build_auth_data_for_cred(const uint8_t *rp_id_hash,
                                           const uint8_t *attested_cred,
                                           uint16_t attested_len,
                                           uint8_t cred_protect,
                                           uint8_t *auth_data,
                                           uint16_t *auth_data_len) {
    // Flags: UP=0x01, UV=0x04, AT=0x40, ED=0x80
    uint8_t flags = 0x01 | 0x40;  // UP=1, AT=1
    bool pin_verified = fido2_is_pin_verified();
    LOG_I(TAG, "Building authData: pin_verified=%d, cred_protect=%u", pin_verified, cred_protect);
    if (pin_verified) {
        flags |= 0x04;  // UV=1 when PIN was verified
        LOG_I(TAG, "UV flag SET -> flags=0x%02X", flags);
    }

    // Build credProtect extension if requested
    uint8_t ext_data[32];
    uint16_t ext_len = 0;
    if (cred_protect > 0) {
        ext_len = ctap2_build_cred_protect_extension(cred_protect, ext_data, sizeof(ext_data));
        if (ext_len > 0) {
            LOG_I(TAG, "Including credProtect extension (level=%u, %u bytes)", cred_protect, ext_len);
        }
    }

    return build_authenticator_data(rp_id_hash, flags, 0,
                                    attested_cred, attested_len,
                                    ext_len > 0 ? ext_data : NULL, ext_len,
                                    auth_data, auth_data_len) == CTAP2_OK;
}

/**
 * \brief Builds CBOR payload for `appid` extension in assertions.
 * \param out Output buffer.
 * \param out_size Size of `out` in bytes.
 * \return Encoded CBOR length, or `0` on failure.
 */
static uint16_t ctap2_build_appid_extension(uint8_t *out, size_t out_size) {
    cbor_writer_t w;
    cbor_writer_init(&w, out, out_size);
    cbor_encode_map(&w, 1);
    cbor_encode_text(&w, "appid");
    cbor_encode_bool(&w, true);
    if (cbor_writer_error(&w)) {
        return 0;
    }
    return (uint16_t)cbor_writer_length(&w);
}

/**
 * \brief Builds packed-attestation makeCredential response CBOR payload.
 * \param auth_data Authenticator data bytes.
 * \param auth_data_len Length of `auth_data`.
 * \param sig Attestation signature bytes.
 * \param sig_len Length of `sig`.
 * \param cert Optional attestation certificate.
 * \param cert_len Length of `cert`.
 * \param response Destination CTAP2 response buffer.
 * \param response_len In/out response buffer length.
 * \return CTAP2 status code.
 */
static uint8_t ctap2_build_make_credential_response_packed(const uint8_t *auth_data,
                                                            uint16_t auth_data_len,
                                                            const uint8_t *sig,
                                                            uint8_t sig_len,
                                                            const uint8_t *cert,
                                                            uint16_t cert_len,
                                                            uint8_t *response,
                                                            uint16_t *response_len) {
    cbor_writer_t w;
    cbor_writer_init(&w, response + 1, *response_len - 1);

    cbor_encode_map(&w, 3);

    // fmt
    cbor_encode_uint(&w, CTAP2_MC_RESP_FMT);
    if (sig_len == 0 && (cert == NULL || cert_len == 0)) {
        // None attestation
        cbor_encode_text(&w, "none");
    } else {
        cbor_encode_text(&w, "packed");
    }

    // authData
    cbor_encode_uint(&w, CTAP2_MC_RESP_AUTH_DATA);
    cbor_encode_bytes(&w, auth_data, auth_data_len);

    // attStmt
    cbor_encode_uint(&w, CTAP2_MC_RESP_ATT_STMT);
    if (sig_len == 0 && (cert == NULL || cert_len == 0)) {
        // None attestation - empty map
        cbor_encode_map(&w, 0);
    } else if (cert && cert_len > 0) {
        // Basic attestation with certificate
        cbor_encode_map(&w, 3);
        cbor_encode_text(&w, "alg");
        cbor_encode_int(&w, COSE_ALG_ES256);
        cbor_encode_text(&w, "sig");
        cbor_encode_bytes(&w, sig, sig_len);
        cbor_encode_text(&w, "x5c");
        cbor_encode_array(&w, 1);  // Array with single certificate
        cbor_encode_bytes(&w, cert, cert_len);
    } else {
        // Self attestation (no certificate)
        cbor_encode_map(&w, 2);
        cbor_encode_text(&w, "alg");
        cbor_encode_int(&w, COSE_ALG_ES256);
        cbor_encode_text(&w, "sig");
        cbor_encode_bytes(&w, sig, sig_len);
    }

    if (cbor_writer_error(&w)) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    response[0] = CTAP2_OK;
    *response_len = 1 + cbor_writer_length(&w);
    return CTAP2_OK;
}

/**
 * \brief Generates ephemeral P-256 key pair and exports 64-byte `X||Y` public key.
 * \param key Destination keypair structure.
 * \param pubkey Destination buffer for public key coordinates.
 * \return `true` on success, otherwise `false`.
 */
static bool ctap2_generate_ephemeral_keypair(mbedtls_ecp_keypair *key, uint8_t pubkey[64]) {
    if (!key || !pubkey) return false;
    mbedtls_ecp_keypair_init(key);

    int rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, key, ctap2_random, NULL);
    if (rc != 0) {
        mbedtls_ecp_keypair_free(key);
        return false;
    }

#if defined(MBEDTLS_PRIVATE)
#define CTAP2_ECP_GRP(k) (k).MBEDTLS_PRIVATE(grp)
#define CTAP2_ECP_Q(k)   (k).MBEDTLS_PRIVATE(Q)
#else
#define CTAP2_ECP_GRP(k) (k).grp
#define CTAP2_ECP_Q(k)   (k).Q
#endif

    uint8_t buf[65];
    size_t olen = 0;
    rc = mbedtls_ecp_point_write_binary(&CTAP2_ECP_GRP((*key)), &CTAP2_ECP_Q((*key)),
                                        MBEDTLS_ECP_PF_UNCOMPRESSED,
                                        &olen, buf, sizeof(buf));
#undef CTAP2_ECP_GRP
#undef CTAP2_ECP_Q
    if (rc != 0 || olen != sizeof(buf)) {
        mbedtls_ecp_keypair_free(key);
        return false;
    }
    memcpy(pubkey, buf + 1, 64);

    return true;
}

/**
 * \brief Signs message using provided keypair (ECDSA over SHA-256).
 * \param key Keypair for signing.
 * \param msg Message bytes to hash and sign.
 * \param msg_len Length of `msg`.
 * \param sig Destination signature buffer.
 * \param sig_size Size of `sig`.
 * \param sig_len Output signature length.
 * \return `true` on success, otherwise `false`.
 */
static bool ctap2_sign_with_keypair(mbedtls_ecp_keypair *key,
                                    const uint8_t *msg, size_t msg_len,
                                    uint8_t *sig, size_t sig_size, size_t *sig_len) {
    if (!key || !msg || !sig || !sig_len) return false;
    uint8_t hash[32];
    sha256(msg, msg_len, hash);

    mbedtls_ecdsa_context ecdsa;
    mbedtls_ecdsa_init(&ecdsa);
    int rc = mbedtls_ecdsa_from_keypair(&ecdsa, key);
    if (rc != 0) {
        mbedtls_ecdsa_free(&ecdsa);
        return false;
    }

    rc = mbedtls_ecdsa_write_signature(&ecdsa, MBEDTLS_MD_SHA256,
                                       hash, sizeof(hash),
                                       sig, sig_size, sig_len,
                                       ctap2_random, NULL);
    mbedtls_ecdsa_free(&ecdsa);

    return rc == 0;
}

/**
 * \brief Builds raw authenticatorData structure.
 * \param rp_id_hash SHA-256 hash of RP ID.
 * \param flags Authenticator data flags.
 * \param sign_count Signature counter value.
 * \param attested_cred_data Optional attested credential block.
 * \param attested_cred_len Length of `attested_cred_data`.
 * \param ext_data Optional extension CBOR bytes.
 * \param ext_len Length of `ext_data`.
 * \param out Destination buffer.
 * \param out_len Output length.
 * \return CTAP2 status code.
 */
static uint8_t build_authenticator_data(
    const uint8_t *rp_id_hash,
    uint8_t flags,
    uint32_t sign_count,
    const uint8_t *attested_cred_data,
    uint16_t attested_cred_len,
    const uint8_t *ext_data,
    uint16_t ext_len,
    uint8_t *out,
    uint16_t *out_len
) {
    uint16_t offset = 0;

    // RP ID hash (32 bytes)
    memcpy(out + offset, rp_id_hash, 32);
    offset += 32;

    // Flags (1 byte)
    if (ext_data && ext_len > 0) {
        flags |= 0x80;  // ED
    }
    out[offset++] = flags;

    // Sign count (4 bytes, big endian)
    out[offset++] = (sign_count >> 24) & 0xFF;
    out[offset++] = (sign_count >> 16) & 0xFF;
    out[offset++] = (sign_count >> 8) & 0xFF;
    out[offset++] = sign_count & 0xFF;

    // Attested credential data (if present)
    if (attested_cred_data && attested_cred_len > 0) {
        memcpy(out + offset, attested_cred_data, attested_cred_len);
        offset += attested_cred_len;
    }

    if (ext_data && ext_len > 0) {
        memcpy(out + offset, ext_data, ext_len);
        offset += ext_len;
    }

    *out_len = offset;
    return CTAP2_OK;
}

/**
 * \brief Requests user-presence confirmation through platform callback.
 * \param rp_id RP ID shown to user.
 * \param action Requested user action type.
 * \param user_name Optional user name shown for registration.
 * \return `true` when approved, otherwise `false`.
 */
static bool wait_for_user_presence(const char *rp_id, fido2_action_t action, const char *user_name) {
    LOG_I(TAG, "User presence required for %s at %s",
          action == FIDO2_ACTION_REGISTER ? "registration" : "authentication",
          rp_id ? rp_id : "unknown");

    // Send keepalive to signal user presence is needed
    uint32_t cid = ctaphid_get_current_cid();
    ctaphid_send_keepalive(cid, CTAPHID_STATUS_UPNEEDED);

    // Request user presence via callback
    fido2_user_presence_result_t result = fido2_request_user_presence(rp_id, action, user_name);

    switch (result) {
        case FIDO2_UP_APPROVED:
            LOG_I(TAG, "User presence approved");
            return true;
        case FIDO2_UP_DENIED:
            LOG_I(TAG, "User presence denied");
            return false;
        case FIDO2_UP_TIMEOUT:
            LOG_I(TAG, "User presence timeout");
            return false;
        default:
            LOG_W(TAG, "User presence unknown state: %d", result);
            return false;
    }
}

/** \brief Reported maximum message size for `authenticatorGetInfo`. */
static constexpr uint64_t CTAP2_INFO_MAX_MSG_SIZE_VALUE = 1200;
/** \brief Reported PIN/UV auth protocol version (Protocol Two). */
static constexpr uint64_t CTAP2_INFO_PIN_UV_AUTH_PROTOCOL_VALUE = 2;
/** \brief Reported maxCredentialCountInList for `authenticatorGetInfo`. */
static constexpr uint64_t CTAP2_INFO_MAX_CRED_LIST_COUNT_VALUE = 8;

/** \brief Encodes the supported FIDO/U2F versions into the getInfo CBOR map. */
static void encode_info_versions(cbor_writer_t *w) {
    cbor_encode_uint(w, CTAP2_INFO_VERSIONS);
    cbor_encode_array(w, 3);
    cbor_encode_text(w, "FIDO_2_0");
    cbor_encode_text(w, "FIDO_2_1");
    cbor_encode_text(w, "U2F_V2");
}

/** \brief Encodes the supported CTAP extensions, sorted for CBOR canonical form. */
static void encode_info_extensions(cbor_writer_t *w) {
    cbor_encode_uint(w, CTAP2_INFO_EXTENSIONS);
    cbor_encode_array(w, 3);
    cbor_encode_text(w, "appid");          // 5 chars
    cbor_encode_text(w, "credProtect");    // 11 chars - required for resident keys
    cbor_encode_text(w, "appidExclude");   // 12 chars
}

/** \brief Encodes the authenticator AAGUID into the getInfo CBOR map. */
static void encode_info_aaguid(cbor_writer_t *w) {
    cbor_encode_uint(w, CTAP2_INFO_AAGUID);
    cbor_encode_bytes(w, AAGUID, 16);
}

/** \brief Encodes the supported authenticator options, keys sorted by length. */
static void encode_info_options(cbor_writer_t *w) {
    cbor_encode_uint(w, CTAP2_INFO_OPTIONS);
    cbor_encode_map(w, 7);
    cbor_encode_text(w, "rk");              // 2 chars
    cbor_encode_bool(w, true);
    cbor_encode_text(w, "up");              // 2 chars
    cbor_encode_bool(w, true);
    cbor_encode_text(w, "uv");              // 2 chars
    cbor_encode_bool(w, false);
    cbor_encode_text(w, "plat");            // 4 chars
    cbor_encode_bool(w, false);
    cbor_encode_text(w, "credMgmt");        // 8 chars
    cbor_encode_bool(w, true);
    cbor_encode_text(w, "clientPin");       // 9 chars
    cbor_encode_bool(w, true);
    cbor_encode_text(w, "pinUvAuthToken");  // 14 chars
    cbor_encode_bool(w, true);
}

/** \brief Encodes the maxMsgSize entry into the getInfo CBOR map. */
static void encode_info_max_msg_size(cbor_writer_t *w) {
    cbor_encode_uint(w, CTAP2_INFO_MAX_MSG_SIZE);
    cbor_encode_uint(w, CTAP2_INFO_MAX_MSG_SIZE_VALUE);
}

/** \brief Encodes the supported pinUvAuthProtocols list. */
static void encode_info_pin_uv_auth_protocols(cbor_writer_t *w) {
    cbor_encode_uint(w, CTAP2_INFO_PIN_UV_AUTH_PROTOCOLS);
    cbor_encode_array(w, 1);
    cbor_encode_uint(w, CTAP2_INFO_PIN_UV_AUTH_PROTOCOL_VALUE);
}

/** \brief Encodes the maxCredentialCountInList entry. */
static void encode_info_max_cred_count(cbor_writer_t *w) {
    cbor_encode_uint(w, CTAP2_INFO_MAX_CRED_COUNT_IN_LIST);
    cbor_encode_uint(w, CTAP2_INFO_MAX_CRED_LIST_COUNT_VALUE);
}

/** \brief Encodes the maxCredentialIdLength entry. */
static void encode_info_max_cred_id_length(cbor_writer_t *w) {
    cbor_encode_uint(w, CTAP2_INFO_MAX_CRED_ID_LENGTH);
    cbor_encode_uint(w, FIDO2_CRED_ID_LEN);
}

/** \brief Encodes the supported transports list. */
static void encode_info_transports(cbor_writer_t *w) {
    cbor_encode_uint(w, CTAP2_INFO_TRANSPORTS);
    cbor_encode_array(w, 1);
    cbor_encode_text(w, INFO_TRANSPORTS[0]);
}

/** \brief Encodes the supported algorithms array (PublicKeyCredentialParameters). */
static void encode_info_algorithms(cbor_writer_t *w) {
    cbor_encode_uint(w, CTAP2_INFO_ALGORITHMS);
    cbor_encode_array(w, 2);
    // ES256 (P-256/ECDSA) - keys sorted by length: "alg" (3) < "type" (4)
    cbor_encode_map(w, 2);
    cbor_encode_text(w, "alg");
    cbor_encode_int(w, COSE_ALG_ES256);
    cbor_encode_text(w, "type");
    cbor_encode_text(w, "public-key");
    // EdDSA (Ed25519)
    cbor_encode_map(w, 2);
    cbor_encode_text(w, "alg");
    cbor_encode_int(w, COSE_ALG_EDDSA);
    cbor_encode_text(w, "type");
    cbor_encode_text(w, "public-key");
}

#if CTAP2_DEBUG
/** \brief Hex-dumps a getInfo response buffer to the debug log. */
static void dump_get_info_response(const uint8_t *response, uint16_t len) {
    LOG_I(TAG, "getInfo response len=%u", len);
    for (uint16_t offset = 0; offset < len; offset += 16) {
        char hex[50] = {0};
        int dump_len = ((len - offset) < 16) ? (len - offset) : 16;
        for (int i = 0; i < dump_len; i++) {
            sprintf(hex + (i * 3), "%02X ", response[offset + i]);
        }
        LOG_D(TAG, "%03u: %s", offset, hex);
    }
}
#endif

/**
 * \brief Handles CTAP2 `authenticatorGetInfo` (`0x04`).
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
uint8_t ctap2_get_info(uint8_t *response, uint16_t *response_len) {
    cbor_writer_t w;
    cbor_writer_init(&w, response + 1, *response_len - 1);

    // Response is a map of 10 entries (CTAP 2.1 getInfo)
    cbor_encode_map(&w, 10);
    encode_info_versions(&w);
    encode_info_extensions(&w);
    encode_info_aaguid(&w);
    encode_info_options(&w);
    encode_info_max_msg_size(&w);
    encode_info_pin_uv_auth_protocols(&w);
    encode_info_max_cred_count(&w);
    encode_info_max_cred_id_length(&w);
    encode_info_transports(&w);
    encode_info_algorithms(&w);

    if (cbor_writer_error(&w)) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    response[0] = CTAP2_OK;
    *response_len = 1 + cbor_writer_length(&w);

#if CTAP2_DEBUG
    dump_get_info_response(response, *response_len);
#endif

    return CTAP2_OK;
}

#ifdef __DOXYGEN__
namespace cdc::mod_fido2 {
#endif

/** \brief Parsed parameters for `authenticatorMakeCredential`. */
struct MakeCredentialParams {
    uint8_t client_data_hash[32];
    char rp_id[FIDO2_RP_ID_MAX_LEN];
    uint8_t rp_id_hash[32];
    uint8_t user_id[FIDO2_USER_ID_MAX_LEN];
    uint8_t user_id_len;
    char user_name[FIDO2_USER_NAME_MAX_LEN];
    bool rk;
    uint8_t cred_protect;
    int alg;
    bool option_uv;
    bool option_up;
    char appid_exclude[256];
    bool has_appid_exclude;
    uint8_t pin_uv_auth_param[64];
    size_t pin_uv_auth_param_len;
    uint8_t pin_uv_auth_protocol;
    bool has_client_data;
    bool has_rp;
    bool has_user;
    bool has_alg;

    void clear() {
        memset(this, 0, sizeof(*this));
        option_up = true;  // Default: UP required
    }
};

/**
 * \brief Parses the RP map from a makeCredential CBOR request.
 * \param r CBOR reader positioned at the RP map.
 * \param p Output parameter structure to fill.
 * \return `true` on success, otherwise `false`.
 */
static bool parse_rp_map(cbor_reader_t *r, MakeCredentialParams *p) {
    int rp_count = cbor_read_map(r);
    if (rp_count < 0) return false;

    for (int j = 0; j < rp_count; j++) {
        char rp_key[16];
        size_t key_len;
        if (!cbor_read_text(r, rp_key, sizeof(rp_key), &key_len)) {
            cbor_skip_item(r);
            continue;
        }
        if (strcmp(rp_key, "id") == 0) {
            size_t id_len;
            cbor_read_text(r, p->rp_id, sizeof(p->rp_id), &id_len);
            sha256_str(p->rp_id, p->rp_id_hash);
            p->has_rp = true;
        } else {
            cbor_skip_item(r);
        }
    }
    return true;
}

/**
 * \brief Parses the user map from a makeCredential CBOR request.
 * \param r CBOR reader positioned at the user map.
 * \param p Output parameter structure to fill.
 * \return `true` on success, otherwise `false`.
 */
static bool parse_user_map(cbor_reader_t *r, MakeCredentialParams *p) {
    int user_count = cbor_read_map(r);
    if (user_count < 0) return false;

    for (int j = 0; j < user_count; j++) {
        char user_key[16];
        size_t key_len;
        if (!cbor_read_text(r, user_key, sizeof(user_key), &key_len)) {
            cbor_skip_item(r);
            continue;
        }
        if (strcmp(user_key, "id") == 0) {
            size_t id_len;
            cbor_read_bytes(r, p->user_id, sizeof(p->user_id), &id_len);
            p->user_id_len = id_len;
            p->has_user = true;
        } else if (strcmp(user_key, "name") == 0) {
            size_t name_len;
            cbor_read_text(r, p->user_name, sizeof(p->user_name), &name_len);
        } else {
            cbor_skip_item(r);
        }
    }
    return true;
}

/**
 * \brief Parses `pubKeyCredParams` and selects a supported algorithm.
 * \param r CBOR reader positioned at the params array.
 * \param p Output parameter structure to fill.
 * \return `true` on success, otherwise `false`.
 */
static bool parse_pubkey_cred_params(cbor_reader_t *r, MakeCredentialParams *p) {
    int params_count = cbor_read_array(r);
    if (params_count < 0) return false;

    for (int j = 0; j < params_count; j++) {
        int param_count = cbor_read_map(r);
        int64_t param_alg = 0;
        for (int k = 0; k < param_count; k++) {
            char param_key[8];
            size_t key_len;
            if (!cbor_read_text(r, param_key, sizeof(param_key), &key_len)) {
                cbor_skip_item(r);
                continue;
            }
            if (strcmp(param_key, "alg") == 0) {
                cbor_read_int(r, &param_alg);
            } else {
                cbor_skip_item(r);
            }
        }
        // We support ES256 (P-256/ECDSA) and EdDSA (Ed25519)
        if (!p->has_alg && (param_alg == COSE_ALG_ES256 || param_alg == COSE_ALG_EDDSA)) {
            p->alg = param_alg;
            p->has_alg = true;
        }
    }
    return true;
}

/**
 * \brief Parses makeCredential extensions map from CBOR.
 * \param r CBOR reader positioned at the extensions map.
 * \param p Output parameter structure to fill.
 * \return `true` on success, otherwise `false`.
 */
static bool parse_extensions_map(cbor_reader_t *r, MakeCredentialParams *p) {
    int ext_count = cbor_read_map(r);
    if (ext_count < 0) return false;

    for (int j = 0; j < ext_count; j++) {
        char ext_key[16];
        size_t key_len;
        if (!cbor_read_text(r, ext_key, sizeof(ext_key), &key_len)) {
            cbor_skip_item(r);
            continue;
        }
        if (strcmp(ext_key, "appidExclude") == 0) {
            size_t len;
            if (cbor_read_text(r, p->appid_exclude, sizeof(p->appid_exclude), &len)) {
                p->has_appid_exclude = (len > 0);
            }
        } else if (strcmp(ext_key, "credProtect") == 0) {
            uint64_t level;
            if (cbor_read_uint(r, &level) && level >= 1 && level <= 3) {
                p->cred_protect = (uint8_t)level;
                LOG_I(TAG, "credProtect requested: level=%u", p->cred_protect);
            }
        } else {
            cbor_skip_item(r);
        }
    }
    return true;
}

/**
 * \brief Parses makeCredential options map from CBOR.
 * \param r CBOR reader positioned at the options map.
 * \param p Output parameter structure to fill.
 * \return `true` on success, otherwise `false`.
 */
static bool parse_options_map(cbor_reader_t *r, MakeCredentialParams *p) {
    int opt_count = cbor_read_map(r);
    if (opt_count < 0) return false;

    for (int j = 0; j < opt_count; j++) {
        char opt_key[8];
        size_t key_len;
        if (!cbor_read_text(r, opt_key, sizeof(opt_key), &key_len)) {
            cbor_skip_item(r);
            continue;
        }
        if (strcmp(opt_key, "rk") == 0) {
            cbor_read_bool(r, &p->rk);
        } else if (strcmp(opt_key, "uv") == 0) {
            cbor_read_bool(r, &p->option_uv);
        } else if (strcmp(opt_key, "up") == 0) {
            cbor_read_bool(r, &p->option_up);
        } else {
            cbor_skip_item(r);
        }
    }
    return true;
}

/**
 * \brief Parses complete makeCredential request map from CBOR payload.
 * \param data CBOR request payload.
 * \param data_len Length of `data`.
 * \param p Output parameter structure.
 * \return CTAP2 status code.
 */
static uint8_t parse_make_credential_params(const uint8_t *data, uint16_t data_len,
                                            MakeCredentialParams *p) {
    cbor_reader_t r;
    cbor_reader_init(&r, data, data_len);
    p->clear();

    int map_count = cbor_read_map(&r);
    if (map_count < 0) {
        return CTAP2_ERR_INVALID_CBOR;
    }

    for (int i = 0; i < map_count; i++) {
        uint64_t key;
        if (!cbor_read_uint(&r, &key)) {
            return CTAP2_ERR_INVALID_CBOR;
        }

        switch (key) {
            case CTAP2_MC_CLIENT_DATA_HASH: {
                size_t len;
                if (!cbor_read_bytes(&r, p->client_data_hash, 32, &len) || len != 32) {
                    return CTAP2_ERR_INVALID_CBOR;
                }
                p->has_client_data = true;
                break;
            }
            case CTAP2_MC_RP:
                if (!parse_rp_map(&r, p)) return CTAP2_ERR_INVALID_CBOR;
                break;
            case CTAP2_MC_USER:
                if (!parse_user_map(&r, p)) return CTAP2_ERR_INVALID_CBOR;
                break;
            case CTAP2_MC_PUB_KEY_CRED_PARAMS:
                if (!parse_pubkey_cred_params(&r, p)) return CTAP2_ERR_INVALID_CBOR;
                break;
            case CTAP2_MC_EXTENSIONS:
                if (!parse_extensions_map(&r, p)) return CTAP2_ERR_INVALID_CBOR;
                break;
            case CTAP2_MC_OPTIONS:
                if (!parse_options_map(&r, p)) return CTAP2_ERR_INVALID_CBOR;
                break;
            case CTAP2_MC_PIN_UV_AUTH_PARAM:
                cbor_read_bytes(&r, p->pin_uv_auth_param, sizeof(p->pin_uv_auth_param),
                               &p->pin_uv_auth_param_len);
                break;
            case CTAP2_MC_PIN_UV_AUTH_PROTOCOL: {
                uint64_t proto;
                if (cbor_read_uint(&r, &proto)) {
                    p->pin_uv_auth_protocol = (uint8_t)proto;
                }
                break;
            }
            default:
                cbor_skip_item(&r);
                break;
        }
    }

    // Validate required parameters
    if (!p->has_client_data || !p->has_rp || !p->has_user || !p->has_alg) {
        return CTAP2_ERR_MISSING_PARAMETER;
    }

    return CTAP2_OK;
}

/**
 * \brief Verifies `pinUvAuthParam` for makeCredential.
 * \param p Parsed makeCredential parameters.
 * \return CTAP2 status code.
 */
static uint8_t verify_pin_uv_auth(const MakeCredentialParams *p) {
    LOG_D(TAG, "pinToken valid=%d", g_client_pin.pin_token_valid);

    if (p->pin_uv_auth_param_len == 0) {
        return CTAP2_OK;  // No auth param provided, skip verification
    }

    if (!g_client_pin.pin_token_valid) {
        LOG_W(TAG, "makeCredential: pinUvAuthParam provided but no valid pinToken");
        return CTAP2_ERR_PIN_AUTH_INVALID;
    }

    // Verify HMAC-SHA-256(pinToken, clientDataHash)
    uint8_t expected_hmac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    g_client_pin.pin_token, sizeof(g_client_pin.pin_token),
                    p->client_data_hash, 32,
                    expected_hmac);

    // Protocol 2 uses first 32 bytes of HMAC
    size_t compare_len = (p->pin_uv_auth_protocol == 2) ? 32 : 16;
    if (p->pin_uv_auth_param_len < compare_len ||
        memcmp(p->pin_uv_auth_param, expected_hmac, compare_len) != 0) {
        LOG_W(TAG, "makeCredential: pinUvAuthParam verification failed");
        return CTAP2_ERR_PIN_AUTH_INVALID;
    }

    LOG_I(TAG, "makeCredential: pinUvAuthParam verified - UV=1");
    fido2_set_pin_verified(true);
    return CTAP2_OK;
}

/**
 * \brief Validates the `appidExclude` extension against existing credentials.
 * \param p Parsed makeCredential parameters.
 * \return CTAP2 status code.
 */
static uint8_t check_appid_exclude(const MakeCredentialParams *p) {
    if (!p->has_appid_exclude || p->appid_exclude[0] == '\0') {
        return CTAP2_OK;
    }

    uint8_t appid_hash[32];
    sha256_str(p->appid_exclude, appid_hash);
    if (fido2_storage_find_by_rp(appid_hash, g_ctap2.assertion_creds, FIDO2_MAX_CREDENTIALS) > 0) {
        return CTAP2_ERR_CREDENTIAL_EXCLUDED;
    }
    return CTAP2_OK;
}

/**
 * \brief Handles browser probe RP IDs by returning a synthetic attested response.
 * \param p Parsed makeCredential parameters.
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
static uint8_t handle_browser_probe(const MakeCredentialParams *p,
                                    uint8_t *response, uint16_t *response_len) {
    LOG_I(TAG, "Browser probe request (%s) - waiting for user selection", p->rp_id);

    // Wait for user to confirm this authenticator (no PIN needed)
    if (!wait_for_user_presence(p->rp_id, FIDO2_ACTION_SELECT, NULL)) {
        response[0] = CTAP2_ERR_OPERATION_DENIED;
        *response_len = 1;
        return CTAP2_ERR_OPERATION_DENIED;
    }

    // Generate dummy credential for probe response
    uint8_t dummy_pubkey[64];
    uint8_t dummy_cred_id[FIDO2_CRED_ID_LEN];
    if (ctap2_random(NULL, dummy_cred_id, sizeof(dummy_cred_id)) != 0) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    uint8_t attested_cred[256];
    uint16_t attested_len = 0;
    uint8_t auth_data[256];
    uint16_t auth_data_len = 0;

    // Large buffer in PSRAM to save stack space
    EXT_RAM_BSS_ATTR static uint8_t to_sign[512];
    uint8_t signature[128];
    size_t sig_len = 0;

    mbedtls_ecp_keypair ephemeral_key;
    if (!ctap2_generate_ephemeral_keypair(&ephemeral_key, dummy_pubkey)) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    if (!ctap2_build_attested_cred(dummy_cred_id, FIDO2_CRED_ID_LEN, dummy_pubkey,
                                   CDC_CURVE_P256, attested_cred, sizeof(attested_cred),
                                   &attested_len)) {
        mbedtls_ecp_keypair_free(&ephemeral_key);
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    if (!ctap2_build_auth_data_for_cred(p->rp_id_hash, attested_cred, attested_len,
                                        0, auth_data, &auth_data_len)) {
        mbedtls_ecp_keypair_free(&ephemeral_key);
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    if (auth_data_len + 32 > sizeof(to_sign)) {
        mbedtls_ecp_keypair_free(&ephemeral_key);
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    memcpy(to_sign, auth_data, auth_data_len);
    memcpy(to_sign + auth_data_len, p->client_data_hash, 32);
    uint16_t to_sign_len = auth_data_len + 32;

    if (!ctap2_sign_with_keypair(&ephemeral_key, to_sign, to_sign_len,
                                 signature, sizeof(signature), &sig_len)) {
        mbedtls_ecp_keypair_free(&ephemeral_key);
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }
    mbedtls_ecp_keypair_free(&ephemeral_key);

    LOG_I(TAG, "User selected this authenticator");
    uint8_t status = ctap2_build_make_credential_response_packed(
        auth_data, auth_data_len, signature, (uint8_t)sig_len,
        NULL, 0, response, response_len);
    LOG_I(TAG, "Probe makeCredential status=0x%02X resp_len=%u", status, *response_len);
    return status;
}

/**
 * \brief Detects known browser probe RP IDs.
 * \param rp_id RP ID string to test.
 * \return `true` if this RP ID is treated as a probe, otherwise `false`.
 */
static bool is_browser_probe(const char *rp_id) {
    return strcmp(rp_id, "make.me.blink") == 0 || strcmp(rp_id, ".dummy") == 0;
}

/**
 * \brief Deletes a just-created credential and reports `CTAP2_ERR_OTHER`.
 * \param slot Credential slot to roll back.
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return `CTAP2_ERR_OTHER`.
 */
static uint8_t mc_rollback_credential(uint8_t slot, uint8_t *response,
                                      uint16_t *response_len) {
    fido2_storage_delete_credential(slot);
    response[0] = CTAP2_ERR_OTHER;
    *response_len = 1;
    return CTAP2_ERR_OTHER;
}

/**
 * \brief Creates credential, signs attestation statement, and builds response.
 * \param p Parsed makeCredential parameters.
 * \param curve Selected key curve.
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
static uint8_t create_credential_and_respond(const MakeCredentialParams *p,
                                             uint8_t curve,
                                             uint8_t *response, uint16_t *response_len) {
    // Send KEEPALIVE before long operation (key generation takes ~200-500ms)
    ctap2_send_keepalive(CTAPHID_STATUS_PROCESSING);

    // Create credential
    uint8_t slot;
    uint8_t cred_id[FIDO2_CRED_ID_LEN];
    uint8_t pubkey[64];  // P-256: X||Y, Ed25519: 32 bytes (only first half used)

    LOG_I(TAG, "Calling fido2_storage_create_credential...");
    if (!fido2_storage_create_credential(
            p->rp_id, p->rp_id_hash, p->user_id, p->user_id_len, p->user_name,
            p->rk, p->cred_protect, curve, &slot, cred_id, pubkey)) {
        response[0] = CTAP2_ERR_KEY_STORE_FULL;
        *response_len = 1;
        return CTAP2_ERR_KEY_STORE_FULL;
    }

    uint8_t attested_cred[256];
    uint16_t attested_len = 0;
    uint8_t auth_data[256];
    uint16_t auth_data_len = 0;

    if (!ctap2_build_attested_cred(cred_id, FIDO2_CRED_ID_LEN, pubkey, curve,
                                   attested_cred, sizeof(attested_cred),
                                   &attested_len) ||
        !ctap2_build_auth_data_for_cred(p->rp_id_hash, attested_cred, attested_len,
                                        p->cred_protect, auth_data, &auth_data_len)) {
        return mc_rollback_credential(slot, response, response_len);
    }

    // Large buffer in PSRAM to save stack space
    EXT_RAM_BSS_ATTR static uint8_t mc_to_sign[512];
    if (auth_data_len + 32 > sizeof(mc_to_sign)) {
        return mc_rollback_credential(slot, response, response_len);
    }
    memcpy(mc_to_sign, auth_data, auth_data_len);
    memcpy(mc_to_sign + auth_data_len, p->client_data_hash, 32);
    uint16_t to_sign_len = auth_data_len + 32;

    // Sign with attestation key for basic attestation
    uint8_t signature[128];
    uint8_t sig_len = 0;
    const uint8_t *att_cert = NULL;
    uint16_t att_cert_len = 0;

    LOG_I(TAG, "PIN state: pinToken_valid=%d, is_pin_verified=%d",
          g_client_pin.pin_token_valid, fido2_is_pin_verified());

    // Use packed attestation with FIDO2-compliant certificate
    if (!u2f_get_attestation_cert(&att_cert, &att_cert_len)) {
        LOG_E(TAG, "Attestation certificate not initialized");
        return mc_rollback_credential(slot, response, response_len);
    }

    // Send KEEPALIVE before signing (TROPIC01 ECDSA takes ~100ms)
    ctap2_send_keepalive(CTAPHID_STATUS_PROCESSING);

    if (!u2f_attestation_sign(mc_to_sign, to_sign_len, signature, &sig_len)) {
        LOG_E(TAG, "Attestation signing failed");
        return mc_rollback_credential(slot, response, response_len);
    }
    LOG_I(TAG, "Using basic attestation (cert=%u, sig=%u)", att_cert_len, sig_len);

    uint8_t status = ctap2_build_make_credential_response_packed(
        auth_data, auth_data_len, signature, sig_len,
        att_cert, att_cert_len, response, response_len);

    if (status != CTAP2_OK) {
        return mc_rollback_credential(slot, response, response_len);
    }
    LOG_I(TAG, "Created credential for %s (slot %d)", p->rp_id, slot);

#if CTAP2_DEBUG
    LOG_I(TAG, "makeCredential status=0x%02X resp_len=%u", status, *response_len);
    for (uint16_t offset = 0; offset < *response_len; offset += 16) {
        char hex[50] = {0};
        int dump_len = ((*response_len - offset) < 16) ? (*response_len - offset) : 16;
        for (int i = 0; i < dump_len; i++) {
            sprintf(hex + (i * 3), "%02X ", response[offset + i]);
        }
        LOG_D(TAG, "%03u: %s", offset, hex);
    }
#endif

    return status;
}

/**
 * \brief Handles CTAP2 `authenticatorMakeCredential` (`0x01`).
 * \param params CBOR request payload.
 * \param params_len Length of `params`.
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
uint8_t ctap2_make_credential(const uint8_t *params, uint16_t params_len,
                               uint8_t *response, uint16_t *response_len) {
    MakeCredentialParams p;

    // Step 1: Parse all CBOR parameters
    uint8_t status = parse_make_credential_params(params, params_len, &p);
    if (status != CTAP2_OK) {
        response[0] = status;
        *response_len = 1;
        return status;
    }

    LOG_I(TAG, "makeCredential rp_id=%s rk=%d uv=%d up=%d alg=%d pinProto=%d pinAuthLen=%zu",
          p.rp_id[0] ? p.rp_id : "(none)", p.rk, p.option_uv, p.option_up, p.alg,
          p.pin_uv_auth_protocol, p.pin_uv_auth_param_len);

    // Step 2: Check appidExclude extension
    status = check_appid_exclude(&p);
    if (status != CTAP2_OK) {
        response[0] = status;
        *response_len = 1;
        return status;
    }

    // Step 3: Verify PIN/UV auth parameter
    status = verify_pin_uv_auth(&p);
    if (status != CTAP2_OK) {
        response[0] = status;
        *response_len = 1;
        return status;
    }

    // Step 4: Handle browser probe requests
    if (is_browser_probe(p.rp_id)) {
        return handle_browser_probe(&p, response, response_len);
    }

    // Step 5: Determine curve from algorithm
    uint8_t curve;
    if (p.alg == COSE_ALG_ES256) {
        curve = CDC_CURVE_P256;
    } else if (p.alg == COSE_ALG_EDDSA) {
        curve = CDC_CURVE_ED25519;
    } else {
        response[0] = CTAP2_ERR_UNSUPPORTED_ALGORITHM;
        *response_len = 1;
        return CTAP2_ERR_UNSUPPORTED_ALGORITHM;
    }

    // Step 6: Validate options
    if (p.option_uv) {
        response[0] = CTAP2_ERR_UNSUPPORTED_OPTION;
        *response_len = 1;
        return CTAP2_ERR_UNSUPPORTED_OPTION;
    }
    if (!p.option_up) {
        response[0] = CTAP2_ERR_INVALID_OPTION;
        *response_len = 1;
        return CTAP2_ERR_INVALID_OPTION;
    }

    fido2_action_t up_action = FIDO2_ACTION_REGISTER;
    if (fido2_storage_find_by_rp_user(p.rp_id_hash, p.user_id, p.user_id_len) >= 0) {
        up_action = FIDO2_ACTION_OVERWRITE;
    }

    if (!wait_for_user_presence(p.rp_id, up_action, p.user_name)) {
        response[0] = CTAP2_ERR_OPERATION_DENIED;
        *response_len = 1;
        return CTAP2_ERR_OPERATION_DENIED;
    }

    LOG_I(TAG, "User presence OK, creating credential (curve=%d)...", curve);

    // Step 8: Create credential and build response
    return create_credential_and_respond(&p, curve, response, response_len);
}

/** \brief Parsed parameters for `authenticatorGetAssertion`. */
struct GetAssertionParams {
    char rp_id[FIDO2_RP_ID_MAX_LEN];
    uint8_t rp_id_hash[32];
    uint8_t client_data_hash[32];
    bool has_rp;
    bool has_client_data;

    // Allow list
    bool allow_list_present;
    uint8_t allow_list_slots[FIDO2_MAX_CREDENTIALS];
    uint8_t allow_list_count;

    // Options
    bool option_uv;
    bool option_up;

    // Extensions
    char appid[256];
    bool has_appid;
    uint8_t appid_hash[32];

    // PIN/UV auth
    uint8_t pin_uv_auth_param[32];
    size_t pin_uv_auth_param_len;
    uint8_t pin_uv_auth_protocol;
};

/** \brief Credential-selection result used to build assertion responses. */
struct AssertionCredentials {
    uint8_t slots[FIDO2_MAX_CREDENTIALS];
    uint8_t count;
    bool include_user;
    bool appid_used;
    uint8_t* hash_in_use;  // Points to rp_id_hash or appid_hash
};

#ifdef __DOXYGEN__
} // namespace cdc::mod_fido2
#endif

/**
 * \brief Parses one allowList credential descriptor and extracts credential ID.
 * \param r CBOR reader positioned at one descriptor map.
 * \param cred_id Output credential ID buffer.
 * \param cred_id_len Output credential ID length.
 * \return `true` on success, otherwise `false`.
 */
static bool ga_parse_allow_list_credential(cbor_reader_t *r, uint8_t *cred_id,
                                           size_t *cred_id_len) {
    int cred_map = cbor_read_map(r);
    if (cred_map < 0) {
        return false;
    }

    *cred_id_len = 0;
    bool have_id = false;

    for (int k = 0; k < cred_map; k++) {
        char cred_key[16];
        size_t key_len;
        if (!cbor_read_text(r, cred_key, sizeof(cred_key), &key_len)) {
            cbor_skip_item(r);
            continue;
        }
        if (strcmp(cred_key, "id") == 0) {
            if (!cbor_read_bytes(r, cred_id, FIDO2_CRED_ID_LEN, cred_id_len)) {
                return false;
            }
            have_id = true;
        } else {
            cbor_skip_item(r);
        }
    }

    return have_id && *cred_id_len == FIDO2_CRED_ID_LEN;
}

/**
 * \brief Parses getAssertion `allowList` (map key `0x03`).
 * \param r CBOR reader positioned at the allowList value.
 * \param p Output getAssertion parameter structure.
 * \return CTAP2 status code.
 */
static uint8_t ga_parse_allow_list(cbor_reader_t *r, GetAssertionParams *p) {
    int list_count = cbor_read_array(r);
    if (list_count < 0) {
        return CTAP2_ERR_INVALID_CBOR;
    }

    p->allow_list_present = true;

    for (int j = 0; j < list_count; j++) {
        uint8_t cred_id[FIDO2_CRED_ID_LEN];
        size_t cred_id_len = 0;

        if (!ga_parse_allow_list_credential(r, cred_id, &cred_id_len)) {
            continue;
        }

        int8_t slot = fido2_storage_find_slot_by_cred_id(cred_id, cred_id_len);
        if (slot < 0) {
            continue;
        }

        // Avoid duplicates
        bool exists = false;
        for (uint8_t m = 0; m < p->allow_list_count; m++) {
            if (p->allow_list_slots[m] == (uint8_t)slot) {
                exists = true;
                break;
            }
        }
        if (!exists && p->allow_list_count < FIDO2_MAX_CREDENTIALS) {
            p->allow_list_slots[p->allow_list_count++] = (uint8_t)slot;
        }
    }

    return CTAP2_OK;
}

/**
 * \brief Parses getAssertion extensions (map key `0x04`).
 * \param r CBOR reader positioned at the extensions value.
 * \param p Output getAssertion parameter structure.
 * \return CTAP2 status code.
 */
static uint8_t ga_parse_extensions(cbor_reader_t *r, GetAssertionParams *p) {
    int ext_count = cbor_read_map(r);
    if (ext_count < 0) {
        return CTAP2_ERR_INVALID_CBOR;
    }

    for (int j = 0; j < ext_count; j++) {
        char ext_key[16];
        size_t key_len;
        if (!cbor_read_text(r, ext_key, sizeof(ext_key), &key_len)) {
            cbor_skip_item(r);
            continue;
        }
        if (strcmp(ext_key, "appid") == 0) {
            size_t len;
            if (cbor_read_text(r, p->appid, sizeof(p->appid), &len)) {
                p->has_appid = (len > 0);
                if (p->has_appid) {
                    sha256_str(p->appid, p->appid_hash);
                }
            }
        } else {
            cbor_skip_item(r);
        }
    }

    return CTAP2_OK;
}

/**
 * \brief Parses getAssertion options (map key `0x05`).
 * \param r CBOR reader positioned at the options value.
 * \param p Output getAssertion parameter structure.
 * \return CTAP2 status code.
 */
static uint8_t ga_parse_options(cbor_reader_t *r, GetAssertionParams *p) {
    int opt_count = cbor_read_map(r);
    if (opt_count < 0) {
        return CTAP2_ERR_INVALID_CBOR;
    }

    for (int j = 0; j < opt_count; j++) {
        char opt_key[8];
        size_t key_len;
        if (!cbor_read_text(r, opt_key, sizeof(opt_key), &key_len)) {
            cbor_skip_item(r);
            continue;
        }
        if (strcmp(opt_key, "uv") == 0) {
            cbor_read_bool(r, &p->option_uv);
        } else if (strcmp(opt_key, "up") == 0) {
            cbor_read_bool(r, &p->option_up);
        } else {
            cbor_skip_item(r);
        }
    }

    return CTAP2_OK;
}

/**
 * \brief Parses complete getAssertion request map from CBOR payload.
 * \param params CBOR request payload.
 * \param params_len Length of `params`.
 * \param p Output parameter structure.
 * \return CTAP2 status code.
 */
static uint8_t ga_parse_params(const uint8_t *params, uint16_t params_len,
                               GetAssertionParams *p) {
    memset(p, 0, sizeof(*p));
    p->option_up = true;  // Default: user presence required

    cbor_reader_t r;
    cbor_reader_init(&r, params, params_len);

    int map_count = cbor_read_map(&r);
    if (map_count < 0) {
        return CTAP2_ERR_INVALID_CBOR;
    }

    for (int i = 0; i < map_count; i++) {
        uint64_t key;
        if (!cbor_read_uint(&r, &key)) {
            return CTAP2_ERR_INVALID_CBOR;
        }

        uint8_t status = CTAP2_OK;

        switch (key) {
            case CTAP2_GA_RP_ID:
                {
                    size_t len;
                    cbor_read_text(&r, p->rp_id, sizeof(p->rp_id), &len);
                    sha256_str(p->rp_id, p->rp_id_hash);
                    p->has_rp = true;
                }
                break;

            case CTAP2_GA_CLIENT_DATA_HASH:
                {
                    size_t len;
                    if (!cbor_read_bytes(&r, p->client_data_hash, 32, &len) || len != 32) {
                        return CTAP2_ERR_INVALID_CBOR;
                    }
                    p->has_client_data = true;
                }
                break;

            case CTAP2_GA_ALLOW_LIST:
                status = ga_parse_allow_list(&r, p);
                if (status != CTAP2_OK) return status;
                break;

            case CTAP2_GA_EXTENSIONS:
                status = ga_parse_extensions(&r, p);
                if (status != CTAP2_OK) return status;
                break;

            case CTAP2_GA_OPTIONS:
                status = ga_parse_options(&r, p);
                if (status != CTAP2_OK) return status;
                break;

            case CTAP2_GA_PIN_UV_AUTH_PARAM:
                cbor_read_bytes(&r, p->pin_uv_auth_param, sizeof(p->pin_uv_auth_param),
                               &p->pin_uv_auth_param_len);
                break;

            case CTAP2_GA_PIN_UV_AUTH_PROTOCOL:
                {
                    uint64_t proto;
                    if (cbor_read_uint(&r, &proto)) {
                        p->pin_uv_auth_protocol = (uint8_t)proto;
                    }
                }
                break;

            default:
                cbor_skip_item(&r);
                break;
        }
    }

    return CTAP2_OK;
}

/**
 * \brief Verifies getAssertion `pinUvAuthParam` via HMAC.
 * \param p Parsed getAssertion parameters.
 * \param uv_verified Output flag set to UV verification result.
 * \return CTAP2 status code.
 */
static uint8_t ga_verify_pin_auth(const GetAssertionParams *p, bool *uv_verified) {
    *uv_verified = false;

    if (p->pin_uv_auth_param_len == 0) {
        return CTAP2_OK;  // No PIN auth provided, continue without UV
    }

    if (!g_client_pin.pin_token_valid) {
        LOG_W(TAG, "pinUvAuthParam provided but no valid pinToken");
        return CTAP2_ERR_PIN_AUTH_INVALID;
    }

    // Compute HMAC-SHA256(pinToken, clientDataHash)
    uint8_t expected_hmac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    g_client_pin.pin_token, PIN_TOKEN_SIZE,
                    p->client_data_hash, 32,
                    expected_hmac);

    // Protocol 2 uses first 32 bytes of HMAC, protocol 1 uses 16
    size_t compare_len = (p->pin_uv_auth_protocol == 2) ? 32 : 16;
    if (p->pin_uv_auth_param_len < compare_len) {
        LOG_W(TAG, "pinUvAuthParam too short: %zu < %zu",
              p->pin_uv_auth_param_len, compare_len);
        return CTAP2_ERR_PIN_AUTH_INVALID;
    }

#if DEBUG_MODE
    LOG_D(TAG, "pinUvAuthParam received (%zu bytes):", p->pin_uv_auth_param_len);
    LOG_D(TAG, "  %02X%02X%02X%02X %02X%02X%02X%02X...",
          p->pin_uv_auth_param[0], p->pin_uv_auth_param[1],
          p->pin_uv_auth_param[2], p->pin_uv_auth_param[3],
          p->pin_uv_auth_param[4], p->pin_uv_auth_param[5],
          p->pin_uv_auth_param[6], p->pin_uv_auth_param[7]);
    LOG_D(TAG, "Expected HMAC (first %zu bytes):", compare_len);
    LOG_D(TAG, "  %02X%02X%02X%02X %02X%02X%02X%02X...",
          expected_hmac[0], expected_hmac[1], expected_hmac[2], expected_hmac[3],
          expected_hmac[4], expected_hmac[5], expected_hmac[6], expected_hmac[7]);
#endif

    if (memcmp(p->pin_uv_auth_param, expected_hmac, compare_len) != 0) {
        LOG_W(TAG, "pinUvAuthParam verification failed");
        return CTAP2_ERR_PIN_AUTH_INVALID;
    }

    LOG_I(TAG, "pinUvAuthParam verified - UV=1");
    *uv_verified = true;
    fido2_set_pin_verified(true);

    return CTAP2_OK;
}

/**
 * \brief Finds credentials matching RP/allowList and appid extension rules.
 * \param p Parsed getAssertion parameters (modified during selection).
 * \param creds Output credential selection result.
 * \return void
 */
static void ga_find_credentials(GetAssertionParams *p, AssertionCredentials *creds) {
    memset(creds, 0, sizeof(*creds));
    creds->hash_in_use = p->rp_id_hash;

    uint8_t temp_slots[FIDO2_MAX_CREDENTIALS] = {0};
    uint8_t temp_count = 0;

    if (p->allow_list_present && p->allow_list_count > 0) {
        // Filter allowList by RP ID hash
        uint8_t filtered = 0;
        for (uint8_t i = 0; i < p->allow_list_count; i++) {
            fido2_credential_info_t info;
            if (fido2_storage_get_credential(p->allow_list_slots[i], &info) &&
                memcmp(info.rp_id_hash, p->rp_id_hash, 32) == 0) {
                creds->slots[filtered++] = p->allow_list_slots[i];
            }
        }
        creds->count = filtered;
        creds->include_user = false;

        LOG_I(TAG, "getAssertion using allowList, matches=%u", creds->count);

        // Try appid extension if present
        if (p->has_appid) {
            uint8_t filtered_appid = 0;
            for (uint8_t i = 0; i < p->allow_list_count; i++) {
                fido2_credential_info_t info;
                if (fido2_storage_get_credential(p->allow_list_slots[i], &info) &&
                    memcmp(info.rp_id_hash, p->appid_hash, 32) == 0) {
                    temp_slots[filtered_appid++] = p->allow_list_slots[i];
                }
            }
            if (filtered_appid > 0) {
                memcpy(creds->slots, temp_slots, filtered_appid);
                creds->count = filtered_appid;
                creds->appid_used = true;
                creds->hash_in_use = p->appid_hash;
            }
        }
    } else {
        // No allowList - search all credentials for this RP
        creds->count = fido2_storage_find_by_rp(p->rp_id_hash, creds->slots,
                                                 FIDO2_MAX_CREDENTIALS);
        creds->include_user = true;

        LOG_I(TAG, "getAssertion using all RP creds, matches=%u", creds->count);

        // Try appid extension if present
        if (p->has_appid) {
            temp_count = fido2_storage_find_by_rp(p->appid_hash, temp_slots,
                                                   FIDO2_MAX_CREDENTIALS);
            if (temp_count > 0) {
                memcpy(creds->slots, temp_slots, temp_count);
                creds->count = temp_count;
                creds->appid_used = true;
                creds->hash_in_use = p->appid_hash;
            }
        }
    }
}

/**
 * \brief Signs assertion message (`authData || clientDataHash`) for one credential slot.
 * \param slot Credential slot index.
 * \param auth_data Authenticator data bytes.
 * \param auth_data_len Length of `auth_data`.
 * \param client_data_hash ClientDataHash bytes.
 * \param signature Destination signature buffer.
 * \param sig_len Output signature length.
 * \return CTAP2 status code.
 */
static uint8_t ga_sign_assertion(uint8_t slot, const uint8_t *auth_data,
                                 uint16_t auth_data_len, const uint8_t *client_data_hash,
                                 uint8_t *signature, uint8_t *sig_len) {
    // Prepare data to sign: authData || clientDataHash
    uint8_t to_sign[96];  // Max 64 bytes auth_data + 32 bytes clientDataHash
    if (auth_data_len > sizeof(to_sign) - 32) {
        LOG_E(TAG, "auth_data too large: %u", auth_data_len);
        return CTAP2_ERR_OTHER;
    }

    memcpy(to_sign, auth_data, auth_data_len);
    memcpy(to_sign + auth_data_len, client_data_hash, 32);
    uint16_t to_sign_len = auth_data_len + 32;

    if (!fido2_storage_sign_raw(slot, to_sign, to_sign_len, signature, sig_len)) {
        return CTAP2_ERR_OTHER;
    }

    return CTAP2_OK;
}

/**
 * \brief Builds CBOR response payload for getAssertion/getNextAssertion.
 * \param cred_id Credential ID bytes.
 * \param auth_data Authenticator data bytes.
 * \param auth_data_len Length of `auth_data`.
 * \param signature Assertion signature bytes.
 * \param sig_len Length of `signature`.
 * \param cred Credential metadata record.
 * \param include_user Whether to include user entity map.
 * \param total_creds Total matching credential count.
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
static uint8_t ga_build_response(const uint8_t *cred_id, const uint8_t *auth_data,
                                 uint16_t auth_data_len, const uint8_t *signature,
                                 uint8_t sig_len, const fido2_credential_info_t *cred,
                                 bool include_user, uint8_t total_creds,
                                 uint8_t *response, uint16_t *response_len) {
    cbor_writer_t w;
    cbor_writer_init(&w, response + 1, *response_len - 1);

    int resp_fields = 3;  // credential, authData, signature
    if (include_user) resp_fields++;
    if (total_creds > 1) resp_fields++;

    cbor_encode_map(&w, resp_fields);

    // credential descriptor
    cbor_encode_uint(&w, CTAP2_GA_RESP_CREDENTIAL);
    cbor_encode_map(&w, 2);
    // Canonical order: "id" (len 2) before "type" (len 4)
    cbor_encode_text(&w, "id");
    cbor_encode_bytes(&w, cred_id, FIDO2_CRED_ID_LEN);
    cbor_encode_text(&w, "type");
    cbor_encode_text(&w, "public-key");

    // authData
    cbor_encode_uint(&w, CTAP2_GA_RESP_AUTH_DATA);
    cbor_encode_bytes(&w, auth_data, auth_data_len);

    // signature
    cbor_encode_uint(&w, CTAP2_GA_RESP_SIGNATURE);
    cbor_encode_bytes(&w, signature, sig_len);

    // user (only for discoverable credentials)
    if (include_user) {
        cbor_encode_uint(&w, CTAP2_GA_RESP_USER);
        int user_fields = 1;
        if (cred->user_name[0] != '\0') user_fields++;
        cbor_encode_map(&w, user_fields);

        cbor_encode_text(&w, "id");
        cbor_encode_bytes(&w, cred->user_id, cred->user_id_len);

        if (cred->user_name[0] != '\0') {
            cbor_encode_text(&w, "name");
            cbor_encode_text(&w, cred->user_name);
        }
    }

    // numberOfCredentials (if multiple)
    if (total_creds > 1) {
        cbor_encode_uint(&w, CTAP2_GA_RESP_NUMBER_OF_CREDS);
        cbor_encode_uint(&w, total_creds);
    }

    if (cbor_writer_error(&w)) {
        return CTAP2_ERR_OTHER;
    }

    response[0] = CTAP2_OK;
    *response_len = 1 + cbor_writer_length(&w);
    return CTAP2_OK;
}

/**
 * \brief Handles CTAP2 `authenticatorGetAssertion` (`0x02`).
 * \param params CBOR request payload.
 * \param params_len Length of `params`.
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
uint8_t ctap2_get_assertion(const uint8_t *params, uint16_t params_len,
                             uint8_t *response, uint16_t *response_len) {
    // Step 1: Parse CBOR parameters
    GetAssertionParams p;
    uint8_t status = ga_parse_params(params, params_len, &p);
    if (status != CTAP2_OK) {
        response[0] = status;
        *response_len = 1;
        return status;
    }

    LOG_I(TAG, "getAssertion rp_id=%s allowList=%d count=%u uv=%d up=%d appid=%s",
          p.rp_id[0] ? p.rp_id : "(none)", p.allow_list_present ? 1 : 0,
          p.allow_list_count, p.option_uv, p.option_up, p.has_appid ? p.appid : "(none)");

    // Step 2: Validate required parameters
    if (!p.has_rp || !p.has_client_data) {
        response[0] = CTAP2_ERR_MISSING_PARAMETER;
        *response_len = 1;
        return CTAP2_ERR_MISSING_PARAMETER;
    }

    if (p.option_uv) {
        response[0] = CTAP2_ERR_UNSUPPORTED_OPTION;
        *response_len = 1;
        return CTAP2_ERR_UNSUPPORTED_OPTION;
    }

    // Step 3: Verify PIN/UV auth if provided
    bool uv_verified = false;
    status = ga_verify_pin_auth(&p, &uv_verified);
    if (status != CTAP2_OK) {
        response[0] = status;
        *response_len = 1;
        return status;
    }

    // Step 4: Find matching credentials
    AssertionCredentials creds;
    ga_find_credentials(&p, &creds);

    if (creds.count == 0) {
        response[0] = CTAP2_ERR_NO_CREDENTIALS;
        *response_len = 1;
        return CTAP2_ERR_NO_CREDENTIALS;
    }

    // Step 5: Request user presence (if required)
    if (p.option_up) {
        if (!wait_for_user_presence(p.rp_id, FIDO2_ACTION_AUTHENTICATE, NULL)) {
            response[0] = CTAP2_ERR_OPERATION_DENIED;
            *response_len = 1;
            return CTAP2_ERR_OPERATION_DENIED;
        }
    }

    // Step 6: Save state for getNextAssertion
    memcpy(g_ctap2.assertion_creds, creds.slots, creds.count);
    g_ctap2.assertion_count = creds.count;
    g_ctap2.assertion_include_user = creds.include_user;
    memcpy(g_ctap2.assertion_rp_id_hash, creds.hash_in_use, 32);
    memcpy(g_ctap2.assertion_client_data_hash, p.client_data_hash, 32);
    g_ctap2.assertion_index = 0;
    g_ctap2.assertion_up_done = p.option_up;
    g_ctap2.assertion_appid_used = creds.appid_used;

    // Step 7: Get first credential
    uint8_t slot = g_ctap2.assertion_creds[0];
    fido2_credential_info_t cred;
    if (!fido2_storage_get_credential(slot, &cred)) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    // Step 8: Build authenticator data
    uint32_t sign_count = fido2_storage_increment_sign_count(slot);
    if (sign_count == 0) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    uint8_t flags = p.option_up ? 0x01 : 0x00;  // UP=1 only if user presence was requested
    if (uv_verified) {
        flags |= 0x04;  // UV=1
    }

    uint8_t ext_data[32];
    uint16_t ext_len = 0;
    if (creds.appid_used) {
        ext_len = ctap2_build_appid_extension(ext_data, sizeof(ext_data));
    }

    uint8_t auth_data[128];
    uint16_t auth_data_len;
    build_authenticator_data(creds.hash_in_use, flags, sign_count, NULL, 0,
                              ext_data, ext_len, auth_data, &auth_data_len);

    // Step 9: Generate signature
    uint8_t signature[128];
    uint8_t sig_len;
    status = ga_sign_assertion(slot, auth_data, auth_data_len, p.client_data_hash,
                               signature, &sig_len);
    if (status != CTAP2_OK) {
        response[0] = status;
        *response_len = 1;
        return status;
    }

    // Step 10: Get credential ID
    uint8_t cred_id[FIDO2_CRED_ID_LEN];
    if (!fido2_storage_get_cred_id(slot, cred_id)) {
        LOG_E(TAG, "Failed to get credential ID for slot %d", slot);
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    // Step 11: Build CBOR response
    status = ga_build_response(cred_id, auth_data, auth_data_len, signature, sig_len,
                               &cred, g_ctap2.assertion_include_user,
                               g_ctap2.assertion_count, response, response_len);
    if (status != CTAP2_OK) {
        response[0] = status;
        *response_len = 1;
        return status;
    }

    fido2_increment_auth_counter();

    LOG_I(TAG, "Assertion for %s (slot %d)", p.rp_id, slot);
    return CTAP2_OK;
}

/**
 * \brief Handles CTAP2 `authenticatorGetNextAssertion` (`0x08`).
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
uint8_t ctap2_get_next_assertion(uint8_t *response, uint16_t *response_len) {
    if (g_ctap2.assertion_count == 0) {
        response[0] = CTAP2_ERR_NOT_ALLOWED;
        *response_len = 1;
        return CTAP2_ERR_NOT_ALLOWED;
    }

    g_ctap2.assertion_index++;
    if (g_ctap2.assertion_index >= g_ctap2.assertion_count) {
        response[0] = CTAP2_ERR_NOT_ALLOWED;
        *response_len = 1;
        return CTAP2_ERR_NOT_ALLOWED;
    }

    // Similar to getAssertion but without user presence check
    uint8_t slot = g_ctap2.assertion_creds[g_ctap2.assertion_index];
    fido2_credential_info_t cred;
    if (!fido2_storage_get_credential(slot, &cred)) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    uint32_t sign_count = fido2_storage_increment_sign_count(slot);
    if (sign_count == 0) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    uint8_t auth_data[128];
    uint16_t auth_data_len;
    uint8_t ext_data[32];
    uint16_t ext_len = 0;
    if (g_ctap2.assertion_appid_used) {
        ext_len = ctap2_build_appid_extension(ext_data, sizeof(ext_data));
    }
    build_authenticator_data(g_ctap2.assertion_rp_id_hash, 0x01, sign_count,
                              NULL, 0, ext_data, ext_len, auth_data, &auth_data_len);

    // Sign: authData || clientDataHash (TROPIC01 hashes internally)
    uint8_t to_sign[96];
    if (auth_data_len > sizeof(to_sign) - 32) {
        LOG_E(TAG, "auth_data too large: %u", auth_data_len);
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }
    memcpy(to_sign, auth_data, auth_data_len);
    memcpy(to_sign + auth_data_len, g_ctap2.assertion_client_data_hash, 32);
    uint16_t to_sign_len = auth_data_len + 32;

    uint8_t signature[128];
    uint8_t sig_len;
    if (!fido2_storage_sign_raw(slot, to_sign, to_sign_len, signature, &sig_len)) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    // Get credential ID
    uint8_t cred_id[FIDO2_CRED_ID_LEN];
    if (!fido2_storage_get_cred_id(slot, cred_id)) {
        LOG_E(TAG, "Failed to get credential ID for slot %d", slot);
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    cbor_writer_t w;
    cbor_writer_init(&w, response + 1, *response_len - 1);

    cbor_encode_map(&w, 3);

    // credential descriptor (type + id)
    cbor_encode_uint(&w, CTAP2_GA_RESP_CREDENTIAL);
    cbor_encode_map(&w, 2);
    // Canonical order: "id" (len 2) before "type" (len 4)
    cbor_encode_text(&w, "id");
    cbor_encode_bytes(&w, cred_id, FIDO2_CRED_ID_LEN);
    cbor_encode_text(&w, "type");
    cbor_encode_text(&w, "public-key");

    // authData
    cbor_encode_uint(&w, CTAP2_GA_RESP_AUTH_DATA);
    cbor_encode_bytes(&w, auth_data, auth_data_len);

    // signature
    cbor_encode_uint(&w, CTAP2_GA_RESP_SIGNATURE);
    cbor_encode_bytes(&w, signature, sig_len);

    response[0] = CTAP2_OK;
    *response_len = 1 + cbor_writer_length(&w);
    return CTAP2_OK;
}

/** \brief ClientPIN command implementation helpers. */
/**
 * \brief Initializes the ClientPIN ephemeral ECDH key pair.
 * \return `true` on success, otherwise `false`.
 */
static bool client_pin_init_ecdh(void) {
    if (g_client_pin.ecdh_valid) return true;

    mbedtls_ecp_keypair_init(&g_client_pin.ecdh_key);

    int ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                                   &g_client_pin.ecdh_key,
                                   ctap2_random, NULL);
    if (ret != 0) {
        LOG_E(TAG_PIN, "ECDH key generation failed: %d", ret);
        return false;
    }

    g_client_pin.ecdh_valid = true;
    g_client_pin.pin_retries = PIN_RETRIES_MAX;
    g_client_pin.uv_retries = PIN_UV_RETRIES_MAX;
    LOG_I(TAG_PIN, "ECDH key pair generated");
    return true;
}

/**
 * \brief Computes ClientPIN shared secret from platform ECDH public key.
 * \param platform_key_x Platform public key X coordinate.
 * \param platform_key_y Platform public key Y coordinate.
 * \param pin_protocol PIN protocol version.
 * \param shared_secret Output 32-byte shared secret.
 * \return `true` on success, otherwise `false`.
 */
static bool client_pin_compute_shared_secret(const uint8_t *platform_key_x,
                                              const uint8_t *platform_key_y,
                                              uint8_t pin_protocol,
                                              uint8_t *shared_secret) {
    if (!g_client_pin.ecdh_valid) return false;

    mbedtls_ecp_point platform_point;
    mbedtls_mpi shared_x;

    mbedtls_ecp_point_init(&platform_point);
    mbedtls_mpi_init(&shared_x);

    int ret = 0;

    // Load platform public key
    ret = mbedtls_mpi_read_binary(&platform_point.MBEDTLS_PRIVATE(X), platform_key_x, 32);
    if (ret != 0) goto cleanup;

    ret = mbedtls_mpi_read_binary(&platform_point.MBEDTLS_PRIVATE(Y), platform_key_y, 32);
    if (ret != 0) goto cleanup;

    ret = mbedtls_mpi_lset(&platform_point.MBEDTLS_PRIVATE(Z), 1);
    if (ret != 0) goto cleanup;

    // Compute ECDH: shared_x = (platformPubKey * authenticatorPrivKey).x
    ret = mbedtls_ecdh_compute_shared(&g_client_pin.ecdh_key.MBEDTLS_PRIVATE(grp),
                                       &shared_x,
                                       &platform_point,
                                       &g_client_pin.ecdh_key.MBEDTLS_PRIVATE(d),
                                       ctap2_random, NULL);
    if (ret != 0) {
        LOG_E(TAG_PIN, "ECDH compute failed: %d", ret);
        goto cleanup;
    }

    // Extract x coordinate as bytes (Z = ECDH shared secret)
    uint8_t ecdh_z[32];
    ret = mbedtls_mpi_write_binary(&shared_x, ecdh_z, 32);
    if (ret != 0) goto cleanup;

#if DEBUG_MODE
    // Full debug output for manual verification
    LOG_I(TAG_PIN, "=== ECDH DEBUG (full 32-byte values) ===");
    LOG_I(TAG_PIN, "Z (ECDH x-coord):");
    LOG_I(TAG_PIN, "  %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
          ecdh_z[0], ecdh_z[1], ecdh_z[2], ecdh_z[3], ecdh_z[4], ecdh_z[5], ecdh_z[6], ecdh_z[7],
          ecdh_z[8], ecdh_z[9], ecdh_z[10], ecdh_z[11], ecdh_z[12], ecdh_z[13], ecdh_z[14], ecdh_z[15]);
    LOG_I(TAG_PIN, "  %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
          ecdh_z[16], ecdh_z[17], ecdh_z[18], ecdh_z[19], ecdh_z[20], ecdh_z[21], ecdh_z[22], ecdh_z[23],
          ecdh_z[24], ecdh_z[25], ecdh_z[26], ecdh_z[27], ecdh_z[28], ecdh_z[29], ecdh_z[30], ecdh_z[31]);
#endif // DEBUG_MODE

    if (pin_protocol == 1) {
        // Protocol 1: sharedSecret = SHA256(Z)
        LOG_D(TAG_PIN, "Using Protocol 1: SHA256(Z)");
        mbedtls_sha256(ecdh_z, 32, shared_secret, 0);  // 0 = SHA256 (not SHA224)
    } else {
        // Protocol 2: Use HKDF-SHA256 to derive AES key
        // AES_key = HKDF-SHA256(salt=32zeros, IKM=Z, L=32, info="CTAP2 AES key")
        LOG_D(TAG_PIN, "Using Protocol 2: HKDF(Z)");
        const char *info = "CTAP2 AES key";
        uint8_t prk[32];
        uint8_t zero_salt[32] = {0};

        // HKDF Extract: PRK = HMAC-SHA256(salt, IKM=Z)
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                        zero_salt, 32, ecdh_z, 32, prk);

        // HKDF Expand: OKM = HMAC-SHA256(PRK, info || 0x01)
        uint8_t expand_input[32];
        size_t info_len = strlen(info);
        memcpy(expand_input, info, info_len);
        expand_input[info_len] = 0x01;

#if DEBUG_MODE
        LOG_I(TAG_PIN, "HKDF PRK (HMAC-SHA256(salt=0, IKM=Z)):");
        LOG_I(TAG_PIN, "  %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
              prk[0], prk[1], prk[2], prk[3], prk[4], prk[5], prk[6], prk[7],
              prk[8], prk[9], prk[10], prk[11], prk[12], prk[13], prk[14], prk[15]);
        LOG_I(TAG_PIN, "  %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
              prk[16], prk[17], prk[18], prk[19], prk[20], prk[21], prk[22], prk[23],
              prk[24], prk[25], prk[26], prk[27], prk[28], prk[29], prk[30], prk[31]);
        LOG_I(TAG_PIN, "HKDF info: '%s' || 0x01 (len=%zu)", info, info_len + 1);
#endif

        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                        prk, 32, expand_input, info_len + 1, shared_secret);
    }

#if DEBUG_MODE
    LOG_I(TAG_PIN, "AES key (shared secret):");
    LOG_I(TAG_PIN, "  %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
          shared_secret[0], shared_secret[1], shared_secret[2], shared_secret[3],
          shared_secret[4], shared_secret[5], shared_secret[6], shared_secret[7],
          shared_secret[8], shared_secret[9], shared_secret[10], shared_secret[11],
          shared_secret[12], shared_secret[13], shared_secret[14], shared_secret[15]);
    LOG_I(TAG_PIN, "  %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
          shared_secret[16], shared_secret[17], shared_secret[18], shared_secret[19],
          shared_secret[20], shared_secret[21], shared_secret[22], shared_secret[23],
          shared_secret[24], shared_secret[25], shared_secret[26], shared_secret[27],
          shared_secret[28], shared_secret[29], shared_secret[30], shared_secret[31]);
    LOG_I(TAG_PIN, "=== END ECDH DEBUG ===");
#endif

cleanup:
    mbedtls_ecp_point_free(&platform_point);
    mbedtls_mpi_free(&shared_x);
    return ret == 0;
}

/**
 * \brief Decrypts data using AES-256-CBC with caller-provided IV.
 * \param key 32-byte AES key.
 * \param iv 16-byte IV.
 * \param input Ciphertext buffer.
 * \param len Ciphertext length in bytes.
 * \param output Destination plaintext buffer.
 * \return `true` on success, otherwise `false`.
 */
static bool aes_256_cbc_decrypt_iv(const uint8_t *key, const uint8_t *iv,
                                    const uint8_t *input, size_t len, uint8_t *output) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);  // mbedtls modifies IV during decrypt

    int ret = mbedtls_aes_setkey_dec(&aes, key, 256);
    if (ret != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }

    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, len, iv_copy, input, output);
    mbedtls_aes_free(&aes);
    return ret == 0;
}

/**
 * \brief Decrypts Protocol-1 PIN payload (AES-256-CBC with zero IV).
 * \param key 32-byte AES key.
 * \param input Ciphertext buffer.
 * \param len Ciphertext length in bytes.
 * \param output Destination plaintext buffer.
 * \return `true` on success, otherwise `false`.
 */
static bool aes_256_cbc_decrypt(const uint8_t *key, const uint8_t *input,
                                 size_t len, uint8_t *output) {
    uint8_t iv[16] = {0};
    return aes_256_cbc_decrypt_iv(key, iv, input, len, output);
}

/**
 * \brief Encrypts Protocol-1 PIN payload (AES-256-CBC with zero IV).
 * \param key 32-byte AES key.
 * \param input Plaintext buffer.
 * \param len Plaintext length in bytes.
 * \param output Destination ciphertext buffer.
 * \return `true` on success, otherwise `false`.
 */
static bool aes_256_cbc_encrypt(const uint8_t *key, const uint8_t *input,
                                 size_t len, uint8_t *output) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    uint8_t iv[16] = {0};  // IV is all zeros for PIN protocol 1

    int ret = mbedtls_aes_setkey_enc(&aes, key, 256);
    if (ret != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }

    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, len, iv, input, output);
    mbedtls_aes_free(&aes);
    return ret == 0;
}

/**
 * \brief Encrypts Protocol-2 PIN payload and prefixes random IV (`IV || ciphertext`).
 * \param key 32-byte AES key.
 * \param input Plaintext buffer.
 * \param len Plaintext length in bytes.
 * \param output Destination buffer (`len + 16` bytes required).
 * \return `true` on success, otherwise `false`.
 */
static bool aes_256_cbc_encrypt_p2(const uint8_t *key, const uint8_t *input,
                                    size_t len, uint8_t *output) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    // Generate random IV using TROPIC01 TRNG
    uint8_t iv[16];
    secure_random_fill(iv, 16);

    // Copy IV to output first
    memcpy(output, iv, 16);

    int ret = mbedtls_aes_setkey_enc(&aes, key, 256);
    if (ret != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }

    // Encrypt after the IV prefix
    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, len, iv, input, output + 16);
    mbedtls_aes_free(&aes);
    return ret == 0;
}

/**
 * \brief Handles ClientPIN subcommand `getPINRetries` (`0x01`).
 * \param response Output response buffer.
 * \param response_len In/out length of `response`.
 * \return CTAP2 status code.
 */
static uint8_t client_pin_get_retries(uint8_t *response, uint16_t *response_len) {
    cbor_writer_t w;
    cbor_writer_init(&w, response + 1, *response_len - 1);

    cbor_encode_map(&w, 2);

    // pinRetries
    cbor_encode_uint(&w, CTAP2_PIN_RESP_PIN_RETRIES);
    cbor_encode_uint(&w, g_client_pin.pin_retries);

    // uvRetries (powerCycleState is optional and not used)
    cbor_encode_uint(&w, CTAP2_PIN_RESP_UV_RETRIES);
    cbor_encode_uint(&w, g_client_pin.uv_retries);

    response[0] = CTAP2_OK;
    *response_len = 1 + cbor_writer_length(&w);
    return CTAP2_OK;
}

/**
 * \brief Handles ClientPIN subcommand `getKeyAgreement` (`0x02`).
 * \param response Output response buffer.
 * \param response_len In/out length of `response`.
 * \return CTAP2 status code.
 */
static uint8_t client_pin_get_key_agreement(uint8_t *response, uint16_t *response_len) {
    if (!client_pin_init_ecdh()) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    // Extract public key coordinates
    uint8_t pub_x[32], pub_y[32];
    mbedtls_mpi_write_binary(&g_client_pin.ecdh_key.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X), pub_x, 32);
    mbedtls_mpi_write_binary(&g_client_pin.ecdh_key.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y), pub_y, 32);

#if DEBUG_MODE
    LOG_I(TAG_PIN, "=== Our ECDH public key ===");
    LOG_I(TAG_PIN, "X: %02X%02X%02X%02X %02X%02X%02X%02X...",
          pub_x[0], pub_x[1], pub_x[2], pub_x[3], pub_x[4], pub_x[5], pub_x[6], pub_x[7]);
    LOG_I(TAG_PIN, "Y: %02X%02X%02X%02X %02X%02X%02X%02X...",
          pub_y[0], pub_y[1], pub_y[2], pub_y[3], pub_y[4], pub_y[5], pub_y[6], pub_y[7]);
#endif

    cbor_writer_t w;
    cbor_writer_init(&w, response + 1, *response_len - 1);

    cbor_encode_map(&w, 1);

    // keyAgreement (COSE_Key, RFC 8152)
    cbor_encode_uint(&w, CTAP2_PIN_RESP_KEY_AGREEMENT);
    cbor_encode_map(&w, 5);

    // kty: EC2
    cbor_encode_uint(&w, COSE_KEY_LABEL_KTY);
    cbor_encode_uint(&w, COSE_KEY_TYPE_EC2);

    // alg: ECDH-ES + HKDF-256
    cbor_encode_uint(&w, COSE_KEY_LABEL_ALG);
    cbor_encode_int(&w, COSE_ALG_ECDH_ES_HKDF_256);

    // crv: P-256
    cbor_encode_int(&w, COSE_KEY_LABEL_CRV);
    cbor_encode_uint(&w, COSE_CRV_P256);

    // x coordinate
    cbor_encode_int(&w, COSE_KEY_LABEL_X);
    cbor_encode_bytes(&w, pub_x, 32);

    // y coordinate
    cbor_encode_int(&w, COSE_KEY_LABEL_Y);
    cbor_encode_bytes(&w, pub_y, 32);

    response[0] = CTAP2_OK;
    *response_len = 1 + cbor_writer_length(&w);
    LOG_I(TAG_PIN, "Sent key agreement");
    return CTAP2_OK;
}

/**
 * \brief Handles ClientPIN subcommand `getPinToken` (`0x05`).
 * \param params CBOR request payload.
 * \param params_len Length of `params`.
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
static uint8_t client_pin_get_pin_token(const uint8_t *params, uint16_t params_len,
                                         uint8_t *response, uint16_t *response_len) {
    // Check if PIN is blocked
    if (g_client_pin.pin_retries == 0) {
        response[0] = CTAP2_ERR_PIN_BLOCKED;
        *response_len = 1;
        return CTAP2_ERR_PIN_BLOCKED;
    }

    // Check if FIDO2 PIN hash is available
    if (!pin_storage_fido2_available()) {
        LOG_E(TAG_PIN, "FIDO2 hash not available - user must reset PIN");
        response[0] = CTAP2_ERR_PIN_NOT_SET;
        *response_len = 1;
        return CTAP2_ERR_PIN_NOT_SET;
    }

    // Parse parameters
    cbor_reader_t r;
    cbor_reader_init(&r, params, params_len);

    uint8_t platform_key_x[32] = {0};
    uint8_t platform_key_y[32] = {0};
    uint8_t pin_hash_enc[64] = {0};  // Can be 16 or 32 bytes (with padding)
    size_t pin_hash_enc_len = 0;
    uint8_t pin_protocol = 2;  // Default to Protocol 2
    bool has_key = false, has_pin = false;

    int map_size = cbor_read_map(&r);
    if (map_size < 0) {
        response[0] = CTAP2_ERR_INVALID_CBOR;
        *response_len = 1;
        return CTAP2_ERR_INVALID_CBOR;
    }

    for (int i = 0; i < map_size; i++) {
        // Read key - could be positive or negative, so use cbor_read_item
        cbor_item_t item;
        if (!cbor_read_item(&r, &item)) {
            LOG_E(TAG_PIN, "Failed to read map key %d", i);
            break;
        }

        int64_t key;
        if (item.type == CBOR_UNSIGNED) {
            key = (int64_t)item.value;
        } else if (item.type == CBOR_NEGATIVE) {
            key = -1 - (int64_t)item.value;
        } else {
            LOG_E(TAG_PIN, "Unexpected key type: %d", item.type);
            cbor_skip_item(&r);
            continue;
        }

        LOG_D(TAG_PIN, "Parsing key: %lld", key);

        switch (key) {
            case CTAP2_PIN_PROTOCOL: {
                uint64_t proto;
                if (cbor_read_uint(&r, &proto)) {
                    pin_protocol = (uint8_t)proto;
                    LOG_I(TAG_PIN, "Client requested protocol: %d", pin_protocol);
                }
                break;
            }
            case CTAP2_PIN_KEY_AGREEMENT: {
                int cose_size = cbor_read_map(&r);
                LOG_D(TAG_PIN, "COSE_Key map size: %d", cose_size);
                if (cose_size < 0) break;
                for (int j = 0; j < cose_size; j++) {
                    // COSE keys can be positive (kty=1, alg=3) or negative (crv=-1, x=-2, y=-3)
                    cbor_item_t cose_item;
                    if (!cbor_read_item(&r, &cose_item)) {
                        LOG_E(TAG_PIN, "Failed to read COSE key %d", j);
                        break;
                    }

                    int64_t cose_key;
                    if (cose_item.type == CBOR_UNSIGNED) {
                        cose_key = (int64_t)cose_item.value;
                    } else if (cose_item.type == CBOR_NEGATIVE) {
                        cose_key = -1 - (int64_t)cose_item.value;
                    } else {
                        LOG_E(TAG_PIN, "Unexpected COSE key type: %d", cose_item.type);
                        cbor_skip_item(&r);
                        continue;
                    }

                    LOG_D(TAG_PIN, "COSE key: %lld", cose_key);

                    if (cose_key == COSE_KEY_LABEL_X) {  // x coordinate
                        size_t x_len;
                        if (cbor_read_bytes(&r, platform_key_x, 32, &x_len) && x_len == 32) {
                            has_key = true;
                            LOG_D(TAG_PIN, "Got x coordinate");
                        }
                    } else if (cose_key == COSE_KEY_LABEL_Y) {  // y coordinate
                        size_t y_len;
                        cbor_read_bytes(&r, platform_key_y, 32, &y_len);
                        LOG_D(TAG_PIN, "Got y coordinate");
                    } else {
                        cbor_skip_item(&r);
                    }
                }
                break;
            }
            case CTAP2_PIN_HASH_ENC: {
                if (cbor_read_bytes(&r, pin_hash_enc, sizeof(pin_hash_enc), &pin_hash_enc_len)) {
                    LOG_D(TAG_PIN, "pinHashEnc read OK, len=%zu", pin_hash_enc_len);
                    if (pin_hash_enc_len == 16 || pin_hash_enc_len == 32 || pin_hash_enc_len == 64) {
                        has_pin = true;
                        LOG_D(TAG_PIN, "Got pinHashEnc (%zu bytes)", pin_hash_enc_len);
                    } else {
                        LOG_E(TAG_PIN, "pinHashEnc unexpected size: %zu", pin_hash_enc_len);
                    }
                } else {
                    LOG_E(TAG_PIN, "Failed to read pinHashEnc bytes");
                }
                break;
            }
            default:
                cbor_skip_item(&r);
                break;
        }
    }

    if (!has_key || !has_pin) {
        LOG_E(TAG_PIN, "Missing keyAgreement or pinHashEnc");
        response[0] = CTAP2_ERR_MISSING_PARAMETER;
        *response_len = 1;
        return CTAP2_ERR_MISSING_PARAMETER;
    }

#if DEBUG_MODE
    // Log received pinHashEnc for debugging
    LOG_I(TAG_PIN, "Received pinHashEnc (%zu bytes):", pin_hash_enc_len);
    for (size_t i = 0; i < pin_hash_enc_len; i += 16) {
        size_t row_len = (pin_hash_enc_len - i < 16) ? (pin_hash_enc_len - i) : 16;
        char hex[64];
        char *p = hex;
        for (size_t j = 0; j < row_len; j++) {
            p += sprintf(p, "%02X ", pin_hash_enc[i + j]);
        }
        LOG_I(TAG_PIN, "  %s", hex);
    }
#endif

    // Compute shared secret
    uint8_t shared_secret[32];
    if (!client_pin_compute_shared_secret(platform_key_x, platform_key_y, pin_protocol, shared_secret)) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

#if DEBUG_MODE
    // Full platform key for verification
    LOG_I(TAG_PIN, "Platform (Chrome) public key X:");
    LOG_I(TAG_PIN, "  %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
          platform_key_x[0], platform_key_x[1], platform_key_x[2], platform_key_x[3],
          platform_key_x[4], platform_key_x[5], platform_key_x[6], platform_key_x[7],
          platform_key_x[8], platform_key_x[9], platform_key_x[10], platform_key_x[11],
          platform_key_x[12], platform_key_x[13], platform_key_x[14], platform_key_x[15]);
    LOG_I(TAG_PIN, "  %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
          platform_key_x[16], platform_key_x[17], platform_key_x[18], platform_key_x[19],
          platform_key_x[20], platform_key_x[21], platform_key_x[22], platform_key_x[23],
          platform_key_x[24], platform_key_x[25], platform_key_x[26], platform_key_x[27],
          platform_key_x[28], platform_key_x[29], platform_key_x[30], platform_key_x[31]);
    LOG_I(TAG_PIN, "Platform (Chrome) public key Y:");
    LOG_I(TAG_PIN, "  %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
          platform_key_y[0], platform_key_y[1], platform_key_y[2], platform_key_y[3],
          platform_key_y[4], platform_key_y[5], platform_key_y[6], platform_key_y[7],
          platform_key_y[8], platform_key_y[9], platform_key_y[10], platform_key_y[11],
          platform_key_y[12], platform_key_y[13], platform_key_y[14], platform_key_y[15]);
    LOG_I(TAG_PIN, "  %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
          platform_key_y[16], platform_key_y[17], platform_key_y[18], platform_key_y[19],
          platform_key_y[20], platform_key_y[21], platform_key_y[22], platform_key_y[23],
          platform_key_y[24], platform_key_y[25], platform_key_y[26], platform_key_y[27],
          platform_key_y[28], platform_key_y[29], platform_key_y[30], platform_key_y[31]);
#endif

    // Decrypt pinHashEnc
    // Protocol 1: 16 bytes ciphertext with IV=0
    // Protocol 2: 32 bytes = IV (16) || ciphertext (16)
    uint8_t decrypted_pin_hash[16];

    if (pin_protocol == 2 && pin_hash_enc_len == 32) {
        // Protocol 2: first 16 bytes are IV, next 16 are ciphertext
        const uint8_t *iv = pin_hash_enc;
        const uint8_t *ciphertext = pin_hash_enc + 16;

        LOG_D(TAG_PIN, "Protocol 2 IV: %02X%02X%02X%02X %02X%02X%02X%02X...",
              iv[0], iv[1], iv[2], iv[3], iv[4], iv[5], iv[6], iv[7]);
        LOG_D(TAG_PIN, "Ciphertext:    %02X%02X%02X%02X %02X%02X%02X%02X...",
              ciphertext[0], ciphertext[1], ciphertext[2], ciphertext[3],
              ciphertext[4], ciphertext[5], ciphertext[6], ciphertext[7]);

        if (!aes_256_cbc_decrypt_iv(shared_secret, iv, ciphertext, 16, decrypted_pin_hash)) {
            LOG_E(TAG_PIN, "PIN decryption failed");
            response[0] = CTAP2_ERR_OTHER;
            *response_len = 1;
            return CTAP2_ERR_OTHER;
        }
    } else {
        // Protocol 1: IV is all zeros
        uint8_t decrypted[64];
        if (!aes_256_cbc_decrypt(shared_secret, pin_hash_enc, pin_hash_enc_len, decrypted)) {
            LOG_E(TAG_PIN, "PIN decryption failed");
            response[0] = CTAP2_ERR_OTHER;
            *response_len = 1;
            return CTAP2_ERR_OTHER;
        }
        memcpy(decrypted_pin_hash, decrypted, 16);
    }

#if DEBUG_MODE
    LOG_D(TAG_PIN, "Decrypted PIN hash: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
          decrypted_pin_hash[0], decrypted_pin_hash[1], decrypted_pin_hash[2], decrypted_pin_hash[3],
          decrypted_pin_hash[4], decrypted_pin_hash[5], decrypted_pin_hash[6], decrypted_pin_hash[7],
          decrypted_pin_hash[8], decrypted_pin_hash[9], decrypted_pin_hash[10], decrypted_pin_hash[11],
          decrypted_pin_hash[12], decrypted_pin_hash[13], decrypted_pin_hash[14], decrypted_pin_hash[15]);

    // Get stored hash for comparison
    uint8_t stored_hash[16];
    pin_storage_get_fido2_hash(stored_hash);
    LOG_D(TAG_PIN, "Stored FIDO2 hash:  %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
          stored_hash[0], stored_hash[1], stored_hash[2], stored_hash[3],
          stored_hash[4], stored_hash[5], stored_hash[6], stored_hash[7],
          stored_hash[8], stored_hash[9], stored_hash[10], stored_hash[11],
          stored_hash[12], stored_hash[13], stored_hash[14], stored_hash[15]);

    // Debug: compute expected hash for "0000"
    uint8_t test_full[32];
    sha256((const uint8_t*)"0000", 4, test_full);
    LOG_D(TAG_PIN, "Expected for 0000: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
          test_full[0], test_full[1], test_full[2], test_full[3],
          test_full[4], test_full[5], test_full[6], test_full[7],
          test_full[8], test_full[9], test_full[10], test_full[11],
          test_full[12], test_full[13], test_full[14], test_full[15]);
#endif

    // Verify PIN hash
    if (!pin_storage_verify_fido2_hash(decrypted_pin_hash)) {
        g_client_pin.pin_retries--;
        LOG_W(TAG_PIN, "Invalid PIN, retries left: %d", g_client_pin.pin_retries);

        if (g_client_pin.pin_retries == 0) {
            response[0] = CTAP2_ERR_PIN_BLOCKED;
        } else {
            response[0] = CTAP2_ERR_PIN_INVALID;
        }
        *response_len = 1;
        return response[0];
    }

    // PIN correct - reset retries and generate pinToken
    g_client_pin.pin_retries = PIN_RETRIES_MAX;
    secure_random_fill(g_client_pin.pin_token, PIN_TOKEN_SIZE);
    g_client_pin.pin_token_valid = true;

    // Encrypt pinToken with shared secret
    // Protocol 1: IV=0, returns ciphertext only (32 bytes)
    // Protocol 2: returns IV || ciphertext (16 + 32 = 48 bytes)
    uint8_t encrypted_token[PIN_TOKEN_SIZE + 16];  // Extra space for IV in Protocol 2
    size_t encrypted_len;

    if (pin_protocol == 2) {
        if (!aes_256_cbc_encrypt_p2(shared_secret, g_client_pin.pin_token, PIN_TOKEN_SIZE, encrypted_token)) {
            response[0] = CTAP2_ERR_OTHER;
            *response_len = 1;
            return CTAP2_ERR_OTHER;
        }
        encrypted_len = PIN_TOKEN_SIZE + 16;  // IV + ciphertext
        LOG_D(TAG_PIN, "Encrypted pinToken (Protocol 2, %zu bytes with IV)", encrypted_len);
    } else {
        if (!aes_256_cbc_encrypt(shared_secret, g_client_pin.pin_token, PIN_TOKEN_SIZE, encrypted_token)) {
            response[0] = CTAP2_ERR_OTHER;
            *response_len = 1;
            return CTAP2_ERR_OTHER;
        }
        encrypted_len = PIN_TOKEN_SIZE;
        LOG_D(TAG_PIN, "Encrypted pinToken (Protocol 1, %zu bytes)", encrypted_len);
    }

    // Build response
    cbor_writer_t w;
    cbor_writer_init(&w, response + 1, *response_len - 1);

    cbor_encode_map(&w, 1);

    // pinUvAuthToken (encrypted)
    cbor_encode_uint(&w, CTAP2_PIN_RESP_PIN_TOKEN);
    cbor_encode_bytes(&w, encrypted_token, encrypted_len);

    response[0] = CTAP2_OK;
    *response_len = 1 + cbor_writer_length(&w);

    // Legacy token (0x05) has all permissions
    g_client_pin.token_permissions = 0xFF;
    g_client_pin.token_rp_id_set = false;

    LOG_I(TAG_PIN, "PIN verified, token issued (legacy, all permissions)");
    return CTAP2_OK;
}

/**
 * \brief Handles ClientPIN subcommand `getPinUvAuthTokenUsingPinWithPermissions` (`0x09`).
 * \param params CBOR request payload.
 * \param params_len Length of `params`.
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
static uint8_t client_pin_get_pin_uv_auth_token(const uint8_t *params, uint16_t params_len,
                                                 uint8_t *response, uint16_t *response_len) {
    // Check if PIN is blocked
    if (g_client_pin.pin_retries == 0) {
        response[0] = CTAP2_ERR_PIN_BLOCKED;
        *response_len = 1;
        return CTAP2_ERR_PIN_BLOCKED;
    }

    // Check if FIDO2 PIN hash is available
    if (!pin_storage_fido2_available()) {
        LOG_E(TAG_PIN, "FIDO2 hash not available - user must reset PIN");
        response[0] = CTAP2_ERR_PIN_NOT_SET;
        *response_len = 1;
        return CTAP2_ERR_PIN_NOT_SET;
    }

    // Parse parameters
    cbor_reader_t r;
    cbor_reader_init(&r, params, params_len);

    uint8_t platform_key_x[32] = {0};
    uint8_t platform_key_y[32] = {0};
    uint8_t pin_hash_enc[64] = {0};
    size_t pin_hash_enc_len = 0;
    uint8_t pin_protocol = 2;
    uint8_t permissions = 0;
    char rp_id[64] = {0};
    bool has_key = false, has_pin = false, has_permissions = false;

    int map_size = cbor_read_map(&r);
    if (map_size < 0) {
        response[0] = CTAP2_ERR_INVALID_CBOR;
        *response_len = 1;
        return CTAP2_ERR_INVALID_CBOR;
    }

    for (int i = 0; i < map_size; i++) {
        cbor_item_t item;
        if (!cbor_read_item(&r, &item)) break;

        int64_t key;
        if (item.type == CBOR_UNSIGNED) {
            key = (int64_t)item.value;
        } else if (item.type == CBOR_NEGATIVE) {
            key = -1 - (int64_t)item.value;
        } else {
            cbor_skip_item(&r);
            continue;
        }

        switch (key) {
            case CTAP2_PIN_PROTOCOL: {
                uint64_t proto;
                if (cbor_read_uint(&r, &proto)) {
                    pin_protocol = (uint8_t)proto;
                }
                break;
            }
            case CTAP2_PIN_KEY_AGREEMENT: {
                int cose_size = cbor_read_map(&r);
                if (cose_size < 0) break;
                for (int j = 0; j < cose_size; j++) {
                    cbor_item_t cose_item;
                    if (!cbor_read_item(&r, &cose_item)) break;

                    int64_t cose_key;
                    if (cose_item.type == CBOR_UNSIGNED) {
                        cose_key = (int64_t)cose_item.value;
                    } else if (cose_item.type == CBOR_NEGATIVE) {
                        cose_key = -1 - (int64_t)cose_item.value;
                    } else {
                        cbor_skip_item(&r);
                        continue;
                    }

                    if (cose_key == COSE_KEY_LABEL_X) {  // x coordinate
                        size_t x_len;
                        if (cbor_read_bytes(&r, platform_key_x, 32, &x_len) && x_len == 32) {
                            has_key = true;
                        }
                    } else if (cose_key == COSE_KEY_LABEL_Y) {  // y coordinate
                        size_t y_len;
                        cbor_read_bytes(&r, platform_key_y, 32, &y_len);
                    } else {
                        cbor_skip_item(&r);
                    }
                }
                break;
            }
            case CTAP2_PIN_HASH_ENC: {
                if (cbor_read_bytes(&r, pin_hash_enc, sizeof(pin_hash_enc), &pin_hash_enc_len)) {
                    if (pin_hash_enc_len == 16 || pin_hash_enc_len == 32 || pin_hash_enc_len == 64) {
                        has_pin = true;
                    }
                }
                break;
            }
            case CTAP2_PIN_PERMISSIONS: {
                uint64_t perm;
                if (cbor_read_uint(&r, &perm)) {
                    permissions = (uint8_t)perm;
                    has_permissions = true;
                    LOG_I(TAG_PIN, "Requested permissions: 0x%02X", permissions);
                }
                break;
            }
            case CTAP2_PIN_PERMISSIONS_RPID: {
                size_t rp_len;
                if (cbor_read_text(&r, rp_id, sizeof(rp_id) - 1, &rp_len)) {
                    LOG_I(TAG_PIN, "Requested rpId: %s", rp_id);
                }
                break;
            }
            default:
                cbor_skip_item(&r);
                break;
        }
    }

    if (!has_key || !has_pin) {
        LOG_E(TAG_PIN, "Missing keyAgreement or pinHashEnc");
        response[0] = CTAP2_ERR_MISSING_PARAMETER;
        *response_len = 1;
        return CTAP2_ERR_MISSING_PARAMETER;
    }

    if (!has_permissions) {
        LOG_E(TAG_PIN, "Missing permissions parameter");
        response[0] = CTAP2_ERR_MISSING_PARAMETER;
        *response_len = 1;
        return CTAP2_ERR_MISSING_PARAMETER;
    }

    // Compute shared secret
    uint8_t shared_secret[32];
    if (!client_pin_compute_shared_secret(platform_key_x, platform_key_y, pin_protocol, shared_secret)) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    // Decrypt pinHashEnc
    uint8_t decrypted_pin_hash[16];
    if (pin_protocol == 2 && pin_hash_enc_len == 32) {
        const uint8_t *iv = pin_hash_enc;
        const uint8_t *ciphertext = pin_hash_enc + 16;
        if (!aes_256_cbc_decrypt_iv(shared_secret, iv, ciphertext, 16, decrypted_pin_hash)) {
            LOG_E(TAG_PIN, "PIN decryption failed");
            response[0] = CTAP2_ERR_OTHER;
            *response_len = 1;
            return CTAP2_ERR_OTHER;
        }
    } else {
        uint8_t decrypted[64];
        if (!aes_256_cbc_decrypt(shared_secret, pin_hash_enc, pin_hash_enc_len, decrypted)) {
            LOG_E(TAG_PIN, "PIN decryption failed");
            response[0] = CTAP2_ERR_OTHER;
            *response_len = 1;
            return CTAP2_ERR_OTHER;
        }
        memcpy(decrypted_pin_hash, decrypted, 16);
    }

    // Verify PIN hash
    if (!pin_storage_verify_fido2_hash(decrypted_pin_hash)) {
        g_client_pin.pin_retries--;
        LOG_W(TAG_PIN, "Invalid PIN, retries left: %d", g_client_pin.pin_retries);
        response[0] = (g_client_pin.pin_retries == 0) ? CTAP2_ERR_PIN_BLOCKED : CTAP2_ERR_PIN_INVALID;
        *response_len = 1;
        return response[0];
    }

    // PIN correct - reset retries and generate pinToken
    g_client_pin.pin_retries = PIN_RETRIES_MAX;
    secure_random_fill(g_client_pin.pin_token, PIN_TOKEN_SIZE);
    g_client_pin.pin_token_valid = true;

    // Store permissions
    g_client_pin.token_permissions = permissions;
    if (rp_id[0]) {
        sha256_str(rp_id, g_client_pin.token_rp_id_hash);
        g_client_pin.token_rp_id_set = true;
    } else {
        g_client_pin.token_rp_id_set = false;
    }

    // Encrypt pinToken
    uint8_t encrypted_token[PIN_TOKEN_SIZE + 16];
    size_t encrypted_len;

    if (pin_protocol == 2) {
        if (!aes_256_cbc_encrypt_p2(shared_secret, g_client_pin.pin_token, PIN_TOKEN_SIZE, encrypted_token)) {
            response[0] = CTAP2_ERR_OTHER;
            *response_len = 1;
            return CTAP2_ERR_OTHER;
        }
        encrypted_len = PIN_TOKEN_SIZE + 16;
    } else {
        if (!aes_256_cbc_encrypt(shared_secret, g_client_pin.pin_token, PIN_TOKEN_SIZE, encrypted_token)) {
            response[0] = CTAP2_ERR_OTHER;
            *response_len = 1;
            return CTAP2_ERR_OTHER;
        }
        encrypted_len = PIN_TOKEN_SIZE;
    }

    // Build response
    cbor_writer_t w;
    cbor_writer_init(&w, response + 1, *response_len - 1);

    cbor_encode_map(&w, 1);
    cbor_encode_uint(&w, CTAP2_PIN_RESP_PIN_TOKEN);
    cbor_encode_bytes(&w, encrypted_token, encrypted_len);

    response[0] = CTAP2_OK;
    *response_len = 1 + cbor_writer_length(&w);
    LOG_I(TAG_PIN, "PIN verified, token issued with permissions=0x%02X", permissions);
    return CTAP2_OK;
}

/**
 * \brief Handles CTAP2 `authenticatorClientPIN` (`0x06`).
 * \param params CBOR request payload.
 * \param params_len Length of `params`.
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
uint8_t ctap2_client_pin(const uint8_t *params, uint16_t params_len,
                          uint8_t *response, uint16_t *response_len) {
    // Initialize if needed
    if (!g_client_pin.initialized) {
        g_client_pin.pin_retries = PIN_RETRIES_MAX;
        g_client_pin.uv_retries = PIN_UV_RETRIES_MAX;
        g_client_pin.initialized = true;
    }

    // Parse subCommand
    cbor_reader_t r;
    cbor_reader_init(&r, params, params_len);

    int map_size = cbor_read_map(&r);
    if (map_size < 0) {
        response[0] = CTAP2_ERR_INVALID_CBOR;
        *response_len = 1;
        return CTAP2_ERR_INVALID_CBOR;
    }

    uint64_t pin_protocol = 0;
    uint64_t sub_command = 0;

    for (int i = 0; i < map_size; i++) {
        uint64_t key;
        if (!cbor_read_uint(&r, &key)) break;

        if (key == 0x01) {  // pinUvAuthProtocol
            cbor_read_uint(&r, &pin_protocol);
        } else if (key == 0x02) {  // subCommand
            cbor_read_uint(&r, &sub_command);
        } else {
            cbor_skip_item(&r);
        }
    }

    LOG_I(TAG_PIN, "ClientPIN: protocol=%llu, subCommand=0x%02llx", pin_protocol, sub_command);

    // We only support protocol 2
    if (pin_protocol != 0 && pin_protocol != PIN_PROTOCOL_VERSION) {
        response[0] = CTAP1_ERR_INVALID_PARAMETER;
        *response_len = 1;
        return CTAP1_ERR_INVALID_PARAMETER;
    }

    switch (sub_command) {
        case PIN_CMD_GET_RETRIES:
            return client_pin_get_retries(response, response_len);

        case PIN_CMD_GET_KEY_AGREEMENT:
            return client_pin_get_key_agreement(response, response_len);

        case PIN_CMD_GET_PIN_TOKEN:
            return client_pin_get_pin_token(params, params_len, response, response_len);

        case PIN_CMD_GET_PIN_UV_TOKEN:
            return client_pin_get_pin_uv_auth_token(params, params_len, response, response_len);

        case PIN_CMD_SET_PIN:
        case PIN_CMD_CHANGE_PIN:
            // Not supported - PIN is set via badge UI
            response[0] = CTAP2_ERR_UNSUPPORTED_OPTION;
            *response_len = 1;
            return CTAP2_ERR_UNSUPPORTED_OPTION;

        default:
            LOG_W(TAG_PIN, "Unknown subCommand: 0x%02lx", sub_command);
            response[0] = CTAP1_ERR_INVALID_COMMAND;
            *response_len = 1;
            return CTAP1_ERR_INVALID_COMMAND;
    }
}

/**
 * \brief Handles CTAP2 `authenticatorReset` (`0x07`).
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
uint8_t ctap2_reset(uint8_t *response, uint16_t *response_len) {
    // CTAP2.1 6.4: reset requires explicit user presence.
    if (!wait_for_user_presence(NULL, FIDO2_ACTION_AUTHENTICATE, NULL)) {
        response[0] = CTAP2_ERR_OPERATION_DENIED;
        *response_len = 1;
        return CTAP2_ERR_OPERATION_DENIED;
    }
    if (!fido2_factory_reset()) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    response[0] = CTAP2_OK;
    *response_len = 1;
    LOG_I(TAG, "Factory reset complete");
    return CTAP2_OK;
}

/** \brief Credential-management helper and command implementation. */
/**
 * \brief Checks whether the slot's secure-element key material is present.
 * \param slot Logical slot index.
 * \return `true` if the public key is readable from the secure element.
 */
static bool cred_mgmt_slot_has_key(uint8_t slot) {
    uint8_t pubkey[64];
    if (!fido2_storage_get_pubkey(slot, pubkey)) {
        LOG_W(TAG, "credMgmt: skipping slot %d (no SE key)", slot);
        return false;
    }
    return true;
}

/**
 * \brief Counts unique RP IDs among resident credentials.
 * \return Number of unique relying parties.
 */
static uint8_t cred_mgmt_count_unique_rps(void) {
    // Large buffer in PSRAM (32 * 32 = 1024 bytes)
    EXT_RAM_BSS_ATTR static uint8_t unique_hashes[FIDO2_MAX_CREDENTIALS][32];
    uint8_t count = 0;

    for (uint8_t slot = 0; slot < FIDO2_MAX_CREDENTIALS; slot++) {
        if (!fido2_storage_is_resident(slot)) continue;

        fido2_credential_info_t info;
        if (!fido2_storage_get_credential(slot, &info)) continue;
        if (!cred_mgmt_slot_has_key(slot)) continue;

        // Check if this RP hash is already in our list
        bool found = false;
        for (uint8_t j = 0; j < count; j++) {
            if (memcmp(unique_hashes[j], info.rp_id_hash, 32) == 0) {
                found = true;
                break;
            }
        }

        if (!found && count < FIDO2_MAX_CREDENTIALS) {
            memcpy(unique_hashes[count], info.rp_id_hash, 32);
            g_cred_mgmt.rp_slots[count] = slot;  // Store a representative slot
            count++;
        }
    }

    return count;
}

/**
 * \brief Collects resident credentials for the given RP ID hash.
 * \param rp_id_hash 32-byte RP ID hash to match.
 * \return Number of matching credentials.
 */
static uint8_t cred_mgmt_find_creds_for_rp(const uint8_t *rp_id_hash) {
    uint8_t count = 0;

    for (uint8_t slot = 0; slot < FIDO2_MAX_CREDENTIALS && count < FIDO2_MAX_CREDENTIALS; slot++) {
        if (!fido2_storage_is_resident(slot)) continue;

        fido2_credential_info_t info;
        if (!fido2_storage_get_credential(slot, &info)) continue;

        if (memcmp(info.rp_id_hash, rp_id_hash, 32) != 0) continue;
        if (!cred_mgmt_slot_has_key(slot)) continue;

        g_cred_mgmt.cred_slots[count++] = slot;
    }

    return count;
}

/**
 * \brief Encodes a credential-management RP response entry.
 * \param w CBOR writer for output encoding.
 * \param slot Credential slot used as RP representative.
 * \param include_total Whether to include total RP count.
 * \return `true` if the entry was encoded.
 */
static bool cred_mgmt_encode_rp(cbor_writer_t *w, uint8_t slot, bool include_total) {
    fido2_credential_info_t info;
    if (!fido2_storage_get_credential(slot, &info)) return false;

    // Map with 2 or 3 entries
    cbor_encode_map(w, include_total ? 3 : 2);

    // rp (map with id)
    cbor_encode_uint(w, CTAP2_CM_RESP_RP);
    cbor_encode_map(w, 1);
    cbor_encode_text(w, "id");
    cbor_encode_text(w, info.rp_id);

    // rpIDHash
    cbor_encode_uint(w, CTAP2_CM_RESP_RP_ID_HASH);
    cbor_encode_bytes(w, info.rp_id_hash, 32);

    // totalRPs (only in first response)
    if (include_total) {
        cbor_encode_uint(w, CTAP2_CM_RESP_TOTAL_RPS);
        cbor_encode_uint(w, g_cred_mgmt.rp_count);
    }

    return true;
}

/**
 * \brief Encodes a credential-management credential response entry.
 * \param w CBOR writer for output encoding.
 * \param slot Credential slot to encode.
 * \param include_total Whether to include total credential count.
 * \return `true` if the entry was encoded.
 */
static bool cred_mgmt_encode_credential(cbor_writer_t *w, uint8_t slot, bool include_total) {
    fido2_credential_info_t info;
    if (!fido2_storage_get_credential(slot, &info)) return false;

    uint8_t cred_id[FIDO2_CRED_ID_LEN];
    if (!fido2_storage_get_cred_id(slot, cred_id)) return false;

    uint8_t pubkey[64];
    if (!fido2_storage_get_pubkey(slot, pubkey)) return false;

    // Map with 4 or 5 entries
    cbor_encode_map(w, include_total ? 5 : 4);

    // user
    cbor_encode_uint(w, CTAP2_CM_RESP_USER);
    cbor_encode_map(w, info.user_name[0] ? 2 : 1);
    cbor_encode_text(w, "id");
    cbor_encode_bytes(w, info.user_id, info.user_id_len);
    if (info.user_name[0]) {
        cbor_encode_text(w, "name");
        cbor_encode_text(w, info.user_name);
    }

    // credentialID (PublicKeyCredentialDescriptor)
    cbor_encode_uint(w, CTAP2_CM_RESP_CREDENTIAL_ID);
    cbor_encode_map(w, 2);
    // Canonical order: "id" (len 2) before "type" (len 4)
    cbor_encode_text(w, "id");
    cbor_encode_bytes(w, cred_id, FIDO2_CRED_ID_LEN);
    cbor_encode_text(w, "type");
    cbor_encode_text(w, "public-key");

    // publicKey (COSE_Key, RFC 8152)
    cbor_encode_uint(w, CTAP2_CM_RESP_PUBLIC_KEY);
    if (info.curve == CDC_CURVE_ED25519) {
        cbor_encode_cose_key_ed25519(w, pubkey);
    } else {
        cbor_encode_cose_key_p256(w, pubkey, pubkey + 32);
    }

    // totalCredentials (only in first response)
    if (include_total) {
        cbor_encode_uint(w, CTAP2_CM_RESP_TOTAL_CREDENTIALS);
        cbor_encode_uint(w, g_cred_mgmt.cred_count);
    }

    // credProtect
    cbor_encode_uint(w, CTAP2_CM_RESP_CRED_PROTECT);
    cbor_encode_uint(w, info.cred_protect ? info.cred_protect : 1);

    return true;
}

/**
 * \brief Handles CTAP2 `authenticatorCredentialManagement` (`0x0A`).
 * \param params CBOR request payload.
 * \param params_len Length of `params`.
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
uint8_t ctap2_cred_management(const uint8_t *params, uint16_t params_len,
                               uint8_t *response, uint16_t *response_len) {
    // Parse parameters
    if (params_len < 1) {
        response[0] = CTAP2_ERR_INVALID_CBOR;
        *response_len = 1;
        return CTAP2_ERR_INVALID_CBOR;
    }

    cbor_reader_t r;
    cbor_reader_init(&r, params, params_len);

    int map_count = cbor_read_map(&r);
    if (map_count < 1) {
        response[0] = CTAP2_ERR_INVALID_CBOR;
        *response_len = 1;
        return CTAP2_ERR_INVALID_CBOR;
    }

    uint8_t subcommand = 0;
    uint8_t rp_id_hash[32] = {0};
    bool has_rp_id_hash = false;
    uint8_t cred_id[FIDO2_CRED_ID_LEN] = {0};
    uint16_t cred_id_len = 0;
    bool has_cred_id = false;

    // Parse map entries
    for (int i = 0; i < map_count; i++) {
        uint64_t key;
        if (!cbor_read_uint(&r, &key)) {
            cbor_skip_item(&r);
            continue;
        }

        switch (key) {
            case CTAP2_CM_SUBCOMMAND:
                {
                    uint64_t cmd;
                    if (cbor_read_uint(&r, &cmd)) {
                        subcommand = (uint8_t)cmd;
                    }
                }
                break;

            case CTAP2_CM_SUBCOMMAND_PARAMS:
                {
                    int sub_count = cbor_read_map(&r);
                    for (int j = 0; j < sub_count; j++) {
                        uint64_t sub_key;
                        if (!cbor_read_uint(&r, &sub_key)) {
                            cbor_skip_item(&r);
                            cbor_skip_item(&r);
                            continue;
                        }

                        if (sub_key == CTAP2_CM_SUB_RP_ID_HASH) {
                            size_t len;
                            if (cbor_read_bytes(&r, rp_id_hash, 32, &len) && len == 32) {
                                has_rp_id_hash = true;
                            }
                        } else if (sub_key == CTAP2_CM_SUB_CREDENTIAL_ID) {
                            int cred_map = cbor_read_map(&r);
                            for (int k = 0; k < cred_map; k++) {
                                char cred_key[16];
                                size_t key_len;
                                if (cbor_read_text(&r, cred_key, sizeof(cred_key), &key_len)) {
                                    if (strcmp(cred_key, "id") == 0) {
                                        size_t len;
                                        if (cbor_read_bytes(&r, cred_id, FIDO2_CRED_ID_LEN, &len)) {
                                            cred_id_len = len;
                                            has_cred_id = true;
                                        }
                                    } else {
                                        cbor_skip_item(&r);
                                    }
                                } else {
                                    cbor_skip_item(&r);
                                    cbor_skip_item(&r);
                                }
                            }
                        } else {
                            cbor_skip_item(&r);
                        }
                    }
                }
                break;

            case CTAP2_CM_PIN_UV_AUTH_PROTOCOL:
            case CTAP2_CM_PIN_UV_AUTH_PARAM:
                // We skip PIN auth verification for now
                // In production, should verify pinUvAuthParam
                cbor_skip_item(&r);
                break;

            default:
                cbor_skip_item(&r);
                break;
        }
    }

    LOG_I(TAG, "credMgmt subCmd=0x%02X", subcommand);

    // CTAP2.1 6.8: credentialManagement requires a valid pinUvAuthToken. Block
    // unauthenticated enumeration/deletion of resident credentials.
    if (!g_client_pin.pin_token_valid) {
        response[0] = CTAP2_ERR_PIN_AUTH_INVALID;
        *response_len = 1;
        return CTAP2_ERR_PIN_AUTH_INVALID;
    }

    cbor_writer_t w;
    cbor_writer_init(&w, response + 1, *response_len - 1);

    switch (subcommand) {
        case CRED_MGMT_GET_CREDS_METADATA:
            {
                // Count resident credentials
                uint8_t existing = 0;
                for (uint8_t slot = 0; slot < FIDO2_MAX_CREDENTIALS; slot++) {
                    if (fido2_storage_is_resident(slot)) existing++;
                }

                cbor_encode_map(&w, 2);

                // existingResidentCredentialsCount
                cbor_encode_uint(&w, CTAP2_CM_RESP_EXISTING_CRED_COUNT);
                cbor_encode_uint(&w, existing);

                // maxPossibleRemainingResidentCredentialsCount
                cbor_encode_uint(&w, CTAP2_CM_RESP_REMAINING_CRED_COUNT);
                cbor_encode_uint(&w, FIDO2_MAX_CREDENTIALS - existing);

                LOG_I(TAG, "credMgmt metadata: %d existing, %d remaining",
                      existing, FIDO2_MAX_CREDENTIALS - existing);
            }
            break;

        case CRED_MGMT_ENUMERATE_RPS_BEGIN:
            {
                g_cred_mgmt.rp_count = cred_mgmt_count_unique_rps();
                g_cred_mgmt.rp_index = 0;

                if (g_cred_mgmt.rp_count == 0) {
                    response[0] = CTAP2_ERR_NO_CREDENTIALS;
                    *response_len = 1;
                    return CTAP2_ERR_NO_CREDENTIALS;
                }

                if (!cred_mgmt_encode_rp(&w, g_cred_mgmt.rp_slots[0], true)) {
                    response[0] = CTAP2_ERR_OTHER;
                    *response_len = 1;
                    return CTAP2_ERR_OTHER;
                }
                g_cred_mgmt.rp_index = 1;

                LOG_I(TAG, "credMgmt enumerateRPs: %d unique RPs", g_cred_mgmt.rp_count);
            }
            break;

        case CRED_MGMT_ENUMERATE_RPS_GET_NEXT:
            {
                if (g_cred_mgmt.rp_index >= g_cred_mgmt.rp_count) {
                    response[0] = CTAP2_ERR_NO_CREDENTIALS;
                    *response_len = 1;
                    return CTAP2_ERR_NO_CREDENTIALS;
                }

                if (!cred_mgmt_encode_rp(&w, g_cred_mgmt.rp_slots[g_cred_mgmt.rp_index], false)) {
                    response[0] = CTAP2_ERR_OTHER;
                    *response_len = 1;
                    return CTAP2_ERR_OTHER;
                }
                g_cred_mgmt.rp_index++;
            }
            break;

        case CRED_MGMT_ENUMERATE_CREDS_BEGIN:
            {
                if (!has_rp_id_hash) {
                    response[0] = CTAP2_ERR_MISSING_PARAMETER;
                    *response_len = 1;
                    return CTAP2_ERR_MISSING_PARAMETER;
                }

                memcpy(g_cred_mgmt.current_rp_id_hash, rp_id_hash, 32);
                g_cred_mgmt.cred_count = cred_mgmt_find_creds_for_rp(rp_id_hash);
                g_cred_mgmt.cred_index = 0;

                if (g_cred_mgmt.cred_count == 0) {
                    response[0] = CTAP2_ERR_NO_CREDENTIALS;
                    *response_len = 1;
                    return CTAP2_ERR_NO_CREDENTIALS;
                }

                if (!cred_mgmt_encode_credential(&w, g_cred_mgmt.cred_slots[0], true)) {
                    response[0] = CTAP2_ERR_OTHER;
                    *response_len = 1;
                    return CTAP2_ERR_OTHER;
                }
                g_cred_mgmt.cred_index = 1;

                LOG_I(TAG, "credMgmt enumerateCreds: %d credentials for RP", g_cred_mgmt.cred_count);
            }
            break;

        case CRED_MGMT_ENUMERATE_CREDS_GET_NEXT:
            {
                if (g_cred_mgmt.cred_index >= g_cred_mgmt.cred_count) {
                    response[0] = CTAP2_ERR_NO_CREDENTIALS;
                    *response_len = 1;
                    return CTAP2_ERR_NO_CREDENTIALS;
                }

                if (!cred_mgmt_encode_credential(&w, g_cred_mgmt.cred_slots[g_cred_mgmt.cred_index], false)) {
                    response[0] = CTAP2_ERR_OTHER;
                    *response_len = 1;
                    return CTAP2_ERR_OTHER;
                }
                g_cred_mgmt.cred_index++;
            }
            break;

        case CRED_MGMT_DELETE_CREDENTIAL:
            {
                if (!has_cred_id) {
                    response[0] = CTAP2_ERR_MISSING_PARAMETER;
                    *response_len = 1;
                    return CTAP2_ERR_MISSING_PARAMETER;
                }

                // Find credential by ID
                int8_t slot = fido2_storage_find_slot_by_cred_id(cred_id, cred_id_len);
                if (slot < 0) {
                    response[0] = CTAP2_ERR_NO_CREDENTIALS;
                    *response_len = 1;
                    return CTAP2_ERR_NO_CREDENTIALS;
                }

                // Delete it
                if (!fido2_storage_delete_credential(slot)) {
                    response[0] = CTAP2_ERR_OTHER;
                    *response_len = 1;
                    return CTAP2_ERR_OTHER;
                }

                LOG_I(TAG, "credMgmt deleted credential slot %d", slot);

                // Success - empty response
                response[0] = CTAP2_OK;
                *response_len = 1;
                return CTAP2_OK;
            }

        default:
            response[0] = CTAP2_ERR_UNSUPPORTED_OPTION;
            *response_len = 1;
            return CTAP2_ERR_UNSUPPORTED_OPTION;
    }

    if (cbor_writer_error(&w)) {
        response[0] = CTAP2_ERR_OTHER;
        *response_len = 1;
        return CTAP2_ERR_OTHER;
    }

    response[0] = CTAP2_OK;
    *response_len = 1 + cbor_writer_length(&w);
    return CTAP2_OK;
}

/**
 * \brief Handles CTAP2 `authenticatorSelection` (`0x0B`).
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2 status code.
 */
uint8_t ctap2_selection(uint8_t *response, uint16_t *response_len) {
    // Selection just requires user presence
    if (!wait_for_user_presence(NULL, FIDO2_ACTION_AUTHENTICATE, NULL)) {
        response[0] = CTAP2_ERR_OPERATION_DENIED;
        *response_len = 1;
        return CTAP2_ERR_OPERATION_DENIED;
    }

    response[0] = CTAP2_OK;
    *response_len = 1;
    return CTAP2_OK;
}

/**
 * \brief Initializes CTAP2 runtime state.
 * \return `true` on success.
 */
bool ctap2_init(void) {
    LOG_I(TAG, "Initializing...");
    memset(&g_ctap2, 0, sizeof(g_ctap2));
    g_ctap2.initialized = true;
    LOG_I(TAG, "Initialized");
    return true;
}

/**
 * \brief Dispatches one CTAP2 command and writes response payload.
 * \param cmd Command buffer (`command byte || CBOR params`).
 * \param cmd_len Length of `cmd`.
 * \param response Output response buffer.
 * \param response_len In/out response length.
 * \return CTAP2/CTAP1 status code.
 */
uint8_t ctap2_process_command(const uint8_t *cmd, uint16_t cmd_len,
                               uint8_t *response, uint16_t *response_len) {
    if (!g_ctap2.initialized || cmd_len < 1) {
        response[0] = CTAP1_ERR_INVALID_COMMAND;
        *response_len = 1;
        return CTAP1_ERR_INVALID_COMMAND;
    }

    uint8_t command = cmd[0];
    const uint8_t *params = cmd + 1;
    uint16_t params_len = cmd_len - 1;

    // Always log command type (helpful for debugging protocol issues)
    const char *cmd_name = "?";
    switch (command) {
        case CTAP2_CMD_MAKE_CREDENTIAL:    cmd_name = "makeCredential";    break;
        case CTAP2_CMD_GET_ASSERTION:      cmd_name = "getAssertion";      break;
        case CTAP2_CMD_GET_INFO:           cmd_name = "getInfo";           break;
        case CTAP2_CMD_CLIENT_PIN:         cmd_name = "clientPIN";         break;
        case CTAP2_CMD_RESET:              cmd_name = "reset";             break;
        case CTAP2_CMD_GET_NEXT_ASSERTION: cmd_name = "getNextAssertion";  break;
        case CTAP2_CMD_CRED_MANAGEMENT:    cmd_name = "credMgmt";          break;
        case CTAP2_CMD_SELECTION:          cmd_name = "selection";         break;
    }
    LOG_I(TAG, "CMD 0x%02X (%s) %d bytes", command, cmd_name, params_len);

    g_ctap2.operation_pending = true;
    g_ctap2.cancelled = false;

    uint8_t status;
    switch (command) {
        case CTAP2_CMD_GET_INFO:
            status = ctap2_get_info(response, response_len);
            break;

        case CTAP2_CMD_MAKE_CREDENTIAL:
            status = ctap2_make_credential(params, params_len, response, response_len);
            break;

        case CTAP2_CMD_GET_ASSERTION:
            status = ctap2_get_assertion(params, params_len, response, response_len);
            break;

        case CTAP2_CMD_GET_NEXT_ASSERTION:
            status = ctap2_get_next_assertion(response, response_len);
            break;

        case CTAP2_CMD_CLIENT_PIN:
            status = ctap2_client_pin(params, params_len, response, response_len);
            break;

        case CTAP2_CMD_RESET:
            status = ctap2_reset(response, response_len);
            break;

        case CTAP2_CMD_CRED_MANAGEMENT:
            status = ctap2_cred_management(params, params_len, response, response_len);
            break;

        case CTAP2_CMD_SELECTION:
            status = ctap2_selection(response, response_len);
            break;

        case CTAP2_CMD_LARGE_BLOBS:
        case CTAP2_CMD_CONFIG:
            response[0] = CTAP2_ERR_UNSUPPORTED_OPTION;
            *response_len = 1;
            status = CTAP2_ERR_UNSUPPORTED_OPTION;
            break;

        default:
            response[0] = CTAP1_ERR_INVALID_COMMAND;
            *response_len = 1;
            status = CTAP1_ERR_INVALID_COMMAND;
            break;
    }

    g_ctap2.operation_pending = false;
    return status;
}

/**
 * \brief Sends CTAPHID keepalive for currently active channel.
 * \param status Keepalive status byte.
 */
void ctap2_send_keepalive(uint8_t status) {
    uint32_t cid = ctaphid_get_current_cid();
    if (cid != 0) {
        ctaphid_send_keepalive(cid, status);
    }
}

/**
 * \brief Marks current CTAP2 operation as cancelled.
 */
void ctap2_cancel(void) {
    g_ctap2.cancelled = true;
    if (CTAP2_DEBUG_COMMANDS) LOG_D(TAG, "Operation cancelled");
}

/**
 * \brief Clears any latched cancel flag. Called at the start of a new
 *        CTAPHID channel (INIT) so a cancel from a previous channel does
 *        not abort responses on the new one.
 */
void ctap2_clear_cancel(void) {
    g_ctap2.cancelled = false;
}

/**
 * \brief Returns true if the current CTAP2 operation has been cancelled.
 */
bool ctap2_is_cancelled(void) {
    return g_ctap2.cancelled;
}
