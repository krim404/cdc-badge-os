#include "fingerprint.h"
#include "mod_gpg/openpgp/constants.h"
#include "mod_gpg/gpg.h"

#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

#include <cstring>

namespace cdc::mod_gpg {

namespace {

static const uint8_t kOidEd25519[] = {0x09, 0x2B, 0x06, 0x01, 0x04, 0x01,
                                      0xDA, 0x47, 0x0F, 0x01};
static const uint8_t kOidP256[]    = {0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D,
                                      0x03, 0x01, 0x07};

/// Serialise the body of an OpenPGP Public Key Packet (Tag 6).
/// \return body length, or 0 on failure.
size_t buildPublicKeyBody(uint8_t curve,
                          const uint8_t* pubkey, size_t pubkey_len,
                          uint32_t created_at,
                          uint8_t* out, size_t out_size)
{
    if (!out || !pubkey) return 0;

    const bool is_ed25519 = (curve == CDC_CURVE_ED25519);

    if (is_ed25519 && pubkey_len < ED25519_PUBKEY_SIZE) return 0;
    if (!is_ed25519 && pubkey_len < 64) return 0;

    const uint8_t  algo    = is_ed25519 ? OPENPGP_ALGO_EDDSA : OPENPGP_ALGO_ECDSA;
    const uint8_t* oid     = is_ed25519 ? kOidEd25519 : kOidP256;
    const size_t   oid_len = is_ed25519 ? sizeof(kOidEd25519) : sizeof(kOidP256);

    uint8_t mpi[MPI_FULL_SIZE_P256];
    size_t  mpi_len;
    if (is_ed25519) {
        // EdDSA point in OpenPGP native format: 0x40 prefix + 32-byte point,
        // encoded as a 263-bit MPI (RFC 9580 / 4880-bis).
        uint16_t bits = 263;
        mpi[0] = static_cast<uint8_t>((bits >> 8) & 0xFF);
        mpi[1] = static_cast<uint8_t>(bits & 0xFF);
        mpi[MPI_HEADER_SIZE] = 0x40;
        std::memcpy(mpi + MPI_HEADER_SIZE + 1, pubkey, ED25519_PUBKEY_SIZE);
        mpi_len = MPI_HEADER_SIZE + 1 + ED25519_PUBKEY_SIZE;
    } else {
        // P-256 MPI: bit-length || 0x04 || X || Y.
        uint16_t bits = P256_PUBKEY_BITS;
        mpi[0] = static_cast<uint8_t>((bits >> 8) & 0xFF);
        mpi[1] = static_cast<uint8_t>(bits & 0xFF);
        mpi[MPI_HEADER_SIZE] = 0x04;
        std::memcpy(mpi + MPI_HEADER_SIZE + 1, pubkey, 64);
        mpi_len = MPI_FULL_SIZE_P256;
    }

    const size_t total = 1 + 4 + 1 + oid_len + mpi_len;
    if (total > out_size) return 0;

    size_t off = 0;
    out[off++] = 0x04;
    out[off++] = (created_at >> 24) & 0xFF;
    out[off++] = (created_at >> 16) & 0xFF;
    out[off++] = (created_at >> 8) & 0xFF;
    out[off++] = created_at & 0xFF;
    out[off++] = algo;
    std::memcpy(out + off, oid, oid_len);
    off += oid_len;
    std::memcpy(out + off, mpi, mpi_len);
    off += mpi_len;
    return off;
}

/// SHA-1 of (0x99 || 2-byte body length || body) -> V4 fingerprint.
void v4FpFromBody(const uint8_t* body, size_t body_len, uint8_t out_fp[20])
{
    const uint8_t prefix[3] = {
        0x99,
        static_cast<uint8_t>((body_len >> 8) & 0xFF),
        static_cast<uint8_t>(body_len & 0xFF),
    };
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    mbedtls_sha1_update(&ctx, prefix, sizeof(prefix));
    mbedtls_sha1_update(&ctx, body, body_len);
    mbedtls_sha1_finish(&ctx, out_fp);
    mbedtls_sha1_free(&ctx);
}

} // namespace

size_t buildEcdhPubkeyBody(const uint8_t* pubkey, uint32_t created_at,
                           uint8_t* out, size_t out_size)
{
    if (!out || !pubkey) return 0;

    // MPI of the uncompressed point: bit-length || 0x04 || X || Y.
    uint8_t mpi[MPI_FULL_SIZE_P256];
    uint16_t bits = P256_PUBKEY_BITS;
    mpi[0] = static_cast<uint8_t>((bits >> 8) & 0xFF);
    mpi[1] = static_cast<uint8_t>(bits & 0xFF);
    mpi[MPI_HEADER_SIZE] = 0x04;
    std::memcpy(mpi + MPI_HEADER_SIZE + 1, pubkey, 64);
    const size_t mpi_len = MPI_FULL_SIZE_P256;

    // RFC 6637 KDF parameters: size(0x03) || reserved(0x01) || hash || sym.
    const uint8_t kdf[4] = {0x03, 0x01, OPENPGP_ECDH_KDF_HASH, OPENPGP_ECDH_KDF_SYM};

    const size_t total = 1 + 4 + 1 + sizeof(kOidP256) + mpi_len + sizeof(kdf);
    if (total > out_size) return 0;

    size_t off = 0;
    out[off++] = 0x04;
    out[off++] = (created_at >> 24) & 0xFF;
    out[off++] = (created_at >> 16) & 0xFF;
    out[off++] = (created_at >> 8) & 0xFF;
    out[off++] = created_at & 0xFF;
    out[off++] = OPENPGP_ALGO_ECDH;
    std::memcpy(out + off, kOidP256, sizeof(kOidP256));
    off += sizeof(kOidP256);
    std::memcpy(out + off, mpi, mpi_len);
    off += mpi_len;
    std::memcpy(out + off, kdf, sizeof(kdf));
    off += sizeof(kdf);
    return off;
}

bool calculateFingerprintV4Ecdh(const uint8_t* pubkey, uint32_t created_at,
                                uint8_t out_fp[20])
{
    if (!out_fp || !pubkey) return false;
    uint8_t body[128];
    const size_t body_len = buildEcdhPubkeyBody(pubkey, created_at, body, sizeof(body));
    if (body_len == 0) return false;
    v4FpFromBody(body, body_len, out_fp);
    return true;
}

bool calculateFingerprintV4(uint8_t curve,
                            const uint8_t* pubkey, size_t pubkey_len,
                            uint32_t created_at,
                            uint8_t out_fp[20])
{
    if (!out_fp) return false;

    uint8_t body[128];
    const size_t body_len = buildPublicKeyBody(curve, pubkey, pubkey_len,
                                               created_at, body, sizeof(body));
    if (body_len == 0) return false;

    v4FpFromBody(body, body_len, out_fp);
    return true;
}

bool calculateFingerprintV5(uint8_t curve,
                            const uint8_t* pubkey, size_t pubkey_len,
                            uint32_t created_at,
                            uint8_t out_fp[32])
{
    if (!out_fp) return false;

    uint8_t body[128];
    const size_t body_len = buildPublicKeyBody(curve, pubkey, pubkey_len,
                                               created_at, body, sizeof(body));
    if (body_len == 0) return false;

    // V5 indicator 0x9A + 4-byte big-endian length, then body, then SHA-256.
    const uint8_t prefix[5] = {
        0x9A,
        static_cast<uint8_t>((body_len >> 24) & 0xFF),
        static_cast<uint8_t>((body_len >> 16) & 0xFF),
        static_cast<uint8_t>((body_len >> 8)  & 0xFF),
        static_cast<uint8_t>(body_len & 0xFF),
    };

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, prefix, sizeof(prefix));
    mbedtls_sha256_update(&ctx, body, body_len);
    mbedtls_sha256_finish(&ctx, out_fp);
    mbedtls_sha256_free(&ctx);
    return true;
}

bool gpgCrossSignDigest(const uint8_t fp_v4[20],
                        const char* user_id,
                        uint8_t out_hash[32])
{
    if (!fp_v4 || !user_id || !out_hash) return false;

    uint8_t input[84] = {0};
    std::memcpy(input, fp_v4, 20);
    const size_t uid_len = std::strlen(user_id);
    std::memcpy(input + 20, user_id, uid_len > 64 ? 64 : uid_len);

    mbedtls_sha256(input, sizeof(input), out_hash, 0);
    return true;
}

} // namespace cdc::mod_gpg
