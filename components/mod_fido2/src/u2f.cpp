/**
 * \file
 * \brief Legacy U2F/CTAP1 protocol implementation for compatibility clients.
 */

#include "mod_fido2/u2f.h"
#include "mod_fido2/fido2.h"
#include "mod_fido2/fido2_storage.h"
#include "mod_fido2/fido2_common.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_core/Bytes.h"
#include "cdc_log.h"
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <nvs.h>
#include <esp_attr.h>
#include <string.h>
#include <stdio.h>

using cdc::mod_fido2::sha256;

static const char* TAG = "U2F";

/** \brief DER encoding helper constants for X.509/signature generation. */
static constexpr uint8_t DER_SEQUENCE_TAG = 0x30;
static constexpr uint8_t DER_INTEGER_TAG = 0x02;
static constexpr uint8_t DER_BIT_STRING_TAG = 0x03;
static constexpr uint8_t DER_EXPLICIT_TAG_0 = 0xA0;  // [0] EXPLICIT
static constexpr uint8_t DER_EXPLICIT_TAG_3 = 0xA3;  // [3] EXPLICIT
static constexpr uint8_t DER_LENGTH_TWO_BYTES = 0x82;  // Length uses 2 following bytes

/** \brief ECDSA and EC-point encoding constants. */
static constexpr uint8_t EC_POINT_UNCOMPRESSED = 0x04;  // Uncompressed EC point prefix
static constexpr uint8_t DER_INTEGER_NEGATIVE_MASK = 0x80;  // MSB set = negative in DER
static constexpr uint8_t DER_ENSURE_POSITIVE_MASK = 0x7F;  // Mask to ensure positive
static constexpr int RAW_SIGNATURE_COMPONENT_SIZE = 32;  // Size of R or S in raw signature

/** \brief Attestation certificate constants and cached buffers. */

#define U2F_ATTEST_SLOT 0  // ECC slot 0 reserved for attestation

/** \brief Cached DER attestation certificate and associated state. */
EXT_RAM_BSS_ATTR static uint8_t g_attest_cert[U2F_MAX_ATT_CERT_SIZE];
static uint16_t g_attest_cert_len = 0;
static uint8_t g_attest_pubkey[65];  // 0x04 || X || Y
static bool g_attest_initialized = false;

/** \brief NVS location of an optionally imported (CA-signed) attestation cert. */
static constexpr const char* ATTEST_NVS_NS   = "attest";
static constexpr const char* ATTEST_NVS_CERT = "cert";

/**
 * \brief Checks whether a DER certificate's public key matches the slot-0
 *        attestation key. `g_attest_pubkey` must already be populated.
 * \param der DER-encoded X.509 certificate.
 * \param der_len Certificate length.
 * \return `true` if the certificate's EC point equals the attestation key.
 */
static bool cert_matches_attest_key(const uint8_t* der, size_t der_len) {
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    bool ok = false;
    if (mbedtls_x509_crt_parse_der(&crt, der, der_len) == 0) {
        uint8_t spki[256];
        int n = mbedtls_pk_write_pubkey_der(&crt.pk, spki, sizeof(spki));
        if (n >= 65) {
            // mbedtls writes the DER at the end of the buffer; the uncompressed
            // EC point (0x04 || X || Y) is its trailing 65 bytes.
            ok = memcmp(spki + sizeof(spki) - 65, g_attest_pubkey, 65) == 0;
        }
    }
    mbedtls_x509_crt_free(&crt);
    return ok;
}

/**
 * \brief Loads an imported attestation certificate from NVS when present and
 *        its public key still matches slot 0.
 * \param out Output buffer for the DER certificate.
 * \param out_size Capacity of `out`.
 * \return Certificate length, or 0 when none is usable.
 */
static uint16_t u2f_load_imported_cert(uint8_t* out, size_t out_size) {
    nvs_handle_t nvs;
    if (nvs_open(ATTEST_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return 0;
    size_t len = out_size;
    esp_err_t err = nvs_get_blob(nvs, ATTEST_NVS_CERT, out, &len);
    nvs_close(nvs);
    if (err != ESP_OK || len == 0 || len > out_size) return 0;
    if (!cert_matches_attest_key(out, len)) {
        LOG_W(TAG, "Imported attestation cert does not match slot 0, ignoring");
        return 0;
    }
    return static_cast<uint16_t>(len);
}

/**
 * \brief Encodes a single big-endian unsigned integer as a DER INTEGER element.
 * \param p Output cursor (advances past written bytes).
 * \param mpi Big-endian magnitude buffer.
 * \param len Magnitude length in bytes.
 * \return Updated output cursor positioned after the encoded INTEGER.
 *
 * Strips leading zero bytes (keeping at least one) and prepends a 0x00 padding
 * byte when the MSB is set, ensuring the integer remains non-negative in DER.
 */
static uint8_t* encode_der_integer(uint8_t *p, const uint8_t *mpi, size_t len) {
    // Skip leading zero bytes but keep at least one byte.
    size_t start = 0;
    while (start + 1 < len && mpi[start] == 0) {
        start++;
    }
    size_t actual_len = len - start;

    // Prepend padding byte if MSB is set (DER INTEGERs are signed, two's complement).
    int pad = (mpi[start] & DER_INTEGER_NEGATIVE_MASK) ? 1 : 0;

    *p++ = DER_INTEGER_TAG;
    *p++ = static_cast<uint8_t>(pad + actual_len);
    if (pad) {
        *p++ = 0x00;
    }
    memcpy(p, mpi + start, actual_len);
    return p + actual_len;
}

/**
 * \brief Signs payload hash with attestation key and encodes signature as DER.
 * \param data Data to hash and sign.
 * \param data_len Length of `data`.
 * \param signature Destination buffer for DER signature.
 * \param sig_len Output DER signature length.
 * \return `true` on success, otherwise `false`.
 */
static bool u2f_attest_sign(const uint8_t *data, size_t data_len,
                             uint8_t *signature, uint8_t *sig_len) {
    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) {
        return false;
    }

    uint8_t raw_sig[64];  // R || S (each 32 bytes)
    size_t raw_len = sizeof(raw_sig);
    if (se->ecdsaSign(U2F_ATTEST_SLOT, data, data_len, raw_sig, &raw_len) !=
            cdc::hal::SeResult::OK ||
        raw_len != sizeof(raw_sig)) {
        LOG_E(TAG, "Attestation signing failed");
        return false;
    }

    // DER: SEQUENCE { INTEGER R, INTEGER S }
    // Encode R and S into a scratch buffer first so we can compute the SEQUENCE length.
    uint8_t body[2 * (2 + 1 + RAW_SIGNATURE_COMPONENT_SIZE)];  // tag + len + pad + magnitude, twice
    uint8_t *body_end = encode_der_integer(body, raw_sig, RAW_SIGNATURE_COMPONENT_SIZE);
    body_end = encode_der_integer(body_end, raw_sig + RAW_SIGNATURE_COMPONENT_SIZE,
                                  RAW_SIGNATURE_COMPONENT_SIZE);
    size_t body_len = static_cast<size_t>(body_end - body);

    uint8_t *p = signature;
    *p++ = DER_SEQUENCE_TAG;
    *p++ = static_cast<uint8_t>(body_len);
    memcpy(p, body, body_len);
    p += body_len;

    *sig_len = static_cast<uint8_t>(p - signature);
    return true;
}

/**
 * \brief Initializes attestation key material and builds self-signed attestation certificate.
 * \return `true` on success, otherwise `false`.
 */
bool u2f_init_attestation(void) {
    if (g_attest_initialized) {
        return true;
    }

    LOG_I(TAG, "Initializing attestation...");

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) {
        return false;
    }

    // Check if attestation key exists in slot 0
    uint8_t pubkey[65];
    cdc::hal::EccCurve curve = cdc::hal::EccCurve::P256;
    if (!se->eccSlotUsed(U2F_ATTEST_SLOT)) {
        LOG_E(TAG, "Attestation key missing in slot %d", U2F_ATTEST_SLOT);
        return false;
    }

    if (se->eccGetPublicKey(U2F_ATTEST_SLOT, pubkey, &curve) != cdc::hal::SeResult::OK) {
        LOG_E(TAG, "Failed to read attestation public key");
        return false;
    }

    if (curve != cdc::hal::EccCurve::P256) {
        LOG_E(TAG, "Attestation key has invalid curve");
        return false;
    }

    LOG_I(TAG, "Attestation key ready (curve=P256)");

    // Store public key (with uncompressed point prefix)
    g_attest_pubkey[0] = EC_POINT_UNCOMPRESSED;
    memcpy(g_attest_pubkey + 1, pubkey, 64);

    // Prefer a CA-signed certificate imported for this device; fall back to the
    // self-signed certificate built below.
    uint16_t imported = u2f_load_imported_cert(g_attest_cert, sizeof(g_attest_cert));
    if (imported > 0) {
        g_attest_cert_len = imported;
        g_attest_initialized = true;
        LOG_I(TAG, "Using imported attestation certificate (%u bytes)",
              static_cast<unsigned>(imported));
        return true;
    }

    // Build self-signed X.509 certificate manually (DER encoded)
    // This is a minimal certificate structure for U2F
    uint8_t *cert = g_attest_cert;
    uint8_t *p = cert;

    // We'll build the TBS (To Be Signed) certificate, sign it, then wrap

    // TBS Certificate structure - use static PSRAM buffer (only called once at init)
    EXT_RAM_BSS_ATTR static uint8_t tbs[512];
    uint8_t *t = tbs;

    // Version [0] EXPLICIT INTEGER = 2 (v3)
    *t++ = DER_EXPLICIT_TAG_0; *t++ = 0x03;  // [0] EXPLICIT
    *t++ = DER_INTEGER_TAG; *t++ = 0x01; *t++ = 0x02;  // INTEGER 2

    // Serial number - random
    uint8_t serial[8];
    if (!se->getRandom(serial, sizeof(serial))) {
        LOG_E(TAG, "Failed to get random serial");
        return false;
    }
    serial[0] &= DER_ENSURE_POSITIVE_MASK;  // Ensure positive
    *t++ = DER_INTEGER_TAG; *t++ = 0x08;  // INTEGER
    memcpy(t, serial, 8);
    t += 8;

    // Signature algorithm: ecdsa-with-SHA256 (1.2.840.10045.4.3.2)
    static const uint8_t ecdsa_sha256_oid[] = {
        0x30, 0x0A,  // SEQUENCE
        0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02  // OID
    };
    memcpy(t, ecdsa_sha256_oid, sizeof(ecdsa_sha256_oid));
    t += sizeof(ecdsa_sha256_oid);

    // FIDO2-konformer Subject: C=DE, O=CDC, OU=Authenticator Attestation, CN=CDC Badge FIDO2
    // Total: 13 + 14 + 36 + 26 = 89 bytes content
    static const uint8_t fido2_subject[] = {
        0x30, 0x59,  // SEQUENCE (89 bytes)
        // C=DE (13 bytes: SET(11) = SEQ(9) = OID(5) + PrintableString(2+2))
        0x31, 0x0B, 0x30, 0x09,
        0x06, 0x03, 0x55, 0x04, 0x06,  // OID: C (2.5.4.6)
        0x13, 0x02, 'D', 'E',
        // O=CDC (14 bytes: SET(12) = SEQ(10) = OID(5) + UTF8String(2+3))
        0x31, 0x0C, 0x30, 0x0A,
        0x06, 0x03, 0x55, 0x04, 0x0A,  // OID: O (2.5.4.10)
        0x0C, 0x03, 'C', 'D', 'C',
        // OU=Authenticator Attestation (36 bytes: SET(34) = SEQ(32) = OID(5) + UTF8String(2+25))
        0x31, 0x22, 0x30, 0x20,
        0x06, 0x03, 0x55, 0x04, 0x0B,  // OID: OU (2.5.4.11)
        0x0C, 0x19,
        'A', 'u', 't', 'h', 'e', 'n', 't', 'i', 'c', 'a', 't', 'o', 'r', ' ',
        'A', 't', 't', 'e', 's', 't', 'a', 't', 'i', 'o', 'n',
        // CN=CDC Badge FIDO2 (26 bytes: SET(24) = SEQ(22) = OID(5) + UTF8String(2+15))
        0x31, 0x18, 0x30, 0x16,
        0x06, 0x03, 0x55, 0x04, 0x03,  // OID: CN (2.5.4.3)
        0x0C, 0x0F,
        'C', 'D', 'C', ' ', 'B', 'a', 'd', 'g', 'e', ' ', 'F', 'I', 'D', 'O', '2'
    };
    memcpy(t, fido2_subject, sizeof(fido2_subject));
    t += sizeof(fido2_subject);

    // Validity (2024-01-01 to 2049-12-31)
    // UTCTime: 00-49 = 2000-2049, 50-99 = 1950-1999
    static const uint8_t validity[] = {
        0x30, 0x1E,  // SEQUENCE
        0x17, 0x0D, '2', '4', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z',  // notBefore: 2024-01-01
        0x17, 0x0D, '4', '9', '1', '2', '3', '1', '2', '3', '5', '9', '5', '9', 'Z'   // notAfter:  2049-12-31
    };
    memcpy(t, validity, sizeof(validity));
    t += sizeof(validity);

    // Subject: same as issuer
    memcpy(t, fido2_subject, sizeof(fido2_subject));
    t += sizeof(fido2_subject);

    // Subject Public Key Info
    // AlgorithmIdentifier: ecPublicKey + prime256v1
    static const uint8_t spki_prefix[] = {
        0x30, 0x59,  // SEQUENCE (89 bytes total)
        0x30, 0x13,  // SEQUENCE (algorithm)
        0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01,  // OID: ecPublicKey
        0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07,  // OID: prime256v1
        0x03, 0x42, 0x00  // BIT STRING (66 bytes, 0 unused bits)
    };
    memcpy(t, spki_prefix, sizeof(spki_prefix));
    t += sizeof(spki_prefix);

    // Public key (0x04 || X || Y)
    memcpy(t, g_attest_pubkey, 65);
    t += 65;

    // FIDO2 Extensions: basicConstraints (critical, CA:FALSE) + keyUsage (digitalSignature)
    static const uint8_t fido2_extensions[] = {
        0xA3, 0x1D,  // [3] EXPLICIT (29 bytes)
        0x30, 0x1B,  // SEQUENCE (27 bytes)
        // basicConstraints: critical, CA:FALSE
        0x30, 0x0C,
        0x06, 0x03, 0x55, 0x1D, 0x13,  // OID 2.5.29.19
        0x01, 0x01, 0xFF,              // critical=TRUE
        0x04, 0x02, 0x30, 0x00,        // CA:FALSE
        // keyUsage: digitalSignature
        0x30, 0x0B,
        0x06, 0x03, 0x55, 0x1D, 0x0F,  // OID 2.5.29.15
        0x04, 0x04,
        0x03, 0x02, 0x07, 0x80         // digitalSignature bit
    };
    memcpy(t, fido2_extensions, sizeof(fido2_extensions));
    t += sizeof(fido2_extensions);

    size_t tbs_len = t - tbs;

    // Now wrap TBS in SEQUENCE - use static PSRAM buffer (only called once at init)
    EXT_RAM_BSS_ATTR static uint8_t tbs_wrapped[600];
    uint8_t *tw = tbs_wrapped;

    *tw++ = DER_SEQUENCE_TAG;
    if (tbs_len < 128) {
        *tw++ = tbs_len;
    } else {
        *tw++ = DER_LENGTH_TWO_BYTES;
        *tw++ = (tbs_len >> 8) & 0xFF;
        *tw++ = tbs_len & 0xFF;
    }
    memcpy(tw, tbs, tbs_len);
    tw += tbs_len;

    size_t tbs_wrapped_len = tw - tbs_wrapped;

    // Sign the TBS
    uint8_t sig[U2F_MAX_EC_SIG_SIZE];
    uint8_t sig_len = 0;

    if (!u2f_attest_sign(tbs_wrapped, tbs_wrapped_len, sig, &sig_len)) {
        LOG_E(TAG, "Failed to sign certificate");
        return false;
    }

    // Build complete certificate:
    // SEQUENCE { TBS, SignatureAlgorithm, Signature }
    size_t cert_content_len = tbs_wrapped_len + sizeof(ecdsa_sha256_oid) + 2 + 1 + sig_len;

    *p++ = DER_SEQUENCE_TAG;
    if (cert_content_len < 128) {
        *p++ = cert_content_len;
    } else {
        *p++ = DER_LENGTH_TWO_BYTES;
        *p++ = (cert_content_len >> 8) & 0xFF;
        *p++ = cert_content_len & 0xFF;
    }

    // TBS Certificate (wrapped)
    memcpy(p, tbs_wrapped, tbs_wrapped_len);
    p += tbs_wrapped_len;

    // Signature Algorithm
    memcpy(p, ecdsa_sha256_oid, sizeof(ecdsa_sha256_oid));
    p += sizeof(ecdsa_sha256_oid);

    // Signature BIT STRING
    *p++ = DER_BIT_STRING_TAG;
    *p++ = sig_len + 1;  // length (signature + unused bits byte)
    *p++ = 0x00;  // unused bits
    memcpy(p, sig, sig_len);
    p += sig_len;

    g_attest_cert_len = p - cert;
    g_attest_initialized = true;

    LOG_I(TAG, "FIDO2 attestation certificate generated (%d bytes)", g_attest_cert_len);

    return true;
}

/**
 * \brief Returns attestation certificate pointer and length, initializing
 *        attestation on demand if the boot-time init did not complete.
 * \param cert Output pointer to DER certificate.
 * \param cert_len Output certificate length.
 * \return `true` on success, otherwise `false`.
 */
bool u2f_get_attestation_cert(const uint8_t **cert, uint16_t *cert_len) {
    if (!cert || !cert_len) {
        return false;
    }
    if (!u2f_init_attestation()) {
        return false;
    }
    *cert = g_attest_cert;
    *cert_len = g_attest_cert_len;
    return true;
}

bool u2f_get_attestation_pubkey(uint8_t out[65]) {
    if (!out) return false;
    if (!u2f_init_attestation()) return false;
    memcpy(out, g_attest_pubkey, 65);
    return true;
}

bool u2f_import_attestation_cert(const uint8_t *der, size_t len) {
    if (!der || len == 0 || len > U2F_MAX_ATT_CERT_SIZE) return false;
    // Ensure g_attest_pubkey reflects the live slot-0 key before validating.
    if (!u2f_init_attestation()) return false;
    if (!cert_matches_attest_key(der, len)) {
        LOG_W(TAG, "Rejecting attestation cert: public key mismatch");
        return false;
    }

    nvs_handle_t nvs;
    if (nvs_open(ATTEST_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(nvs, ATTEST_NVS_CERT, der, len);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return false;

    // Reload so subsequent makeCredential responses use the imported cert.
    g_attest_initialized = false;
    return u2f_init_attestation();
}

bool u2f_clear_attestation_cert(void) {
    nvs_handle_t nvs;
    if (nvs_open(ATTEST_NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, ATTEST_NVS_CERT);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    g_attest_initialized = false;
    return u2f_init_attestation();
}

/**
 * \brief Signs payload using the attestation key, initializing attestation
 *        on demand if the boot-time init did not complete.
 * \param data Data to sign.
 * \param data_len Length of `data`.
 * \param signature Destination signature buffer.
 * \param sig_len Output signature length.
 * \return `true` on success, otherwise `false`.
 */
bool u2f_attestation_sign(const uint8_t *data, size_t data_len,
                          uint8_t *signature, uint8_t *sig_len) {
    if (!u2f_init_attestation()) {
        return false;
    }
    return u2f_attest_sign(data, data_len, signature, sig_len);
}

/**
 * \brief Writes a U2F status word to response buffer.
 * \param response Destination response buffer.
 * \param sw Status word.
 * \return Number of response bytes written.
 */
static uint16_t u2f_response_sw(uint8_t *response, uint16_t sw) {
    response[0] = (sw >> 8) & 0xFF;
    response[1] = sw & 0xFF;
    return 2;
}

/**
 * \brief Writes a U2F error status word to response buffer.
 * \param response Destination response buffer.
 * \param sw Status word.
 * \return Number of response bytes written.
 */
static uint16_t u2f_response_error(uint8_t *response, uint16_t sw) {
    return u2f_response_sw(response, sw);
}

/**
 * \brief Handles U2F `VERSION` instruction (`INS=0x03`).
 * \param response Destination response buffer.
 * \param response_max Capacity of `response`.
 * \return Number of response bytes written.
 */
static uint16_t u2f_version(uint8_t *response, uint16_t response_max) {
    const char *version = "U2F_V2";
    size_t len = strlen(version);

    if (response_max < len + 2) {
        return u2f_response_error(response, U2F_SW_WRONG_LENGTH);
    }

    memcpy(response, version, len);
    response[len] = 0x90;
    response[len + 1] = 0x00;

    LOG_I(TAG, "Version request: U2F_V2");
    return len + 2;
}

/** \brief U2F register/authenticate command helpers. */
/**
 * \brief Detects Chrome-style dummy application hashes used for blink/device selection.
 * \param application Pointer to the 32-byte U2F application hash.
 * \return `true` if the hash matches the repetitive dummy pattern, otherwise `false`.
 */
static bool is_dummy_application(const uint8_t *application) {
    uint8_t first = application[0];
    // Check if all 32 bytes are the same (dummy pattern)
    for (int i = 1; i < 32; i++) {
        if (application[i] != first) {
            return false;
        }
    }
    LOG_I(TAG, "Detected dummy/blink request (app=0x%02x...)", first);
    return true;
}

/**
 * \brief Handles U2F `REGISTER` instruction (`INS=0x01`).
 * \param challenge 32-byte challenge parameter.
 * \param application 32-byte application hash parameter.
 * \param response Destination response buffer.
 * \param response_max Capacity of `response`.
 * \return Number of response bytes written.
 */
static uint16_t u2f_register(const uint8_t *challenge, const uint8_t *application,
                              uint8_t *response, uint16_t response_max) {
    LOG_I(TAG, "Register request");

    bool is_dummy = is_dummy_application(application);

    // For dummy/blink requests (app hash = 0x41414141... or similar),
    // Chrome is probing for device presence before real registration.
    // We need to wait for actual user touch, then return a valid-looking response.
    // Chrome will discard the result but recognize the touch happened.
    if (is_dummy) {
        // Show identifier based on the dummy byte pattern
        char dummy_id[16];
        snprintf(dummy_id, sizeof(dummy_id), "U2F:%02x%02x%02x%02x",
                 application[0], application[1], application[2], application[3]);

        // Request user presence - SELECT action for device selection
        fido2_user_presence_result_t up_result = fido2_request_user_presence(
            dummy_id, FIDO2_ACTION_SELECT, NULL);

        if (up_result != FIDO2_UP_APPROVED) {
            LOG_D(TAG, "Dummy: no user presence yet");
            return u2f_response_error(response, U2F_SW_CONDITIONS_NOT_SATISFIED);
        }

        // User touched - generate a dummy response (random data, not stored)
        LOG_I(TAG, "Dummy: user touched - generating response");
        uint8_t dummy_cred[U2F_KEY_HANDLE_SIZE];
        uint8_t dummy_pubkey[64];
        auto* se = cdc::hal::getSecureElementInstance();
        if (!se || !se->getRandom(dummy_cred, U2F_KEY_HANDLE_SIZE) ||
            !se->getRandom(dummy_pubkey, 64)) {
            LOG_E(TAG, "Failed to get random for dummy response");
            return U2F_SW_WTF;
        }

        // Build minimal response: 0x05 || pubkey || kh_len || kh || cert || sig
        uint16_t offset = 0;
        response[offset++] = U2F_REGISTER_ID;
        response[offset++] = EC_POINT_UNCOMPRESSED;
        memcpy(response + offset, dummy_pubkey, 64);
        offset += 64;
        response[offset++] = U2F_KEY_HANDLE_SIZE;
        memcpy(response + offset, dummy_cred, U2F_KEY_HANDLE_SIZE);
        offset += U2F_KEY_HANDLE_SIZE;

        // Add attestation cert
        if (offset + g_attest_cert_len + U2F_MAX_EC_SIG_SIZE + 2 > response_max) {
            return u2f_response_error(response, U2F_SW_WRONG_LENGTH);
        }
        memcpy(response + offset, g_attest_cert, g_attest_cert_len);
        offset += g_attest_cert_len;

        // Sign with attestation key
        uint8_t to_sign[1 + 32 + 32 + U2F_KEY_HANDLE_SIZE + 65];
        size_t to_sign_len = 0;
        to_sign[to_sign_len++] = 0x00;
        memcpy(to_sign + to_sign_len, application, 32);
        to_sign_len += 32;
        memcpy(to_sign + to_sign_len, challenge, 32);
        to_sign_len += 32;
        memcpy(to_sign + to_sign_len, dummy_cred, U2F_KEY_HANDLE_SIZE);
        to_sign_len += U2F_KEY_HANDLE_SIZE;
        to_sign[to_sign_len++] = EC_POINT_UNCOMPRESSED;
        memcpy(to_sign + to_sign_len, dummy_pubkey, 64);
        to_sign_len += 64;

        uint8_t signature[U2F_MAX_EC_SIG_SIZE];
        uint8_t sig_len = 0;
        if (!u2f_attest_sign(to_sign, to_sign_len, signature, &sig_len)) {
            return u2f_response_error(response, U2F_SW_WRONG_DATA);
        }
        memcpy(response + offset, signature, sig_len);
        offset += sig_len;

        response[offset++] = 0x90;
        response[offset++] = 0x00;

        LOG_I(TAG, "Dummy response complete, len=%u", offset);
        return offset;
    }

    // Create unique identifier from application hash (first 4 bytes as hex)
    char rp_id[16];
    snprintf(rp_id, sizeof(rp_id), "U2F:%02x%02x%02x%02x",
             application[0], application[1], application[2], application[3]);

    // Request user presence for real registration
    fido2_user_presence_result_t up_result = fido2_request_user_presence(
        rp_id, FIDO2_ACTION_REGISTER, NULL);

    if (up_result != FIDO2_UP_APPROVED) {
        LOG_I(TAG, "User presence denied");
        return u2f_response_error(response, U2F_SW_CONDITIONS_NOT_SATISFIED);
    }

    uint8_t cred_id[U2F_KEY_HANDLE_SIZE];
    uint8_t pubkey[64];  // X || Y (no uncompressed prefix)
    uint8_t slot = 0;

    {
        // Real registration - create and store credential
        uint8_t user_id[1] = {0};

        if (!fido2_storage_create_credential(
                rp_id, application, user_id, 1, "U2F",
                false, 0, CDC_CURVE_P256, &slot, cred_id, pubkey)) {
            LOG_E(TAG, "Failed to create credential");
            return u2f_response_error(response, U2F_SW_WRONG_DATA);
        }
        LOG_I(TAG, "Created credential in slot %d", slot);
    }

    // Build registration response:
    // 0x05 || pubkey (65) || keyHandleLen (1) || keyHandle || attestation cert || signature
    uint16_t offset = 0;

    // Reserved byte
    response[offset++] = U2F_REGISTER_ID;

    // Public key (uncompressed: 0x04 || X || Y)
    response[offset++] = EC_POINT_UNCOMPRESSED;
    memcpy(response + offset, pubkey, 64);
    offset += 64;

    // Key handle length
    response[offset++] = U2F_KEY_HANDLE_SIZE;

    // Key handle (credential ID)
    memcpy(response + offset, cred_id, U2F_KEY_HANDLE_SIZE);
    offset += U2F_KEY_HANDLE_SIZE;

    // Attestation certificate
    if (!u2f_init_attestation()) {
        LOG_E(TAG, "Attestation not initialized");
        fido2_storage_delete_credential(slot);
        return u2f_response_error(response, U2F_SW_WRONG_DATA);
    }

    if (offset + g_attest_cert_len + U2F_MAX_EC_SIG_SIZE + 2 > response_max) {
        LOG_E(TAG, "Response buffer too small");
        fido2_storage_delete_credential(slot);
        return u2f_response_error(response, U2F_SW_WRONG_LENGTH);
    }

    memcpy(response + offset, g_attest_cert, g_attest_cert_len);
    offset += g_attest_cert_len;

    // Build data to sign: 0x00 || appParam || challenge || keyHandle || pubkey
    // This is signed with the ATTESTATION key, not the credential key
    uint8_t to_sign[1 + 32 + 32 + U2F_KEY_HANDLE_SIZE + 65];
    size_t to_sign_len = 0;

    to_sign[to_sign_len++] = 0x00;  // Reserved
    memcpy(to_sign + to_sign_len, application, 32);
    to_sign_len += 32;
    memcpy(to_sign + to_sign_len, challenge, 32);
    to_sign_len += 32;
    memcpy(to_sign + to_sign_len, cred_id, U2F_KEY_HANDLE_SIZE);
    to_sign_len += U2F_KEY_HANDLE_SIZE;
    to_sign[to_sign_len++] = EC_POINT_UNCOMPRESSED;
    memcpy(to_sign + to_sign_len, pubkey, 64);
    to_sign_len += 64;

    // Sign with attestation key (slot 0)
    uint8_t signature[U2F_MAX_EC_SIG_SIZE];
    uint8_t sig_len = 0;

    if (!u2f_attest_sign(to_sign, to_sign_len, signature, &sig_len)) {
        LOG_E(TAG, "Attestation signing failed");
        fido2_storage_delete_credential(slot);
        return u2f_response_error(response, U2F_SW_WRONG_DATA);
    }

    memcpy(response + offset, signature, sig_len);
    offset += sig_len;

    // Status word
    response[offset++] = 0x90;
    response[offset++] = 0x00;

    LOG_I(TAG, "Register complete, response len=%u", offset);
    return offset;
}

/**
 * \brief Handles U2F `AUTHENTICATE` instruction (`INS=0x02`).
 * \param p1 U2F authenticate control byte.
 * \param challenge 32-byte challenge parameter.
 * \param application 32-byte application hash parameter.
 * \param key_handle Key-handle bytes.
 * \param key_handle_len Length of `key_handle`.
 * \param response Destination response buffer.
 * \param response_max Capacity of `response`.
 * \return Number of response bytes written.
 */
static uint16_t u2f_authenticate(uint8_t p1, const uint8_t *challenge,
                                  const uint8_t *application,
                                  const uint8_t *key_handle, uint8_t key_handle_len,
                                  uint8_t *response, uint16_t response_max) {
    LOG_I(TAG, "Authenticate request, p1=0x%02X, kh_len=%d", p1, key_handle_len);

    if (key_handle_len != U2F_KEY_HANDLE_SIZE) {
        LOG_W(TAG, "Invalid key handle length: %d", key_handle_len);
        return u2f_response_error(response, U2F_SW_WRONG_DATA);
    }

    // Find credential by key handle
    int8_t slot = fido2_storage_find_slot_by_cred_id(key_handle, key_handle_len);
    if (slot < 0) {
        LOG_W(TAG, "Key handle not found");
        return u2f_response_error(response, U2F_SW_WRONG_DATA);
    }

    // Verify RP ID hash matches
    fido2_credential_info_t cred;
    if (!fido2_storage_get_credential(slot, &cred)) {
        LOG_E(TAG, "Failed to get credential info");
        return u2f_response_error(response, U2F_SW_WRONG_DATA);
    }

    if (memcmp(cred.rp_id_hash, application, 32) != 0) {
        LOG_W(TAG, "Application hash mismatch");
        return u2f_response_error(response, U2F_SW_WRONG_DATA);
    }

    // Check-only mode - just verify key handle is valid
    if (p1 == U2F_AUTH_CHECK_ONLY) {
        LOG_I(TAG, "Check-only: key handle valid");
        return u2f_response_error(response, U2F_SW_CONDITIONS_NOT_SATISFIED);
    }

    // Request user presence (unless dont-enforce)
    if (p1 == U2F_AUTH_ENFORCE) {
        fido2_user_presence_result_t up_result = fido2_request_user_presence(
            cred.rp_id, FIDO2_ACTION_AUTHENTICATE, cred.user_name);

        if (up_result != FIDO2_UP_APPROVED) {
            LOG_I(TAG, "User presence denied");
            return u2f_response_error(response, U2F_SW_CONDITIONS_NOT_SATISFIED);
        }
    }

    // Increment counter
    uint32_t counter = fido2_storage_increment_sign_count(slot);
    if (counter == 0) {
        return u2f_response_error(response, U2F_SW_CONDITIONS_NOT_SATISFIED);
    }
    fido2_increment_auth_counter();

    // Build authentication response:
    // userPresence (1) || counter (4) || signature
    uint16_t offset = 0;

    // User presence flag
    response[offset++] = 0x01;  // UP=1

    // Counter (big-endian)
    cdc::core::writeBe32(&response[offset], counter);
    offset += 4;

    // Build data to sign: appParam || userPresence || counter || challenge
    uint8_t to_sign[32 + 1 + 4 + 32];
    size_t to_sign_len = 0;

    memcpy(to_sign + to_sign_len, application, 32);
    to_sign_len += 32;
    to_sign[to_sign_len++] = 0x01;  // User presence
    cdc::core::writeBe32(&to_sign[to_sign_len], counter);
    to_sign_len += 4;
    memcpy(to_sign + to_sign_len, challenge, 32);
    to_sign_len += 32;

    // Sign with TROPIC01
    uint8_t signature[U2F_MAX_EC_SIG_SIZE];
    uint8_t sig_len = 0;

    if (!fido2_storage_sign_raw(slot, to_sign, to_sign_len, signature, &sig_len)) {
        LOG_E(TAG, "Signing failed");
        return u2f_response_error(response, U2F_SW_WRONG_DATA);
    }

    memcpy(response + offset, signature, sig_len);
    offset += sig_len;

    // Status word
    response[offset++] = 0x90;
    response[offset++] = 0x00;

    LOG_I(TAG, "Authenticate complete, counter=%u, response len=%u", counter, offset);
    return offset;
}

/**
 * \brief Parses U2F APDU and dispatches to instruction handlers.
 * \param apdu Input APDU bytes.
 * \param apdu_len Length of `apdu`.
 * \param response Destination response buffer.
 * \param response_max Capacity of `response`.
 * \return Number of response bytes written.
 */
uint16_t u2f_process_apdu(const uint8_t *apdu, uint16_t apdu_len,
                          uint8_t *response, uint16_t response_max) {
    if (apdu_len < 4) {
        LOG_W(TAG, "APDU too short: %d", apdu_len);
        return u2f_response_error(response, U2F_SW_WRONG_LENGTH);
    }

    uint8_t cla = apdu[0];
    uint8_t ins = apdu[1];
    uint8_t p1 = apdu[2];
    uint8_t p2 = apdu[3];

    // Only support CLA=0x00
    if (cla != 0x00) {
        LOG_W(TAG, "Unsupported CLA: 0x%02X", cla);
        return u2f_response_error(response, U2F_SW_CLA_NOT_SUPPORTED);
    }

    LOG_I(TAG, "APDU: CLA=0x%02X INS=0x%02X P1=0x%02X P2=0x%02X len=%d",
          cla, ins, p1, p2, apdu_len);

    // Parse extended length APDU
    // Format: CLA INS P1 P2 [Lc(3)] [DATA] [Le(2)]
    uint32_t data_len = 0;
    const uint8_t *data = NULL;

    if (apdu_len > 4) {
        if (apdu[4] == 0x00 && apdu_len > 6) {
            // Extended length: 00 Lc1 Lc2
            data_len = (apdu[5] << 8) | apdu[6];
            data = apdu + 7;
            if (data_len + 7 > apdu_len) {
                data_len = apdu_len - 7;
            }
        } else {
            // Short length: Lc
            data_len = apdu[4];
            data = apdu + 5;
            if (data_len + 5 > apdu_len) {
                data_len = apdu_len - 5;
            }
        }
    }

    switch (ins) {
        case U2F_INS_VERSION:
            return u2f_version(response, response_max);

        case U2F_INS_REGISTER:
            if (data_len < U2F_CHALLENGE_SIZE + U2F_APPLICATION_SIZE) {
                LOG_W(TAG, "Register: insufficient data: %u", data_len);
                return u2f_response_error(response, U2F_SW_WRONG_LENGTH);
            }
            return u2f_register(data, data + U2F_CHALLENGE_SIZE,
                               response, response_max);

        case U2F_INS_AUTHENTICATE:
            if (data_len < U2F_CHALLENGE_SIZE + U2F_APPLICATION_SIZE + 1) {
                LOG_W(TAG, "Authenticate: insufficient data: %u", data_len);
                return u2f_response_error(response, U2F_SW_WRONG_LENGTH);
            }
            {
                uint8_t kh_len = data[U2F_CHALLENGE_SIZE + U2F_APPLICATION_SIZE];
                const uint8_t *kh = data + U2F_CHALLENGE_SIZE + U2F_APPLICATION_SIZE + 1;

                if (data_len < U2F_CHALLENGE_SIZE + U2F_APPLICATION_SIZE + 1 + kh_len) {
                    LOG_W(TAG, "Authenticate: key handle truncated");
                    return u2f_response_error(response, U2F_SW_WRONG_LENGTH);
                }

                return u2f_authenticate(p1, data, data + U2F_CHALLENGE_SIZE,
                                        kh, kh_len, response, response_max);
            }

        default:
            LOG_W(TAG, "Unsupported INS: 0x%02X", ins);
            return u2f_response_error(response, U2F_SW_INS_NOT_SUPPORTED);
    }
}
