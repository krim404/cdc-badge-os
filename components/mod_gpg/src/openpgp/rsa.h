/**
 * \brief Software RSA backend for the OpenPGP card (mbedTLS).
 *
 * The TROPIC01 secure element handles only ECC, so RSA roles are software
 * keys. The serialized private-key blob layout (stored encrypted in R-Memory
 * by GpgStorage) is, all length fields big-endian:
 *
 *   [n_bits:u16][e_len:u16][e...][p_len:u16][p...][q_len:u16][q...]
 *
 * Only the two primes plus the public exponent are kept; mbedTLS reconstructs
 * the remaining CRT parameters on load.
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \brief Supported RSA modulus sizes share this maximum byte count (4096). */
#define GPG_RSA_MAX_MODULUS_BYTES 512

/**
 * \brief Serialises raw RSA components into a private-key blob.
 * \param n_bits Modulus length in bits (2048 / 3072 / 4096).
 * \param e Public exponent bytes (big-endian).
 * \param e_len Public exponent length.
 * \param p First prime (big-endian).
 * \param p_len First prime length.
 * \param q Second prime (big-endian).
 * \param q_len Second prime length.
 * \param blob_out Output buffer.
 * \param blob_cap Output capacity.
 * \param blob_len_out Receives the serialized length.
 * \return `true` when the components form a valid RSA private key.
 */
bool gpg_rsa_blob_build(uint16_t n_bits, const uint8_t* e, size_t e_len,
                        const uint8_t* p, size_t p_len, const uint8_t* q, size_t q_len,
                        uint8_t* blob_out, size_t blob_cap, size_t* blob_len_out);

/**
 * \brief Generates a fresh RSA key pair and serialises its private blob.
 *        The public exponent is fixed to 65537. This is computationally heavy
 *        on the ESP32-S3 (seconds to minutes for 4096); the caller is expected
 *        to keep the task watchdog fed.
 * \param n_bits Modulus length in bits (2048 / 3072 / 4096).
 * \param blob_out Output buffer.
 * \param blob_cap Output capacity.
 * \param blob_len_out Receives the serialized length.
 * \return `true` on success.
 */
bool gpg_rsa_generate(uint16_t n_bits, uint8_t* blob_out, size_t blob_cap, size_t* blob_len_out);

/**
 * \brief Extracts the public modulus and exponent from a private-key blob.
 * \param blob Serialized private-key blob.
 * \param blob_len Blob length.
 * \param n_out Receives the modulus (big-endian, n_bits/8 bytes).
 * \param n_cap Capacity of `n_out`.
 * \param n_len_out Receives the modulus length.
 * \param e_out Receives the public exponent (big-endian).
 * \param e_cap Capacity of `e_out`.
 * \param e_len_out Receives the exponent length.
 * \return `true` on success.
 */
bool gpg_rsa_blob_public(const uint8_t* blob, size_t blob_len,
                         uint8_t* n_out, size_t n_cap, size_t* n_len_out,
                         uint8_t* e_out, size_t e_cap, size_t* e_len_out);

/**
 * \brief RSASSA-PKCS1-v1.5 signature over a host-supplied DigestInfo.
 *
 * GnuPG sends the full DER DigestInfo as the PSO:CDS / INTERNAL AUTHENTICATE
 * payload; the card applies the EMSA-PKCS1-v1.5 padding and the raw RSA
 * private operation.
 * \param blob Serialized private-key blob.
 * \param blob_len Blob length.
 * \param digestinfo DigestInfo bytes to sign.
 * \param di_len DigestInfo length.
 * \param sig_out Output signature buffer (>= n_bits/8 bytes).
 * \param sig_cap Capacity of `sig_out`.
 * \param sig_len_out Receives the signature length (= n_bits/8).
 * \return `true` on success.
 */
bool gpg_rsa_sign(const uint8_t* blob, size_t blob_len,
                  const uint8_t* digestinfo, size_t di_len,
                  uint8_t* sig_out, size_t sig_cap, size_t* sig_len_out);

/**
 * \brief RSAES-PKCS1-v1.5 decryption of a cryptogram.
 * \param blob Serialized private-key blob.
 * \param blob_len Blob length.
 * \param ct Ciphertext (exactly n_bits/8 bytes).
 * \param ct_len Ciphertext length.
 * \param pt_out Output plaintext buffer.
 * \param pt_cap Capacity of `pt_out`.
 * \param pt_len_out Receives the recovered plaintext length.
 * \return `true` on success.
 */
bool gpg_rsa_decrypt(const uint8_t* blob, size_t blob_len,
                     const uint8_t* ct, size_t ct_len,
                     uint8_t* pt_out, size_t pt_cap, size_t* pt_len_out);

/**
 * \brief End-to-end self-test: generate a key, serialise/reload its blob, then
 *        sign+verify and encrypt+decrypt a known sample. Exercises the full
 *        software RSA path on the running firmware (no SE / NVS).
 * \param n_bits Modulus length to test (2048 / 3072 / 4096).
 * \return `true` if every step round-trips correctly.
 */
bool gpg_rsa_selftest(uint16_t n_bits);

#ifdef __cplusplus
}
#endif
