#pragma once

#include "mod_gpg/GpgRecvStore.h"

#include <cstddef>
#include <cstdint>

namespace cdc::mod_gpg {

/**
 * \file
 * \brief Transport-agnostic (de)serialisation of a GPG public key.
 *
 * Wire payload layout (badge-to-badge, carried as MIME `application/pgp-keys`):
 *   1B curve, 1B pubkey_len, 32 or 64 pubkey bytes (SIG primary key),
 *   4B created_at (big-endian), 20B fingerprint_v4,
 *   1B user_id_len, up to 63 user_id bytes,
 *   4B created_at_dec, 64B pubkey_dec (P-256 X || Y, DEC encryption subkey),
 *   64B owner_self_sig, 64B dec_binding_sig.
 *
 * `created_at` is the originator's key-creation timestamp. The receiver
 * recomputes the v4 fingerprint from (curve, pubkey, created_at) and rejects
 * the payload if it does not match the transmitted `fingerprint_v4`, so a
 * later certification binds to the peer's real OpenPGP key. The DEC subkey and
 * its two signatures travel verbatim so the exchanged key imports as a full
 * sign + encrypt OpenPGP key.
 */

/// Fixed-size trailing DEC encryption-subkey block.
constexpr size_t kGpgKeyDecBlock = 4 + 64 + 64 + 64;
/// Minimum payload size (Ed25519 + empty user id + DEC block).
constexpr size_t kGpgKeyPayloadMin = 1 + 1 + 32 + 4 + 20 + 1 + 1 + kGpgKeyDecBlock;
/// Maximum payload size (P-256 + maximum user id + DEC block).
constexpr size_t kGpgKeyPayloadMax = 1 + 1 + 64 + 4 + 20 + 1 + 63 + kGpgKeyDecBlock;

/**
 * \brief Serialise the badge's own public key into the wire payload.
 * \param out Output buffer.
 * \param out_size Capacity of \p out.
 * \return Bytes written, or 0 on failure (no key configured / buffer too small).
 */
size_t gpgBuildOwnKeyPayload(uint8_t* out, size_t out_size);

/**
 * \brief Serialise a stored received key into the wire payload (for forwarding).
 * \param key Received key descriptor.
 * \param out Output buffer.
 * \param out_size Capacity of \p out.
 * \return Bytes written, or 0 on failure (buffer too small).
 */
size_t gpgBuildRecvKeyPayload(const gpg_recv_key_t& key, uint8_t* out, size_t out_size);

/**
 * \brief Parse a wire payload into a key record, computing the V5 fingerprint.
 * \param data Payload bytes.
 * \param len Payload length.
 * \param out Output key record (zeroed on entry).
 * \return `true` on a well-formed payload, otherwise `false`.
 */
bool gpgParseKeyPayload(const uint8_t* data, size_t len, gpg_recv_key_t* out);

} // namespace cdc::mod_gpg
