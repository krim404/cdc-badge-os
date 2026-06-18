// FIDO2 Storage Layer (TROPIC01 + NVS)
// Handles credential storage in ECC slots and R-Memory

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "mod_fido2/fido2.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Storage Layout
// ============================================================================
// ECC Slots:         Private keys (P-256 or Ed25519 for WebAuthn/SSH)
// R-Memory slots:    Credential metadata (rp_id, user_id, sign_count, curve, etc.)
// NVS "fido2":       Global auth counter, PIN hash

void fido2_storage_set_slot_range(uint8_t ecc_start, uint8_t ecc_end,
                                  uint16_t rmem_start, uint16_t rmem_end);
uint8_t fido2_storage_ecc_start(void);
uint8_t fido2_storage_ecc_end(void);
uint16_t fido2_storage_rmem_start(void);
uint16_t fido2_storage_rmem_end(void);

// ============================================================================
// Initialization
// ============================================================================

/**
 * Initialize storage layer.
 * Loads credential metadata from TROPIC01 cache.
 *
 * @return Number of credentials found
 */
uint8_t fido2_storage_init(void);

// ============================================================================
// Credential Operations
// ============================================================================

/**
 * Create a new credential.
 *
 * @param rp_id Relying Party ID string
 * @param rp_id_hash SHA-256 hash of RP ID (32 bytes)
 * @param user_id User handle (opaque bytes)
 * @param user_id_len Length of user handle
 * @param user_name Display name (can be empty)
 * @param resident_key Store as discoverable credential
 * @param cred_protect Credential protection level (0-3)
 * @param curve CDC_CURVE_P256 or CDC_CURVE_ED25519
 * @param out_slot Output: allocated slot index
 * @param out_cred_id Output: credential ID (64 bytes)
 * @param out_pubkey Output: public key (64 bytes for P-256 X||Y, 32 bytes for Ed25519)
 * @return true on success
 */
bool fido2_storage_create_credential(
    const char *rp_id,
    const uint8_t *rp_id_hash,
    const uint8_t *user_id,
    uint8_t user_id_len,
    const char *user_name,
    bool resident_key,
    uint8_t cred_protect,
    uint8_t curve,
    uint8_t *out_slot,
    uint8_t *out_cred_id,
    uint8_t *out_pubkey
);

/**
 * Get curve type for a credential.
 *
 * @param slot Logical slot index (0..count-1)
 * @return CDC_CURVE_P256 or CDC_CURVE_ED25519, or 0xFF on error
 */
uint8_t fido2_storage_get_curve(uint8_t slot);

/**
 * Get credential info by slot.
 *
 * @param slot Logical slot index (0..count-1)
 * @param info Output structure
 * @return true if credential exists
 */
bool fido2_storage_get_credential(uint8_t slot, fido2_credential_info_t *info);

/**
 * Delete credential by slot.
 *
 * @param slot Logical slot index (0..count-1)
 * @return true on success
 */
bool fido2_storage_delete_credential(uint8_t slot);

/**
 * Increment and get sign count for a credential.
 *
 * @param slot Logical slot index
 * @return New sign count, or 0 on error
 */
uint32_t fido2_storage_increment_sign_count(uint8_t slot);

// ============================================================================
// Lookup Operations (use cache - no TROPIC01 access)
// ============================================================================

/**
 * Get total credential count.
 */
uint8_t fido2_storage_count(void);

/**
 * Check if slot has a credential.
 */
bool fido2_storage_slot_used(uint8_t slot);

/**
 * Find first free slot.
 *
 * @return Slot index, or -1 if full
 */
int8_t fido2_storage_find_free_slot(void);

/**
 * Find credentials matching RP ID hash.
 *
 * @param rp_id_hash SHA-256 of RP ID
 * @param out_slots Array to store matching slot indices
 * @param max_slots Array size
 * @return Number of matches
 */
uint8_t fido2_storage_find_by_rp(const uint8_t *rp_id_hash,
                                  uint8_t *out_slots, uint8_t max_slots);

/**
 * Find resident (discoverable) credentials matching RP ID hash.
 *
 * @param rp_id_hash SHA-256 of RP ID
 * @param out_slots Array to store matching slot indices
 * @param max_slots Array size
 * @return Number of matches
 */
uint8_t fido2_storage_find_by_rp_resident(const uint8_t *rp_id_hash,
                                          uint8_t *out_slots, uint8_t max_slots);

/**
 * Find existing credential for same RP ID + User ID combination.
 * Used to detect credentials that should be replaced (per FIDO2 spec).
 *
 * @param rp_id_hash SHA-256 of RP ID
 * @param user_id User handle
 * @param user_id_len Length of user handle
 * @return Slot index if found, -1 if not found
 */
int8_t fido2_storage_find_by_rp_user(const uint8_t *rp_id_hash,
                                      const uint8_t *user_id,
                                      uint8_t user_id_len);

/**
 * Check if slot contains a resident (discoverable) credential.
 */
bool fido2_storage_is_resident(uint8_t slot);

/**
 * Find slot by credential ID.
 *
 * @param cred_id Credential ID bytes
 * @param cred_id_len Credential ID length
 * @return Slot index, or -1 if not found
 */
int8_t fido2_storage_find_slot_by_cred_id(const uint8_t *cred_id, uint16_t cred_id_len);

/**
 * Get user information for a credential slot.
 *
 * @param slot ECC slot index
 * @param user_id Output user handle buffer
 * @param user_id_len Output user handle length
 * @param user_name Output user name buffer (can be NULL)
 * @param user_name_max Output buffer size for user_name
 * @return true on success
 */
bool fido2_storage_get_user(uint8_t slot,
                            uint8_t *user_id,
                            uint8_t *user_id_len,
                            char *user_name,
                            size_t user_name_max);

/**
 * Verify credential ID belongs to slot.
 *
 * @param slot ECC slot index
 * @param cred_id Credential ID to verify
 * @return true if matches
 */
bool fido2_storage_verify_cred_id(uint8_t slot, const uint8_t *cred_id);

/**
 * Get credential ID for slot.
 *
 * @param slot ECC slot index
 * @param out_cred_id Output buffer (FIDO2_CRED_ID_LEN bytes)
 * @return true on success
 */
bool fido2_storage_get_cred_id(uint8_t slot, uint8_t *out_cred_id);

// ============================================================================
// Signing Operations (requires TROPIC01 access)
// ============================================================================

/**
 * Sign raw message with credential key, returns DER-encoded signature.
 * TROPIC01 computes SHA256(msg) internally before signing.
 *
 * @param slot ECC slot index
 * @param msg Raw message to sign (NOT a pre-computed hash!)
 * @param msg_len Length of message
 * @param signature Output DER-encoded signature
 * @param sig_len Output signature length
 * @return true on success
 */
bool fido2_storage_sign(uint8_t slot, const uint8_t *msg, uint16_t msg_len,
                        uint8_t *signature, uint8_t *sig_len);

/**
 * Sign raw message with credential private key (TROPIC01 hashes internally).
 * Returns raw signature format (r || s = 64 bytes) for CTAP2/WebAuthn.
 *
 * @param slot ECC slot index
 * @param msg Raw message to sign (authData || clientDataHash)
 * @param msg_len Message length
 * @param signature Output raw signature (64 bytes)
 * @param sig_len Output signature length (always 64)
 * @return true on success
 */
bool fido2_storage_sign_raw(uint8_t slot, const uint8_t *msg, uint16_t msg_len,
                            uint8_t *signature, uint8_t *sig_len);

/**
 * Sign raw message with credential private key (TROPIC01 hashes internally).
 * Returns DER-encoded signature for U2F compatibility.
 *
 * @param slot ECC slot index
 * @param msg Raw message to sign (authData || clientDataHash)
 * @param msg_len Message length
 * @param signature Output DER-encoded signature
 * @param sig_len Output signature length
 * @return true on success
 */
bool fido2_storage_sign_der(uint8_t slot, const uint8_t *msg, uint16_t msg_len,
                            uint8_t *signature, uint8_t *sig_len);

/**
 * Get public key for credential.
 *
 * @param slot ECC slot index
 * @param pubkey Output: uncompressed P-256 public key (65 bytes)
 * @return true on success
 */
bool fido2_storage_get_pubkey(uint8_t slot, uint8_t *pubkey);

// ============================================================================
// NVS Counter Operations
// ============================================================================

/**
 * Load global auth counter from NVS.
 */
void fido2_storage_counter_load(void);

/**
 * Get global auth counter value.
 */
uint32_t fido2_storage_counter_get(void);

/**
 * Increment and save global auth counter.
 *
 * @return true if counter was successfully persisted to NVS
 */
bool fido2_storage_counter_increment(void);

/**
 * No-op flush kept for API stability; per-increment path commits.
 *
 * @return true.
 */
bool fido2_storage_counter_flush(void);

/* --- authenticatorLargeBlobs (CTAP2.1) persistence, NVS blob. --- */

/**
 * Loads the stored large-blob array, or the canonical empty array if unset.
 *
 * @param out Output buffer.
 * @param max_len Output buffer capacity.
 * @param out_len Receives the array length.
 * @return true on success.
 */
bool fido2_storage_largeblob_get(uint8_t* out, uint16_t max_len, uint16_t* out_len);

/**
 * Persists the full serialized large-blob array.
 *
 * @param data Array bytes.
 * @param len Array length (1..1024).
 * @return true on success.
 */
bool fido2_storage_largeblob_set(const uint8_t* data, uint16_t len);

/** @return Stored large-blob array length, or the empty-array length if unset. */
uint16_t fido2_storage_largeblob_length(void);

/* --- authenticatorConfig (CTAP2.1) persistent flags. --- */

/** @return true when alwaysUv is enabled. */
bool fido2_storage_get_always_uv(void);

/** Sets the alwaysUv flag. @return true on success. */
bool fido2_storage_set_always_uv(bool enabled);

/** @return Stored minimum PIN length, or 0 when unset (i.e. use the default). */
uint8_t fido2_storage_get_min_pin_len(void);

/** Persists the minimum PIN length. @return true on success. */
bool fido2_storage_set_min_pin_len(uint8_t min_len);

/** Erases all CTAP2.1 large-blob and config keys (used by factory reset). */
void fido2_storage_config_reset(void);

#ifdef __cplusplus
}
#endif

