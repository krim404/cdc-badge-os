// U2F/CTAP1 Protocol Implementation
// Legacy U2F support for Chrome compatibility

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// U2F Constants
// ============================================================================

// U2F Command bytes (INS)
#define U2F_INS_REGISTER        0x01
#define U2F_INS_AUTHENTICATE    0x02
#define U2F_INS_VERSION         0x03

// U2F Authentication control bytes (P1)
#define U2F_AUTH_CHECK_ONLY     0x07    // Check if key handle valid
#define U2F_AUTH_ENFORCE        0x03    // Sign with user presence
#define U2F_AUTH_DONT_ENFORCE   0x08    // Sign without user presence (not recommended)

// U2F Status Words (SW1 || SW2)
#define U2F_SW_NO_ERROR                 0x9000
#define U2F_SW_CONDITIONS_NOT_SATISFIED 0x6985  // User presence required
#define U2F_SW_WRONG_DATA               0x6A80  // Invalid key handle
#define U2F_SW_WRONG_LENGTH             0x6700
#define U2F_SW_CLA_NOT_SUPPORTED        0x6E00
#define U2F_SW_INS_NOT_SUPPORTED        0x6D00
#define U2F_SW_WRONG_P1P2              0x6B00
#define U2F_SW_WTF                     0x6F00  // Internal error

// U2F Sizes
#define U2F_CHALLENGE_SIZE      32
#define U2F_APPLICATION_SIZE    32
#define U2F_KEY_HANDLE_SIZE     64      // Match FIDO2 credential ID
#define U2F_REGISTER_ID         0x05    // Registration reserved byte
#define U2F_EC_POINT_SIZE       65      // 0x04 || X || Y (uncompressed)
#define U2F_EC_KEY_SIZE         32      // P-256 key component
#define U2F_MAX_ATT_CERT_SIZE   1024
#define U2F_MAX_EC_SIG_SIZE     72      // DER encoded ECDSA signature
#define U2F_CTR_SIZE            4       // Counter size

// ============================================================================
// Functions
// ============================================================================

/**
 * Initialize U2F attestation.
 * Generates attestation key in slot 30 if not present.
 * Must be called before using U2F.
 *
 * @return true on success
 */
bool u2f_init_attestation(void);

/**
 * Get attestation certificate (DER encoded).
 * Must call u2f_init_attestation() first.
 *
 * @param cert Output buffer for certificate
 * @param cert_len Output certificate length
 * @return true if attestation is initialized
 */
bool u2f_get_attestation_cert(const uint8_t **cert, uint16_t *cert_len);

/**
 * Get the attestation public key (slot 0) as an uncompressed EC point.
 *
 * @param out 65-byte output buffer receiving 0x04 || X || Y.
 * @return true on success.
 */
bool u2f_get_attestation_pubkey(uint8_t out[65]);

/**
 * Import a CA-signed attestation certificate (DER).
 *
 * The certificate's public key must match the attestation key in slot 0.
 * On success the DER is persisted and used in place of the self-signed
 * certificate for future makeCredential responses.
 *
 * @param der DER-encoded X.509 certificate.
 * @param len Certificate length.
 * @return true if the certificate was validated and stored.
 */
bool u2f_import_attestation_cert(const uint8_t *der, size_t len);

/**
 * Remove an imported attestation certificate, reverting to the self-signed
 * certificate generated on-device.
 *
 * @return true on success.
 */
bool u2f_clear_attestation_cert(void);

/**
 * Sign data with attestation key (slot 30).
 * Must call u2f_init_attestation() first.
 *
 * @param data Data to sign
 * @param data_len Data length
 * @param signature Output DER-encoded signature
 * @param sig_len Output signature length
 * @return true on success
 */
bool u2f_attestation_sign(const uint8_t *data, size_t data_len,
                          uint8_t *signature, uint8_t *sig_len);

/**
 * Process a U2F APDU message.
 *
 * @param apdu Input APDU (CLA INS P1 P2 [Lc DATA Le])
 * @param apdu_len Length of input APDU
 * @param response Output buffer for response
 * @param response_max Maximum response size
 * @return Actual response length (including SW1 SW2)
 */
uint16_t u2f_process_apdu(const uint8_t *apdu, uint16_t apdu_len,
                          uint8_t *response, uint16_t response_max);

#ifdef __cplusplus
}
#endif

