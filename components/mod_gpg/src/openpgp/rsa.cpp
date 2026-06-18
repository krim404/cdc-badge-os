/**
 * \brief Software RSA backend for the OpenPGP card (see rsa.h).
 */

#include "rsa.h"
#include "cdc_log.h"

#include <mbedtls/rsa.h>
#include <mbedtls/bignum.h>
#include <mbedtls/platform_util.h>
#include <esp_random.h>
#include <esp_attr.h>
#include <string.h>

static const char* TAG = "GPGRsa";

/**
 * \brief mbedTLS RNG callback backed by the ESP hardware RNG.
 */
static int rsa_rng(void* ctx, unsigned char* out, size_t len) {
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

/**
 * \brief Reads a big-endian u16 from a buffer and advances the cursor.
 */
static bool rd_u16(const uint8_t* b, size_t len, size_t* pos, uint16_t* out) {
    if (*pos + 2 > len) return false;
    *out = static_cast<uint16_t>((b[*pos] << 8) | b[*pos + 1]);
    *pos += 2;
    return true;
}

/**
 * \brief Parses a private-key blob and imports it into an RSA context.
 *        On success the caller owns \p rsa and must free it.
 */
static bool rsa_load_ctx(const uint8_t* blob, size_t blob_len, mbedtls_rsa_context* rsa) {
    if (!blob) return false;
    size_t pos = 0;
    uint16_t n_bits = 0, e_len = 0, p_len = 0, q_len = 0;
    if (!rd_u16(blob, blob_len, &pos, &n_bits)) return false;
    if (!rd_u16(blob, blob_len, &pos, &e_len)) return false;
    if (e_len == 0 || pos + e_len > blob_len) return false;
    const uint8_t* e = blob + pos; pos += e_len;
    if (!rd_u16(blob, blob_len, &pos, &p_len)) return false;
    if (p_len == 0 || pos + p_len > blob_len) return false;
    const uint8_t* p = blob + pos; pos += p_len;
    if (!rd_u16(blob, blob_len, &pos, &q_len)) return false;
    if (q_len == 0 || pos + q_len > blob_len) return false;
    const uint8_t* q = blob + pos;

    mbedtls_rsa_init(rsa);
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
    int rc = mbedtls_rsa_import_raw(rsa, nullptr, 0, p, p_len, q, q_len, nullptr, 0, e, e_len);
    if (rc == 0) rc = mbedtls_rsa_complete(rsa);
    if (rc == 0) rc = mbedtls_rsa_check_privkey(rsa);
    if (rc != 0) {
        mbedtls_rsa_free(rsa);
        return false;
    }
    return true;
}

/**
 * \brief Canonically serialises an RSA context (primes + exponent) into a blob.
 */
static bool rsa_serialize(mbedtls_rsa_context* rsa, uint8_t* out, size_t cap, size_t* out_len) {
    mbedtls_mpi P, Q, E;
    mbedtls_mpi_init(&P);
    mbedtls_mpi_init(&Q);
    mbedtls_mpi_init(&E);
    bool ok = false;
    do {
        if (mbedtls_rsa_export(rsa, nullptr, &P, &Q, nullptr, &E) != 0) break;
        const size_t n_len = mbedtls_rsa_get_len(rsa);
        const uint16_t n_bits = static_cast<uint16_t>(n_len * 8);
        const size_t half = n_len / 2;
        size_t e_len = mbedtls_mpi_size(&E);
        if (e_len == 0) e_len = 1;
        const size_t total = 2 + 2 + e_len + 2 + half + 2 + half;
        if (total > cap) break;

        size_t pos = 0;
        out[pos++] = static_cast<uint8_t>((n_bits >> 8) & 0xFF);
        out[pos++] = static_cast<uint8_t>(n_bits & 0xFF);
        out[pos++] = static_cast<uint8_t>((e_len >> 8) & 0xFF);
        out[pos++] = static_cast<uint8_t>(e_len & 0xFF);
        if (mbedtls_mpi_write_binary(&E, out + pos, e_len) != 0) break;
        pos += e_len;
        out[pos++] = static_cast<uint8_t>((half >> 8) & 0xFF);
        out[pos++] = static_cast<uint8_t>(half & 0xFF);
        if (mbedtls_mpi_write_binary(&P, out + pos, half) != 0) break;
        pos += half;
        out[pos++] = static_cast<uint8_t>((half >> 8) & 0xFF);
        out[pos++] = static_cast<uint8_t>(half & 0xFF);
        if (mbedtls_mpi_write_binary(&Q, out + pos, half) != 0) break;
        pos += half;
        *out_len = pos;
        ok = true;
    } while (0);
    mbedtls_mpi_free(&P);
    mbedtls_mpi_free(&Q);
    mbedtls_mpi_free(&E);
    return ok;
}

bool gpg_rsa_blob_build(uint16_t n_bits, const uint8_t* e, size_t e_len,
                        const uint8_t* p, size_t p_len, const uint8_t* q, size_t q_len,
                        uint8_t* blob_out, size_t blob_cap, size_t* blob_len_out) {
    if (!e || !p || !q || !blob_out || !blob_len_out) return false;
    if (e_len == 0 || p_len == 0 || q_len == 0) return false;

    mbedtls_rsa_context rsa;
    mbedtls_rsa_init(&rsa);
    mbedtls_rsa_set_padding(&rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
    bool ok = false;
    int rc = mbedtls_rsa_import_raw(&rsa, nullptr, 0, p, p_len, q, q_len, nullptr, 0, e, e_len);
    if (rc == 0) rc = mbedtls_rsa_complete(&rsa);
    if (rc == 0) rc = mbedtls_rsa_check_privkey(&rsa);
    if (rc == 0) {
        const size_t bits = mbedtls_rsa_get_len(&rsa) * 8;
        if ((bits == 2048 || bits == 3072 || bits == 4096) &&
            (n_bits == 0 || n_bits == bits)) {
            ok = rsa_serialize(&rsa, blob_out, blob_cap, blob_len_out);
        } else {
            LOG_W(TAG, "RSA import rejected: %zu bits (declared %u)", bits, n_bits);
        }
    } else {
        LOG_W(TAG, "RSA import invalid key material (rc=-0x%04x)", -rc);
    }
    mbedtls_rsa_free(&rsa);
    return ok;
}

bool gpg_rsa_generate(uint16_t n_bits, uint8_t* blob_out, size_t blob_cap, size_t* blob_len_out) {
    if (n_bits != 2048 && n_bits != 3072 && n_bits != 4096) return false;
    if (!blob_out || !blob_len_out) return false;

    mbedtls_rsa_context rsa;
    mbedtls_rsa_init(&rsa);
    mbedtls_rsa_set_padding(&rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
    bool ok = false;
    int rc = mbedtls_rsa_gen_key(&rsa, rsa_rng, nullptr, n_bits, 65537);
    if (rc == 0) {
        ok = rsa_serialize(&rsa, blob_out, blob_cap, blob_len_out);
    } else {
        LOG_E(TAG, "RSA-%u keygen failed (rc=-0x%04x)", n_bits, -rc);
    }
    mbedtls_rsa_free(&rsa);
    return ok;
}

bool gpg_rsa_blob_public(const uint8_t* blob, size_t blob_len,
                         uint8_t* n_out, size_t n_cap, size_t* n_len_out,
                         uint8_t* e_out, size_t e_cap, size_t* e_len_out) {
    if (!n_out || !e_out || !n_len_out || !e_len_out) return false;
    mbedtls_rsa_context rsa;
    if (!rsa_load_ctx(blob, blob_len, &rsa)) return false;

    bool ok = false;
    mbedtls_mpi N, E;
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&E);
    do {
        if (mbedtls_rsa_export(&rsa, &N, nullptr, nullptr, nullptr, &E) != 0) break;
        const size_t n_len = mbedtls_rsa_get_len(&rsa);
        size_t e_len = mbedtls_mpi_size(&E);
        if (e_len == 0) e_len = 1;
        if (n_len > n_cap || e_len > e_cap) break;
        if (mbedtls_mpi_write_binary(&N, n_out, n_len) != 0) break;
        if (mbedtls_mpi_write_binary(&E, e_out, e_len) != 0) break;
        *n_len_out = n_len;
        *e_len_out = e_len;
        ok = true;
    } while (0);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&E);
    mbedtls_rsa_free(&rsa);
    return ok;
}

bool gpg_rsa_sign(const uint8_t* blob, size_t blob_len,
                  const uint8_t* digestinfo, size_t di_len,
                  uint8_t* sig_out, size_t sig_cap, size_t* sig_len_out) {
    if (!digestinfo || di_len == 0 || !sig_out || !sig_len_out) return false;
    mbedtls_rsa_context rsa;
    if (!rsa_load_ctx(blob, blob_len, &rsa)) return false;

    bool ok = false;
    const size_t klen = mbedtls_rsa_get_len(&rsa);
    if (klen <= sig_cap) {
        int rc = mbedtls_rsa_pkcs1_sign(&rsa, rsa_rng, nullptr, MBEDTLS_MD_NONE,
                                        static_cast<unsigned int>(di_len), digestinfo, sig_out);
        if (rc == 0) {
            *sig_len_out = klen;
            ok = true;
        } else {
            LOG_W(TAG, "RSA sign failed (rc=-0x%04x)", -rc);
        }
    }
    mbedtls_rsa_free(&rsa);
    return ok;
}

bool gpg_rsa_selftest(uint16_t n_bits) {
    static EXT_RAM_BSS_ATTR uint8_t blob[GPG_RSA_MAX_MODULUS_BYTES + 64];
    size_t blob_len = 0;
    if (!gpg_rsa_generate(n_bits, blob, sizeof(blob), &blob_len)) {
        LOG_E(TAG, "selftest: keygen failed");
        return false;
    }

    mbedtls_rsa_context rsa;
    if (!rsa_load_ctx(blob, blob_len, &rsa)) {
        mbedtls_platform_zeroize(blob, sizeof(blob));
        LOG_E(TAG, "selftest: blob reload failed");
        return false;
    }

    bool ok = false;
    const size_t klen = mbedtls_rsa_get_len(&rsa);
    static EXT_RAM_BSS_ATTR uint8_t sig[GPG_RSA_MAX_MODULUS_BYTES];
    static EXT_RAM_BSS_ATTR uint8_t ct[GPG_RSA_MAX_MODULUS_BYTES];
    static EXT_RAM_BSS_ATTR uint8_t pt[GPG_RSA_MAX_MODULUS_BYTES];
    uint8_t di[32];
    memset(di, 0xAB, sizeof(di));
    uint8_t msg[16];
    memset(msg, 0x5A, sizeof(msg));
    do {
        size_t sig_len = 0;
        if (!gpg_rsa_sign(blob, blob_len, di, sizeof(di), sig, sizeof(sig), &sig_len)) {
            LOG_E(TAG, "selftest: sign failed");
            break;
        }
        if (mbedtls_rsa_pkcs1_verify(&rsa, MBEDTLS_MD_NONE, sizeof(di), di, sig) != 0) {
            LOG_E(TAG, "selftest: verify failed");
            break;
        }
        if (mbedtls_rsa_pkcs1_encrypt(&rsa, rsa_rng, nullptr, sizeof(msg), msg, ct) != 0) {
            LOG_E(TAG, "selftest: encrypt failed");
            break;
        }
        size_t pt_len = 0;
        if (!gpg_rsa_decrypt(blob, blob_len, ct, klen, pt, sizeof(pt), &pt_len)) {
            LOG_E(TAG, "selftest: decrypt failed");
            break;
        }
        if (pt_len != sizeof(msg) || memcmp(pt, msg, sizeof(msg)) != 0) {
            LOG_E(TAG, "selftest: plaintext mismatch");
            break;
        }
        ok = true;
    } while (0);

    mbedtls_rsa_free(&rsa);
    mbedtls_platform_zeroize(blob, sizeof(blob));
    mbedtls_platform_zeroize(sig, sizeof(sig));
    mbedtls_platform_zeroize(ct, sizeof(ct));
    mbedtls_platform_zeroize(pt, sizeof(pt));
    LOG_I(TAG, "RSA-%u selftest %s", n_bits, ok ? "PASS" : "FAIL");
    return ok;
}

bool gpg_rsa_decrypt(const uint8_t* blob, size_t blob_len,
                     const uint8_t* ct, size_t ct_len,
                     uint8_t* pt_out, size_t pt_cap, size_t* pt_len_out) {
    if (!ct || !pt_out || !pt_len_out) return false;
    mbedtls_rsa_context rsa;
    if (!rsa_load_ctx(blob, blob_len, &rsa)) return false;

    bool ok = false;
    const size_t klen = mbedtls_rsa_get_len(&rsa);
    if (ct_len == klen) {
        size_t olen = 0;
        int rc = mbedtls_rsa_pkcs1_decrypt(&rsa, rsa_rng, nullptr, &olen, ct, pt_out, pt_cap);
        if (rc == 0) {
            *pt_len_out = olen;
            ok = true;
        } else {
            LOG_W(TAG, "RSA decrypt failed (rc=-0x%04x)", -rc);
        }
    }
    mbedtls_rsa_free(&rsa);
    return ok;
}
