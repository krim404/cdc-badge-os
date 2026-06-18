#pragma once

#include "mod_gpg/GpgRecvStore.h"
#include "mod_gpg/GpgSelfCertStore.h"

#include <cstddef>
#include <cstdint>

namespace cdc::mod_gpg {

/**
 * \file
 * \brief (De)serialisation of a badge-to-badge certification return message.
 *
 * Wire payload layout (carried as MIME `application/pgp-signature`):
 *   20B target_fp_v4, 20B issuer_fp_v4, 1B issuer_uid_len,
 *   up to 63 issuer_uid bytes, 2B sig_pkt_len (big-endian),
 *   sig_pkt_len bytes (the verbatim OpenPGP Signature Packet body).
 *
 * `target_fp_v4` identifies the key being certified (the original owner the
 * message is sent back to); the receiver must reject the payload unless it
 * matches its own SIG key. `issuer_*` describe the signer for display.
 */

/// Minimum payload size (empty issuer uid, 1-byte signature packet).
constexpr size_t kGpgCertPayloadMin = 20 + 20 + 1 + 2 + 1;
/// Maximum payload size (maximum issuer uid + maximum signature packet).
constexpr size_t kGpgCertPayloadMax = 20 + 20 + 1 + 63 + 2 + kGpgSelfCertSigMax;

/**
 * \brief Serialise a certification return message for a cross-signed key.
 *
 * Builds the Tag 2 signature packet from `signed_key` (which must already hold
 * `my_signature`) and frames it with the target fingerprint and the badge's own
 * issuer identity.
 *
 * \param signed_key Received key that was cross-signed by this badge.
 * \param out Output buffer.
 * \param out_size Capacity of \p out.
 * \return Bytes written, or 0 on failure (key unsigned / buffer too small).
 */
size_t gpgBuildCertPayload(const gpg_recv_key_t& signed_key,
                           uint8_t* out, size_t out_size);

/**
 * \brief Parse a certification return message into a self-cert record.
 *
 * \param data Payload bytes.
 * \param len Payload length.
 * \param out Output self-cert record (zeroed on entry, `received_at` stamped).
 * \param out_target_fp 20-byte buffer receiving the certified-key fingerprint.
 * \return `true` on a well-formed payload, otherwise `false`.
 */
bool gpgParseCertPayload(const uint8_t* data, size_t len,
                         gpg_self_cert_t* out, uint8_t out_target_fp[20]);

} // namespace cdc::mod_gpg
