// CTAP2 Protocol Implementation (FIDO2)
// Handles CBOR-encoded CTAP2 commands

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fido2.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CTAP2 Commands
// ============================================================================

#define CTAP2_CMD_MAKE_CREDENTIAL       0x01
#define CTAP2_CMD_GET_ASSERTION         0x02
#define CTAP2_CMD_GET_INFO              0x04
#define CTAP2_CMD_CLIENT_PIN            0x06
#define CTAP2_CMD_RESET                 0x07
#define CTAP2_CMD_GET_NEXT_ASSERTION    0x08
#define CTAP2_CMD_CRED_MANAGEMENT       0x0A
#define CTAP2_CMD_SELECTION             0x0B
#define CTAP2_CMD_LARGE_BLOBS           0x0C
#define CTAP2_CMD_CONFIG                0x0D

// Vendor commands
#define CTAP2_CMD_VENDOR_FIRST          0x40
#define CTAP2_CMD_VENDOR_LAST           0xBF

// ============================================================================
// CTAP2 Status Codes
// ============================================================================

#define CTAP2_OK                        0x00
#define CTAP1_ERR_INVALID_COMMAND       0x01
#define CTAP1_ERR_INVALID_PARAMETER     0x02
#define CTAP1_ERR_INVALID_LENGTH        0x03
#define CTAP1_ERR_INVALID_SEQ           0x04
#define CTAP1_ERR_TIMEOUT               0x05
#define CTAP1_ERR_CHANNEL_BUSY          0x06
#define CTAP1_ERR_LOCK_REQUIRED         0x0A
#define CTAP1_ERR_INVALID_CHANNEL       0x0B
#define CTAP2_ERR_CBOR_UNEXPECTED_TYPE  0x11
#define CTAP2_ERR_INVALID_CBOR          0x12
#define CTAP2_ERR_MISSING_PARAMETER     0x14
#define CTAP2_ERR_LIMIT_EXCEEDED        0x15
#define CTAP2_ERR_UNSUPPORTED_EXT       0x16
#define CTAP2_ERR_LARGE_BLOB_STORAGE_FULL 0x18
#define CTAP2_ERR_CREDENTIAL_EXCLUDED   0x19
#define CTAP2_ERR_PROCESSING            0x21
#define CTAP2_ERR_INVALID_CREDENTIAL    0x22
#define CTAP2_ERR_USER_ACTION_PENDING   0x23
#define CTAP2_ERR_OPERATION_PENDING     0x24
#define CTAP2_ERR_NO_OPERATIONS         0x25
#define CTAP2_ERR_UNSUPPORTED_ALGORITHM 0x26
#define CTAP2_ERR_OPERATION_DENIED      0x27
#define CTAP2_ERR_KEY_STORE_FULL        0x28
#define CTAP2_ERR_NO_OPERATION_PENDING  0x2A
#define CTAP2_ERR_UNSUPPORTED_OPTION    0x2B
#define CTAP2_ERR_INVALID_OPTION        0x2C
#define CTAP2_ERR_KEEPALIVE_CANCEL      0x2D
#define CTAP2_ERR_NO_CREDENTIALS        0x2E
#define CTAP2_ERR_USER_ACTION_TIMEOUT   0x2F
#define CTAP2_ERR_NOT_ALLOWED           0x30
#define CTAP2_ERR_PIN_INVALID           0x31
#define CTAP2_ERR_PIN_BLOCKED           0x32
#define CTAP2_ERR_PIN_AUTH_INVALID      0x33
#define CTAP2_ERR_PIN_AUTH_BLOCKED      0x34
#define CTAP2_ERR_PIN_NOT_SET           0x35
#define CTAP2_ERR_PIN_REQUIRED          0x36
#define CTAP2_ERR_PIN_POLICY_VIOLATION  0x37
#define CTAP2_ERR_PIN_TOKEN_EXPIRED     0x38
#define CTAP2_ERR_REQUEST_TOO_LARGE     0x39
#define CTAP2_ERR_ACTION_TIMEOUT        0x3A
#define CTAP2_ERR_UP_REQUIRED           0x3B
#define CTAP2_ERR_UV_BLOCKED            0x3C
#define CTAP2_ERR_INTEGRITY_FAILURE     0x3D
#define CTAP2_ERR_OTHER                 0x7F

// ============================================================================
// CTAP2 Algorithms
// ============================================================================

#define COSE_ALG_ES256              -7     // ECDSA with SHA-256 (P-256)
#define COSE_ALG_EDDSA              -8     // EdDSA (Ed25519)
#define COSE_ALG_RS256              -257   // RSASSA-PKCS1-v1_5 with SHA-256
#define COSE_ALG_ECDH_ES_HKDF_256   -25    // ECDH-ES + HKDF-256 (RFC 8152)

// ============================================================================
// COSE Key Constants (RFC 8152 / RFC 8037)
// See: https://datatracker.ietf.org/doc/html/rfc8152
//      https://www.iana.org/assignments/cose/cose.xhtml
// ============================================================================

// COSE Key Common Parameter labels (negative values are curve-specific labels)
#define COSE_KEY_LABEL_KTY          1      // Key type
#define COSE_KEY_LABEL_KID          2      // Key identifier
#define COSE_KEY_LABEL_ALG          3      // Algorithm
#define COSE_KEY_LABEL_OPS          4      // Key operations
#define COSE_KEY_LABEL_BASE_IV      5      // Base IV
#define COSE_KEY_LABEL_CRV          -1     // Curve (EC2/OKP)
#define COSE_KEY_LABEL_X            -2     // X coordinate (EC2) / public key (OKP)
#define COSE_KEY_LABEL_Y            -3     // Y coordinate (EC2)
#define COSE_KEY_LABEL_D            -4     // Private key

// COSE Key Type (kty) values
#define COSE_KEY_TYPE_OKP           1      // Octet Key Pair (Ed25519, X25519)
#define COSE_KEY_TYPE_EC2           2      // Elliptic Curve with x/y coordinates
#define COSE_KEY_TYPE_SYMMETRIC     4      // Symmetric key

// COSE Elliptic Curves (crv) values
#define COSE_CRV_P256               1      // NIST P-256 (secp256r1)
#define COSE_CRV_P384               2      // NIST P-384
#define COSE_CRV_P521               3      // NIST P-521
#define COSE_CRV_X25519             4      // X25519 ECDH
#define COSE_CRV_X448               5      // X448 ECDH
#define COSE_CRV_ED25519            6      // Ed25519 EdDSA
#define COSE_CRV_ED448              7      // Ed448 EdDSA

// ============================================================================
// CTAP2 CBOR Map Keys (CTAP 2.1 Specification)
// See: https://fidoalliance.org/specs/fido-v2.1-rd-20191217/fido-client-to-authenticator-protocol-v2.1-rd-20191217.html
// ============================================================================

// authenticatorGetInfo response keys (Section 6.4)
#define CTAP2_INFO_VERSIONS                 0x01
#define CTAP2_INFO_EXTENSIONS               0x02
#define CTAP2_INFO_AAGUID                   0x03
#define CTAP2_INFO_OPTIONS                  0x04
#define CTAP2_INFO_MAX_MSG_SIZE             0x05
#define CTAP2_INFO_PIN_UV_AUTH_PROTOCOLS    0x06
#define CTAP2_INFO_MAX_CRED_COUNT_IN_LIST   0x07
#define CTAP2_INFO_MAX_CRED_ID_LENGTH       0x08
#define CTAP2_INFO_TRANSPORTS               0x09
#define CTAP2_INFO_ALGORITHMS               0x0A
#define CTAP2_INFO_MAX_SERIALIZED_LARGE_BLOB_ARRAY  0x0B
#define CTAP2_INFO_MIN_PIN_LENGTH                   0x0D

// authenticatorMakeCredential parameter keys (Section 6.1)
#define CTAP2_MC_CLIENT_DATA_HASH           0x01
#define CTAP2_MC_RP                         0x02
#define CTAP2_MC_USER                       0x03
#define CTAP2_MC_PUB_KEY_CRED_PARAMS        0x04
#define CTAP2_MC_EXCLUDE_LIST               0x05
#define CTAP2_MC_EXTENSIONS                 0x06
#define CTAP2_MC_OPTIONS                    0x07
#define CTAP2_MC_PIN_UV_AUTH_PARAM          0x08
#define CTAP2_MC_PIN_UV_AUTH_PROTOCOL       0x09

// authenticatorMakeCredential response keys (Section 6.1)
#define CTAP2_MC_RESP_FMT                   0x01
#define CTAP2_MC_RESP_AUTH_DATA             0x02
#define CTAP2_MC_RESP_ATT_STMT              0x03

// authenticatorGetAssertion parameter keys (Section 6.2)
#define CTAP2_GA_RP_ID                      0x01
#define CTAP2_GA_CLIENT_DATA_HASH           0x02
#define CTAP2_GA_ALLOW_LIST                 0x03
#define CTAP2_GA_EXTENSIONS                 0x04
#define CTAP2_GA_OPTIONS                    0x05
#define CTAP2_GA_PIN_UV_AUTH_PARAM          0x06
#define CTAP2_GA_PIN_UV_AUTH_PROTOCOL       0x07

// authenticatorGetAssertion response keys (Section 6.2)
#define CTAP2_GA_RESP_CREDENTIAL            0x01
#define CTAP2_GA_RESP_AUTH_DATA             0x02
#define CTAP2_GA_RESP_SIGNATURE             0x03
#define CTAP2_GA_RESP_USER                  0x04
#define CTAP2_GA_RESP_NUMBER_OF_CREDS       0x05

// authenticatorClientPIN parameter keys (Section 6.5)
#define CTAP2_PIN_PROTOCOL                  0x01
#define CTAP2_PIN_SUBCOMMAND                0x02
#define CTAP2_PIN_KEY_AGREEMENT             0x03
#define CTAP2_PIN_AUTH                      0x04
#define CTAP2_PIN_NEW_PIN_ENC               0x05
#define CTAP2_PIN_HASH_ENC                  0x06
#define CTAP2_PIN_PERMISSIONS               0x09
#define CTAP2_PIN_PERMISSIONS_RPID          0x0A

// authenticatorClientPIN response keys (Section 6.5)
#define CTAP2_PIN_RESP_KEY_AGREEMENT        0x01
#define CTAP2_PIN_RESP_PIN_TOKEN            0x02
#define CTAP2_PIN_RESP_PIN_RETRIES          0x03
#define CTAP2_PIN_RESP_POWER_CYCLE_STATE    0x04
#define CTAP2_PIN_RESP_UV_RETRIES           0x05

// authenticatorCredentialManagement parameter keys (Section 6.8)
#define CTAP2_CM_SUBCOMMAND                 0x01
#define CTAP2_CM_SUBCOMMAND_PARAMS          0x02
#define CTAP2_CM_PIN_UV_AUTH_PROTOCOL       0x03
#define CTAP2_CM_PIN_UV_AUTH_PARAM          0x04

// authenticatorCredentialManagement subcommand parameter keys
#define CTAP2_CM_SUB_RP_ID_HASH             0x01
#define CTAP2_CM_SUB_CREDENTIAL_ID          0x02

// authenticatorCredentialManagement response keys (Section 6.8)
#define CTAP2_CM_RESP_EXISTING_CRED_COUNT   0x01
#define CTAP2_CM_RESP_REMAINING_CRED_COUNT  0x02
#define CTAP2_CM_RESP_RP                    0x03
#define CTAP2_CM_RESP_RP_ID_HASH            0x04
#define CTAP2_CM_RESP_TOTAL_RPS             0x05
#define CTAP2_CM_RESP_USER                  0x06
#define CTAP2_CM_RESP_CREDENTIAL_ID         0x07
#define CTAP2_CM_RESP_PUBLIC_KEY            0x08
#define CTAP2_CM_RESP_TOTAL_CREDENTIALS     0x09
#define CTAP2_CM_RESP_CRED_PROTECT          0x0A

// authenticatorLargeBlobs parameter keys (Section 6.10)
#define CTAP2_LB_GET                        0x01
#define CTAP2_LB_SET                        0x02
#define CTAP2_LB_OFFSET                     0x03
#define CTAP2_LB_LENGTH                     0x04
#define CTAP2_LB_PIN_UV_AUTH_PARAM          0x05
#define CTAP2_LB_PIN_UV_AUTH_PROTOCOL       0x06
#define CTAP2_LB_RESP_CONFIG                0x01

// authenticatorConfig parameter keys (Section 6.11)
#define CTAP2_CONFIG_SUBCOMMAND             0x01
#define CTAP2_CONFIG_SUBCOMMAND_PARAMS      0x02
#define CTAP2_CONFIG_PIN_UV_AUTH_PROTOCOL   0x03
#define CTAP2_CONFIG_PIN_UV_AUTH_PARAM      0x04

// authenticatorConfig subcommands (Section 6.11)
#define CTAP2_CONFIG_SUB_ENABLE_EP          0x01
#define CTAP2_CONFIG_SUB_TOGGLE_ALWAYS_UV   0x02
#define CTAP2_CONFIG_SUB_SET_MIN_PIN_LENGTH 0x03
#define CTAP2_CONFIG_SUB_VENDOR_PROTOTYPE   0xFF

// setMinPINLength subCommandParams keys (Section 6.11)
#define CTAP2_CONFIG_PARAM_NEW_MIN_PIN_LEN  0x01
#define CTAP2_CONFIG_PARAM_MIN_PIN_RPIDS    0x02
#define CTAP2_CONFIG_PARAM_FORCE_CHANGE_PIN 0x03

// ============================================================================
// Processing Functions
// ============================================================================

/**
 * Initialize CTAP2 protocol handler.
 *
 * @return true on success
 */
bool ctap2_init(void);

/**
 * Process a CTAP2 command.
 *
 * @param cmd Command buffer (first byte is command code)
 * @param cmd_len Command length
 * @param response Output buffer for response
 * @param response_len Input: max size, Output: actual size
 * @return CTAP2 status code (first byte of response)
 */
uint8_t ctap2_process_command(const uint8_t *cmd, uint16_t cmd_len,
                               uint8_t *response, uint16_t *response_len);

/**
 * Send keepalive during long operations.
 * Called periodically to prevent timeout.
 */
void ctap2_send_keepalive(uint8_t status);

/**
 * Cancel any pending operation.
 */
void ctap2_cancel(void);

/**
 * \brief Clears the cancel flag. Called when a new CTAPHID channel is opened
 *        so a cancel from a previous channel doesn't poison responses on the
 *        new one (notably the INIT response itself).
 */
void ctap2_clear_cancel(void);

/**
 * \brief Returns true if the current CTAP2 operation has been cancelled.
 */
bool ctap2_is_cancelled(void);

// ============================================================================
// Individual Command Handlers
// ============================================================================

uint8_t ctap2_make_credential(const uint8_t *params, uint16_t params_len,
                               uint8_t *response, uint16_t *response_len);

uint8_t ctap2_get_assertion(const uint8_t *params, uint16_t params_len,
                             uint8_t *response, uint16_t *response_len);

uint8_t ctap2_get_info(uint8_t *response, uint16_t *response_len);

uint8_t ctap2_client_pin(const uint8_t *params, uint16_t params_len,
                          uint8_t *response, uint16_t *response_len);

uint8_t ctap2_reset(uint8_t *response, uint16_t *response_len);

uint8_t ctap2_get_next_assertion(uint8_t *response, uint16_t *response_len);

uint8_t ctap2_cred_management(const uint8_t *params, uint16_t params_len,
                               uint8_t *response, uint16_t *response_len);

uint8_t ctap2_selection(uint8_t *response, uint16_t *response_len);

uint8_t ctap2_large_blobs(const uint8_t *params, uint16_t params_len,
                          uint8_t *response, uint16_t *response_len);

uint8_t ctap2_config(const uint8_t *params, uint16_t params_len,
                     uint8_t *response, uint16_t *response_len);

#ifdef __cplusplus
}
#endif

