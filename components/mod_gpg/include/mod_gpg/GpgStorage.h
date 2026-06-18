#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

void gpg_storage_set_slot_range(uint16_t eccStart, uint16_t eccEnd);
void gpg_storage_set_rmem_range(uint16_t rmemStart, uint16_t rmemEnd);
bool gpg_storage_ready(void);
uint8_t gpg_storage_sig_slot(void);
uint8_t gpg_storage_dec_slot(void);
uint8_t gpg_storage_aut_slot(void);

/**
 * \brief Saves a DEC private key into R-Memory using PIN-bound AES-GCM.
 * \param privkey 32-byte P-256 private key scalar.
 * \param pin Session PIN; `nullptr` falls back to chip-bound key.
 * \return `true` on success.
 */
bool gpg_storage_save_dec_privkey(const uint8_t* privkey, const char* pin);

/**
 * \brief Loads and decrypts the DEC private key from R-Memory.
 * \param privkey_out 32-byte output buffer.
 * \param pin Session PIN; `nullptr` falls back to chip-bound key.
 * \return `true` on success.
 */
bool gpg_storage_load_dec_privkey(uint8_t* privkey_out, const char* pin);

/** \brief Returns `true` if encrypted DEC private key record exists. */
bool gpg_storage_has_dec_privkey(void);

/** \brief Deletes DEC private key record. */
bool gpg_storage_delete_dec_privkey(void);

/**
 * \brief Saves the symmetric AES key for PSO:DECIPHER (DO 0xD5).
 * \param key AES key bytes (16 or 32).
 * \param key_len Key length (16 or 32).
 * \param pin Session PIN; `nullptr` falls back to chip-bound key.
 * \return `true` on success.
 */
bool gpg_storage_save_aes_key(const uint8_t* key, size_t key_len, const char* pin);

/**
 * \brief Loads the symmetric AES key from R-Memory.
 * \param key_out Output buffer (must hold at least 32 bytes).
 * \param key_len_out Receives the stored key length (16 or 32).
 * \param pin Session PIN; `nullptr` falls back to chip-bound key.
 * \return `true` on success.
 */
bool gpg_storage_load_aes_key(uint8_t* key_out, size_t* key_len_out, const char* pin);

/** \brief Returns `true` if a symmetric AES key record exists. */
bool gpg_storage_has_aes_key(void);

/** \brief Deletes the symmetric AES key record. */
bool gpg_storage_delete_aes_key(void);

/** \brief Maximum serialized RSA private-key blob (RSA-4096 n_bits||e||p||q). */
#define GPG_RSA_BLOB_MAX 560

/**
 * \brief Sets the dedicated R-Memory range used for RSA private-key blobs.
 *
 * Distinct from the primary GPG range; carved as `mod_gpg_rsa` in the slot map.
 * Each of the three roles occupies two consecutive slots so an RSA-4096 blob
 * can span them.
 * \param start First absolute R-Memory slot of the RSA pool.
 * \param end Last absolute R-Memory slot of the RSA pool.
 */
void gpg_storage_set_rsa_slot_range(uint16_t start, uint16_t end);

/**
 * \brief Saves an encrypted RSA private-key blob for a key role.
 * \param role 0 = SIG, 1 = DEC, 2 = AUT.
 * \param blob Serialized RSA private material.
 * \param blob_len Blob length (<= GPG_RSA_BLOB_MAX).
 * \param pin Session PIN; `nullptr` falls back to chip-bound key.
 * \return `true` on success.
 */
bool gpg_storage_save_rsa_key(uint8_t role, const uint8_t* blob, size_t blob_len, const char* pin);

/**
 * \brief Loads and decrypts the RSA private-key blob for a key role.
 * \param role 0 = SIG, 1 = DEC, 2 = AUT.
 * \param blob_out Output buffer (>= GPG_RSA_BLOB_MAX bytes).
 * \param blob_cap Capacity of `blob_out`.
 * \param blob_len_out Receives the decrypted blob length.
 * \param pin Session PIN; `nullptr` falls back to chip-bound key.
 * \return `true` on success.
 */
bool gpg_storage_load_rsa_key(uint8_t role, uint8_t* blob_out, size_t blob_cap,
                              size_t* blob_len_out, const char* pin);

/** \brief Returns `true` if an RSA private-key blob exists for the role. */
bool gpg_storage_has_rsa_key(uint8_t role);

/** \brief Deletes the RSA private-key blob for the role (both slots). */
bool gpg_storage_delete_rsa_key(uint8_t role);

/**
 * \brief Returns current session key if session is active.
 * \param key_out 32-byte output buffer.
 * \return `true` if session key is available.
 */
bool gpg_storage_get_session_key(uint8_t* key_out);

/**
 * \brief Stores session PIN-derived key after successful PIN verification.
 * \param pin Verified PIN string.
 */
void gpg_storage_set_session_pin(const char* pin);

/** \brief Clears the cached session key. */
void gpg_storage_clear_session(void);

#ifdef __cplusplus
}
#endif
