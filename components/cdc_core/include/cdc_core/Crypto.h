/**
 * \file Crypto.h
 * \brief Shared AES-256-GCM helpers built on mbedTLS.
 *
 * Single source of truth for the AES-256-GCM seal/open pattern used across the
 * firmware (encrypted backups, GPG key storage, plugin host crypto API),
 * centralizing the `mbedtls_gcm_init`/`setkey`/`crypt_and_tag`/`auth_decrypt`/
 * `free` sequence.
 */

#pragma once

#include <mbedtls/gcm.h>

#include <cstddef>
#include <cstdint>

namespace cdc::core {

/** \brief RAII wrapper around `mbedtls_gcm_context`. Non-copyable, non-movable. */
class GcmContext {
public:
    GcmContext() { mbedtls_gcm_init(&ctx_); }
    ~GcmContext() { mbedtls_gcm_free(&ctx_); }
    GcmContext(const GcmContext&) = delete;
    GcmContext& operator=(const GcmContext&) = delete;
    GcmContext(GcmContext&&) = delete;
    GcmContext& operator=(GcmContext&&) = delete;
    /** \brief Returns the underlying mbedTLS context. */
    mbedtls_gcm_context* get() { return &ctx_; }
private:
    mbedtls_gcm_context ctx_;
};

/**
 * \brief Encrypts \p pt with AES-256-GCM and produces a 16-byte tag.
 * \param key 32-byte AES key.
 * \param iv Initialization vector / nonce.
 * \param ivLen IV length in bytes (12 for the canonical GCM nonce).
 * \param aad Additional authenticated data (may be `nullptr` if \p aadLen is 0).
 * \param aadLen AAD length in bytes.
 * \param pt Plaintext input.
 * \param ptLen Plaintext length in bytes.
 * \param ctOut Ciphertext output buffer of at least \p ptLen bytes.
 * \param tagOut 16-byte authentication tag output.
 * \return `true` on success, `false` on any mbedTLS error.
 */
inline bool aesGcm256Seal(const uint8_t key[32],
                          const uint8_t* iv, size_t ivLen,
                          const uint8_t* aad, size_t aadLen,
                          const uint8_t* pt, size_t ptLen,
                          uint8_t* ctOut, uint8_t tagOut[16]) {
    GcmContext gcm;
    int rc = mbedtls_gcm_setkey(gcm.get(), MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(
            gcm.get(), MBEDTLS_GCM_ENCRYPT, ptLen,
            iv, ivLen,
            aad, aadLen,
            pt, ctOut,
            16, tagOut);
    }
    return rc == 0;
}

/**
 * \brief Authenticates and decrypts \p ct with AES-256-GCM.
 * \param key 32-byte AES key.
 * \param iv Initialization vector / nonce.
 * \param ivLen IV length in bytes (12 for the canonical GCM nonce).
 * \param aad Additional authenticated data (may be `nullptr` if \p aadLen is 0).
 * \param aadLen AAD length in bytes.
 * \param ct Ciphertext input.
 * \param ctLen Ciphertext length in bytes.
 * \param tag 16-byte authentication tag.
 * \param ptOut Plaintext output buffer of at least \p ctLen bytes.
 * \return `true` on success, `false` on tag mismatch or any mbedTLS error.
 */
inline bool aesGcm256Open(const uint8_t key[32],
                          const uint8_t* iv, size_t ivLen,
                          const uint8_t* aad, size_t aadLen,
                          const uint8_t* ct, size_t ctLen,
                          const uint8_t tag[16], uint8_t* ptOut) {
    GcmContext gcm;
    int rc = mbedtls_gcm_setkey(gcm.get(), MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(
            gcm.get(), ctLen,
            iv, ivLen,
            aad, aadLen,
            tag, 16,
            ct, ptOut);
    }
    return rc == 0;
}

} // namespace cdc::core
