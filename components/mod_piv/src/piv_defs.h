/*
 * PIV applet shared definitions: AIDs, instruction/tag constants, status
 * words, on-card state, and the hardware backend seam.
 *
 * The APDU state machine (piv_applet.cpp) is deliberately free of any
 * secure-element, mbedtls, NVS or PinManager dependency. Every hardware or
 * crypto side effect goes through piv_backend_t, so the state machine can be
 * unit-tested on the host with injected fakes.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "cdc_scard/applet.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- PIV application identifier (NIST SP 800-73) ---------------------------
// 9-byte prefix (RID + application portion, without the 2-byte version). The
// dispatcher's longest-prefix match also accepts the full 11-byte AID that
// hosts sometimes send.
#define PIV_AID_PREFIX_LEN 9

// --- Instructions ----------------------------------------------------------
#define PIV_INS_SELECT              0xA4
#define PIV_INS_GET_DATA            0xCB
#define PIV_INS_VERIFY              0x20
#define PIV_INS_CHANGE_REFERENCE    0x24
#define PIV_INS_GENERAL_AUTH        0x87
#define PIV_INS_GENERATE_KEYPAIR    0x47
#define PIV_INS_PUT_DATA            0xDB
#define PIV_INS_GET_RESPONSE        0xC0
// Yubico PIV extensions used by ykman for management-key metadata.
#define PIV_INS_GET_METADATA        0xF7
#define PIV_INS_SET_MGMT_KEY        0xFF

// --- Key references ---------------------------------------------------------
#define PIV_KEY_9A 0x9A  // PIV Authentication
#define PIV_KEY_9C 0x9C  // Digital Signature
#define PIV_KEY_9D 0x9D  // Key Management (software key, ECDH)
#define PIV_KEY_9E 0x9E  // Card Authentication
#define PIV_KEY_9B 0x9B  // Management key
#define PIV_KEY_PIN 0x80 // Application PIN reference (VERIFY P2)

// --- Crypto algorithm identifiers ------------------------------------------
#define PIV_ALG_ECC_P256 0x11
#define PIV_ALG_AES128   0x08
#define PIV_ALG_AES192   0x0A
#define PIV_ALG_AES256   0x0C

// Management key: AES-256 (32 bytes). The ESP32-S3 AES accelerator supports
// only 128- and 256-bit keys (no AES-192), so v1 ships AES-256.
#define PIV_MGMT_KEY_LEN 32
#define PIV_MGMT_BLOCK   16

// --- BER-TLV tags -----------------------------------------------------------
#define PIV_TAG_APT          0x61
#define PIV_TAG_AID          0x4F
#define PIV_TAG_ALLOC_AUTH   0x79
#define PIV_TAG_APP_LABEL    0x50
#define PIV_TAG_ALG_ID       0xAC
#define PIV_TAG_DATA_OBJECT  0x53
#define PIV_TAG_TAG_LIST     0x5C
#define PIV_TAG_DISCOVERY    0x7E
#define PIV_TAG_DYN_AUTH     0x7C
#define PIV_TAG_WITNESS      0x80
#define PIV_TAG_CHALLENGE    0x81
#define PIV_TAG_RESPONSE     0x82
#define PIV_TAG_EXP          0x85  // ECDH peer public point
#define PIV_TAG_PUBKEY       0x7F49
#define PIV_TAG_PUBKEY_POINT 0x86

// --- Status words -----------------------------------------------------------
#define PIV_SW_OK               0x9000
#define PIV_SW_MORE_DATA        0x6100  // low byte carries remaining length
#define PIV_SW_WRONG_LENGTH     0x6700
#define PIV_SW_SECURITY_STATUS  0x6982
#define PIV_SW_AUTH_BLOCKED     0x6983
#define PIV_SW_DATA_INVALID     0x6A80
#define PIV_SW_FILE_NOT_FOUND   0x6A82
#define PIV_SW_INCORRECT_P1P2   0x6A86
#define PIV_SW_REF_NOT_FOUND    0x6A88
#define PIV_SW_INS_NOT_SUPPORTED 0x6D00
#define PIV_SW_CLA_NOT_SUPPORTED 0x6E00
#define PIV_SW_PIN_RETRIES      0x63C0  // low nibble = remaining retries

// --- On-card PIV state (persisted by the backend) --------------------------
typedef struct {
    uint8_t version;                     // Layout version (1)
    uint8_t mgmt_alg;                    // PIV_ALG_AES* for key 9B
    uint8_t mgmt_is_default;             // 1 while the factory default key stands
    uint8_t mgmt_key[PIV_MGMT_KEY_LEN];  // 24-byte management key
    uint8_t guid[16];                    // CHUID GUID (random, non-zero)
    uint8_t ccc_id[14];                  // CCC card identifier (random)
    uint8_t key_flags[4];                // Per key 9A/9C/9D/9E: bit0 = generated
} piv_state_t;

// --- Hardware / crypto backend seam ----------------------------------------
// All pointers must be set before the applet is registered. On the target the
// real implementation lives in piv_keys.cpp; host tests inject fakes.
typedef struct {
    // ECC key operations. key_ref is one of PIV_KEY_9A/9C/9D/9E.
    bool (*key_present)(uint8_t key_ref);
    // Sign a 32-byte digest, output raw R||S (64 bytes).
    bool (*key_sign)(uint8_t key_ref, const uint8_t digest[32], uint8_t sig_rs[64]);
    // Generate a fresh P-256 key, output uncompressed public point (65 bytes).
    bool (*key_generate)(uint8_t key_ref, uint8_t pub_out[65]);
    // Read the uncompressed public point of an existing key (65 bytes).
    bool (*key_pubkey)(uint8_t key_ref, uint8_t pub_out[65]);
    // ECDH on key 9D: shared_x = X(d * peer_point). peer_point is 65 bytes.
    bool (*key_ecdh)(const uint8_t peer_point[65], uint8_t shared_x[32]);

    // Application PIN (mapped to the badge PIN on target).
    bool (*pin_verify)(const char* pin);
    uint8_t (*pin_retries)(void);
    bool (*pin_blocked)(void);

    // Management-key block cipher: out = AES-ENC(mgmt_key, in), block = 16.
    bool (*mgmt_encrypt)(const uint8_t in[PIV_MGMT_BLOCK], uint8_t out[PIV_MGMT_BLOCK]);
    // Replace the management key (INS FF). Clears the default flag.
    bool (*mgmt_set_key)(uint8_t alg, const uint8_t* key, uint8_t key_len);

    // Persisted state.
    bool (*state_load)(piv_state_t* out);

    // Data objects (certificates etc.) keyed by their 5FC1xx object tag.
    // Returns length written, or -1 when absent / on error.
    int  (*object_load)(uint32_t object_tag, uint8_t* out, size_t out_max);
    bool (*object_store)(uint32_t object_tag, const uint8_t* data, size_t len);

    // Random bytes (strict TRNG on target).
    bool (*rng)(uint8_t* buf, size_t len);
} piv_backend_t;

// Install the backend. Must be called before scard_register_applet.
void piv_set_backend(const piv_backend_t* backend);

// Applet descriptor for the dispatcher.
const scard_applet_t* piv_applet(void);

#ifdef __cplusplus
}
#endif
