#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::mod_gpg {

/**
 * \brief Compute the RFC 4880 V4 OpenPGP fingerprint (SHA-1, 20 bytes).
 * \param curve `CDC_CURVE_ED25519` or `CDC_CURVE_P256`.
 * \param pubkey 32-byte Ed25519 point or raw 64-byte P-256 (X || Y, no SEC1 prefix).
 * \param pubkey_len Length of `pubkey` (32 or 64).
 * \param created_at Key creation timestamp (Unix epoch seconds).
 * \param out_fp 20-byte output buffer.
 * \return `true` on success.
 */
bool calculateFingerprintV4(uint8_t curve,
                            const uint8_t* pubkey, size_t pubkey_len,
                            uint32_t created_at,
                            uint8_t out_fp[20]);

/**
 * \brief Compute the V5 / RFC 9580 OpenPGP fingerprint (SHA-256, 32 bytes).
 *
 * Uses the same body layout as V4 (version 0x04, EdDSA/ECDSA algo byte,
 * curve OID, MPI). Differs from V4 in the hash function and in the prefix:
 * `0x9A || 4-byte body length` (vs V4's `0x99 || 2-byte length`).
 */
bool calculateFingerprintV5(uint8_t curve,
                            const uint8_t* pubkey, size_t pubkey_len,
                            uint32_t created_at,
                            uint8_t out_fp[32]);

/**
 * \brief Serialise an RFC 6637 ECDH Public Key Packet body (algorithm 18).
 *
 * Layout: version 0x04, 4-byte creation time, ECDH algorithm id, P-256 curve
 * OID, the MPI-encoded uncompressed point, then the KDF parameter field
 * `03 01 <hash> <sym>`. Used for the decryption (DEC) encryption subkey so the
 * exported packet and the stored fingerprint stay byte-identical.
 *
 * \param pubkey Raw 64-byte P-256 point (X || Y, no SEC1 prefix).
 * \param created_at Subkey creation timestamp (Unix epoch seconds).
 * \param out Output buffer.
 * \param out_size Capacity of \p out.
 * \return body length, or 0 on failure.
 */
size_t buildEcdhPubkeyBody(const uint8_t* pubkey, uint32_t created_at,
                           uint8_t* out, size_t out_size);

/**
 * \brief Compute the V4 fingerprint of an ECDH (DEC) subkey.
 *
 * Hashes the RFC 6637 ECDH public-key body (see buildEcdhPubkeyBody) so the
 * result matches what GnuPG derives from the exported encryption subkey.
 *
 * \param pubkey Raw 64-byte P-256 point (X || Y).
 * \param created_at Subkey creation timestamp.
 * \param out_fp 20-byte output buffer.
 * \return `true` on success.
 */
bool calculateFingerprintV4Ecdh(const uint8_t* pubkey, uint32_t created_at,
                                uint8_t out_fp[20]);

/**
 * \brief Build the digest input for a cross-signature.
 *
 * Per `docs/CROSS_SIGNING.md`:
 *   data = fp_v4 (20 B) || user_id padded with zeros to 64 B
 *   hash = SHA-256(data)
 */
bool gpgCrossSignDigest(const uint8_t fp_v4[20],
                        const char* user_id,
                        uint8_t out_hash[32]);

} // namespace cdc::mod_gpg
