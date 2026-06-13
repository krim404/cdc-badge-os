// FIDO2/WebAuthn Module
// Main interface for credential management and user presence

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Constants
// ============================================================================

#define FIDO2_MAX_CREDENTIALS   32      // Max ECC slots (runtime range provided by module registry)
#define FIDO2_RP_ID_MAX_LEN     64
#define FIDO2_USER_ID_MAX_LEN   64
#define FIDO2_USER_NAME_MAX_LEN 32
#define FIDO2_CRED_ID_LEN       64      // Credential ID length

// Curve identifiers
#define CDC_CURVE_ED25519 0
#define CDC_CURVE_P256    1

// ============================================================================
// Types
// ============================================================================

typedef enum {
    FIDO2_UP_PENDING = 0,   // Waiting for user
    FIDO2_UP_APPROVED,      // User approved
    FIDO2_UP_DENIED,        // User denied
    FIDO2_UP_TIMEOUT        // Timeout waiting for user
} fido2_user_presence_result_t;

typedef enum {
    FIDO2_ACTION_REGISTER = 0,  // makeCredential, new credential
    FIDO2_ACTION_AUTHENTICATE,  // getAssertion
    FIDO2_ACTION_SELECT,        // Device selection (authenticatorSelection 0x0B / make.me.blink probe) - user presence only, no PIN
    FIDO2_ACTION_OVERWRITE      // makeCredential replacing an existing resident credential
} fido2_action_t;

#ifdef __DOXYGEN__
namespace cdc::mod_fido2 {
#endif

// Credential info (for listing - no private key data)
typedef struct {
    uint8_t slot;                           // Logical slot index (0..count-1)
    char rp_id[FIDO2_RP_ID_MAX_LEN];        // Relying Party ID (e.g., "github.com" or "ssh:server")
    uint8_t rp_id_hash[32];                 // SHA-256 of RP ID
    char user_name[FIDO2_USER_NAME_MAX_LEN]; // Display name
    uint8_t user_id[FIDO2_USER_ID_MAX_LEN]; // User handle
    uint8_t user_id_len;
    uint32_t sign_count;                    // Per-credential counter
    bool resident_key;                      // Is discoverable credential
    uint8_t cred_protect;                   // Credential protection level
    uint8_t curve;                          // CDC_CURVE_P256 or CDC_CURVE_ED25519
} fido2_credential_info_t;

#ifdef __DOXYGEN__
} // namespace cdc::mod_fido2
#endif

// User presence callback type
typedef fido2_user_presence_result_t (*fido2_user_presence_cb_t)(
    const char *rp_id,
    fido2_action_t action,
    const char *user_name
);

// ============================================================================
// Initialization
// ============================================================================

/**
 * Initialize FIDO2 module.
 * Loads credential list from cache.
 *
 * @return true on success
 */
bool fido2_init(void);

/**
 * Set user presence callback.
 * Called when authentication requires user confirmation.
 *
 * @param cb Callback function (NULL to disable prompts)
 */
void fido2_set_user_presence_callback(fido2_user_presence_cb_t cb);

/**
 * Request user presence (called from CTAP2).
 * Invokes the registered callback if set.
 *
 * @param rp_id Relying party identifier
 * @param action Registration or authentication
 * @param user_name User display name (optional, may be NULL)
 * @return User presence result
 */
fido2_user_presence_result_t fido2_request_user_presence(
    const char *rp_id,
    fido2_action_t action,
    const char *user_name
);

/**
 * Set/check PIN verification status (from ClientPIN protocol).
 * When PIN was verified via ClientPIN, device PIN entry can be skipped.
 */
void fido2_set_pin_verified(bool verified);
bool fido2_is_pin_verified(void);

// ============================================================================
// Credential Management
// ============================================================================

/**
 * Get number of stored credentials.
 */
uint8_t fido2_get_credential_count(void);

/**
 * Get credential info by index.
 *
 * @param index Credential index (0 to count-1)
 * @param info Output structure
 * @return true if credential exists
 */
bool fido2_get_credential_info(uint8_t index, fido2_credential_info_t *info);

/**
 * Find credential by RP ID hash.
 *
 * @param rp_id_hash SHA-256 of RP ID (32 bytes)
 * @param out_indices Array to store matching indices
 * @param max_indices Maximum number of indices to return
 * @return Number of matching credentials
 */
uint8_t fido2_find_credentials_by_rp(const uint8_t *rp_id_hash,
                                      uint8_t *out_indices, uint8_t max_indices);

/**
 * Delete credential by slot.
 *
 * @param slot ECC slot index (0-28)
 * @return true on success
 */
bool fido2_delete_credential(uint8_t slot);

/**
 * Factory reset - delete all credentials.
 *
 * @return true on success
 */
bool fido2_factory_reset(void);

// ============================================================================
// Authentication Counter
// ============================================================================

/**
 * Get global authentication counter.
 * Stored in NVS (not TROPIC01 to prevent lockout).
 */
uint32_t fido2_get_auth_counter(void);

/**
 * Increment global authentication counter.
 */
void fido2_increment_auth_counter(void);

// ============================================================================
// Status
// ============================================================================

/**
 * Check if FIDO2 module is initialized.
 */
bool fido2_is_initialized(void);

/**
 * Get available credential slots.
 */
uint8_t fido2_get_available_slots(void);

#ifdef __cplusplus
}
#endif

