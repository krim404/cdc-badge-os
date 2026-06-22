#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDC_CURVE_ED25519 0
#define CDC_CURVE_P256    1

#define GPG_USER_ID_MAX         64
#define GPG_FINGERPRINT_LEN     20
#define GPG_PUBKEY_MAX_LEN      64

#ifdef __DOXYGEN__
namespace cdc::mod_gpg {
#endif

/**
 * \brief Snapshot of the current OpenPGP card-application state for UI display.
 */
typedef struct {
    bool initialized;
    uint8_t curve;
    char user_id[GPG_USER_ID_MAX];
    uint8_t fingerprint[GPG_FINGERPRINT_LEN];
    uint32_t created_at;
    uint32_t sign_count;
} gpg_status_t;

#ifdef __DOXYGEN__
} // namespace cdc::mod_gpg
#endif

/**
 * \brief Initializes the GPG module bookkeeping.
 *
 * No persistent state is loaded here: the OpenPGP card application owns the
 * canonical state in its own NVS blob (see openpgp.h) and the device-UI reads
 * straight from there.
 */
bool gpg_init(void);

/**
 * \brief Reports whether at least one OpenPGP key role has a configured
 *        fingerprint on the card.
 */
bool gpg_is_initialized(void);

/**
 * \brief Fills \p status from the OpenPGP card-application state.
 * \return `true` if a key snapshot was returned, `false` if no key configured.
 */
bool gpg_get_status(gpg_status_t *status);

/**
 * \brief Stages a user-id string for the next on-device key generation.
 * The string is forwarded to OpenpgpNvsState::cardholder_name during
 * gpg_generate_key() so that gpg --card-status sees it.
 */
bool gpg_set_pending_user_id(const char *user_id);

/**
 * \brief Returns whether a user-id was staged via gpg_set_pending_user_id().
 */
bool gpg_has_pending_user_id(void);

/**
 * \brief Generates SIG / DEC / AUT keys on the device and announces them to
 *        the OpenPGP card application (fingerprints, gen-time, cardholder name).
 *
 * Used by the on-device wizard. The host-side `gpg --card-edit -> generate`
 * path goes through CCID and bypasses this function entirely.
 */
bool gpg_generate_key(uint8_t curve);

/**
 * \brief Factory-resets all GPG key material and metadata.
 *
 * Wipes ECC slots, the wrapped DEC private key, the NVS-resident OpenPGP
 * state (fingerprints, gen-times, counter, cardholder, RC) and the PINs.
 */
bool gpg_reset(void);

/**
 * \brief Renders the current SIG public key as a SubjectPublicKeyInfo PEM.
 * The key is read straight from the secure element.
 */
bool gpg_export_pubkey_pem(char *buf, size_t size, size_t *out_len);

/**
 * \brief Derives the DEC (decryption) public key as an uncompressed P-256 point.
 *
 * Loads the encrypted DEC private key (chip-bound), derives its public point and
 * zeroes the private scalar. Used to append the encryption subkey to exported
 * and badge-to-badge transferred public keys.
 *
 * \param pub65 65-byte output buffer (0x04 || X || Y).
 * \return `true` on success.
 */
bool gpg_get_dec_pubkey(uint8_t *pub65);

/**
 * \brief Writes the alchemical-word fingerprint of the SIG public key.
 *
 * Reads the current SIG key from the secure element, derives a SHA-256, and
 * encodes the first 25 bits as five space-separated words from the shared
 * 32-word alchemy table. Intended for visual comparison between two devices.
 *
 * \param buf Output buffer; must be at least KEY_FINGERPRINT_MAX_LEN bytes.
 * \param len Capacity of \p buf.
 * \return `true` on success. On failure the buffer holds an error placeholder.
 */
bool gpg_alchemy_fingerprint(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
