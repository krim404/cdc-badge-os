#pragma once

#include "mod_gpg/GpgRecvStore.h"
#include <cstddef>
#include <cstdint>

namespace cdc::mod_gpg {

/**
 * \brief Cross-sign a received key with the badge's own SIG ECC slot.
 *
 * Builds the RFC 4880 certification hash (sig type 0x10 over the target's
 * Public Key Packet + User ID Packet), signs it with the badge's signature
 * subkey via TROPIC01, and returns R || S as 64 bytes.
 *
 * The badge's own curve is read from `gpg_get_status`; sig length is always
 * 64 regardless of curve (Ed25519 and P-256 both produce 32+32-byte
 * concatenated R || S in this implementation).
 *
 * \param target Received key descriptor.
 * \param sig_creation_time Timestamp embedded into the hashed-subpackets.
 * \param out_sig 64-byte output (R || S).
 * \return `true` on success.
 */
bool gpgCrossSign(const gpg_recv_key_t& target,
                  uint32_t sig_creation_time,
                  uint8_t out_sig[64]);

/**
 * \brief Build the OpenPGP Signature Packet body certifying `key`.
 *
 * Assembles the Tag 2 packet *body* (version, sig type 0x10, algo, hash algo,
 * hashed + unhashed subpackets, left-16 hash bits and the R || S MPIs) for the
 * cross-signature already stored in `key.my_signature`. No tag/length header is
 * written. The same bytes are used by the armored export and the badge-to-badge
 * certification return message so both stay identical.
 *
 * \param key Received key descriptor with a populated `my_signature` / `sig_created_at`.
 * \param out Output buffer.
 * \param out_size Capacity of `out`.
 * \return Body length, or 0 on error / buffer overflow.
 */
size_t buildCertSigPacket(const gpg_recv_key_t& key, uint8_t* out, size_t out_size);

/**
 * \brief Build an ASCII-armored OpenPGP block for the badge's own public key,
 *        appending every stored third-party certification on it.
 *
 * Packs the badge's own Public Key Packet (Tag 6) + User ID Packet (Tag 13)
 * followed by one Certification Signature Packet (Tag 2) per cert held in
 * `GpgSelfCertStore`. Importing the result merges the accumulated third-party
 * signatures onto the badge's key in a GPG keyring.
 *
 * \param out Output character buffer.
 * \param out_size Capacity of `out`.
 * \param out_len Bytes written (excluding any terminating null).
 * \return `true` on success; `false` on buffer overflow or no key configured.
 */
bool gpgBuildOwnSignedKeyArmored(char* out, size_t out_size, size_t* out_len);

/**
 * \brief Build an ASCII-armored OpenPGP block carrying the cross-signed key.
 *
 * Packs the received key as Public Key Packet (Tag 6) + User ID Packet
 * (Tag 13) + Certification Signature Packet (Tag 2) into a single
 * `BEGIN/END PGP PUBLIC KEY BLOCK` payload. Suitable for `gpg --import`.
 *
 * \param key Received key descriptor (must already have `my_signature`).
 * \param out Output character buffer.
 * \param out_size Capacity of `out`.
 * \param out_len Bytes written (excluding any terminating null).
 * \return `true` on success; `false` on buffer overflow or invalid input.
 */
bool gpgBuildSignedKeyArmored(const gpg_recv_key_t& key,
                              char* out, size_t out_size,
                              size_t* out_len);

/**
 * \brief Build an ASCII-armored OpenPGP public-key block (no signature).
 *
 * Packs the received key as Public Key Packet (Tag 6) + User ID Packet
 * (Tag 13) into a single `BEGIN/END PGP PUBLIC KEY BLOCK` payload, suitable
 * for `gpg --import` or display as a QR code. Works for unsigned keys.
 *
 * \param key Received key descriptor.
 * \param out Output character buffer.
 * \param out_size Capacity of `out`.
 * \param out_len Bytes written (excluding any terminating null).
 * \return `true` on success; `false` on buffer overflow or invalid input.
 */
bool gpgBuildPublicKeyArmored(const gpg_recv_key_t& key,
                              char* out, size_t out_size,
                              size_t* out_len);

} // namespace cdc::mod_gpg
