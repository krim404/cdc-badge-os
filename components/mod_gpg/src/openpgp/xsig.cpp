#include "xsig.h"
#include "fingerprint.h"
#include "mod_gpg/gpg.h"
#include "mod_gpg/GpgStorage.h"
#include "mod_gpg/GpgSelfCertStore.h"
#include "mod_gpg/openpgp/constants.h"
#include "mod_gpg/openpgp/openpgp.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_core/Raii.h"
#include "cdc_log.h"

#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cdc::mod_gpg {

namespace {

constexpr const char* TAG = "GPG_XSIG";

static const uint8_t kOidEd25519[] = {0x09, 0x2B, 0x06, 0x01, 0x04, 0x01,
                                      0xDA, 0x47, 0x0F, 0x01};
static const uint8_t kOidP256[]    = {0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D,
                                      0x03, 0x01, 0x07};

constexpr uint8_t kHashAlgoSha256 = 0x08;
constexpr uint8_t kSigTypeGenericCert = 0x10;
constexpr uint8_t kSigTypeSubkeyBinding = 0x18;
constexpr uint8_t kSubpktTypeSigCreated = 0x02;
constexpr uint8_t kSubpktTypeIssuerKeyId = 0x10;
constexpr uint8_t kSubpktTypeKeyFlags = 0x1B;
constexpr uint8_t kKeyFlagEncrypt = 0x0C;  // encrypt communications + storage

/// \return true if the first `n` bytes of `p` are all zero.
bool isAllZero(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

/// Build the RFC 4880 Public Key Packet body for the given key.
/// \return body length, 0 on error.
size_t buildPubkeyBody(const gpg_recv_key_t& key, uint8_t* out, size_t out_size)
{
    if (!out) return 0;
    const bool is_ed25519 = (key.curve == CDC_CURVE_ED25519);
    if (is_ed25519 && key.pubkey_len < ED25519_PUBKEY_SIZE) return 0;
    if (!is_ed25519 && key.pubkey_len < 64) return 0;

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
        std::memcpy(mpi + MPI_HEADER_SIZE + 1, key.pubkey, ED25519_PUBKEY_SIZE);
        mpi_len = MPI_HEADER_SIZE + 1 + ED25519_PUBKEY_SIZE;
    } else {
        uint16_t bits = P256_PUBKEY_BITS;
        mpi[0] = static_cast<uint8_t>((bits >> 8) & 0xFF);
        mpi[1] = static_cast<uint8_t>(bits & 0xFF);
        mpi[MPI_HEADER_SIZE] = 0x04;
        std::memcpy(mpi + MPI_HEADER_SIZE + 1, key.pubkey, 64);
        mpi_len = MPI_FULL_SIZE_P256;
    }

    const size_t total = 1 + 4 + 1 + oid_len + mpi_len;
    if (total > out_size) return 0;

    size_t off = 0;
    out[off++] = 0x04;
    out[off++] = (key.created_at >> 24) & 0xFF;
    out[off++] = (key.created_at >> 16) & 0xFF;
    out[off++] = (key.created_at >> 8) & 0xFF;
    out[off++] = key.created_at & 0xFF;
    out[off++] = algo;
    std::memcpy(out + off, oid, oid_len);
    off += oid_len;
    std::memcpy(out + off, mpi, mpi_len);
    off += mpi_len;
    return off;
}

/// Write the body of the signature subpackets used for the certification.
/// \param hashed `true` for the hashed area (sig-creation time),
///               `false` for the unhashed area (issuer key id).
/// \param fp_self The signer's own V4 fingerprint (used for Issuer).
/// \param sig_creation_time Timestamp embedded into the hashed area.
size_t buildSigSubpackets(bool hashed,
                          const uint8_t fp_self[20],
                          uint32_t sig_creation_time,
                          uint8_t* out, size_t out_size)
{
    if (!out) return 0;
    if (hashed) {
        if (out_size < 6) return 0;
        // Sig creation time: length=5, type=0x02, 4-byte timestamp
        out[0] = 5;
        out[1] = kSubpktTypeSigCreated;
        out[2] = (sig_creation_time >> 24) & 0xFF;
        out[3] = (sig_creation_time >> 16) & 0xFF;
        out[4] = (sig_creation_time >> 8) & 0xFF;
        out[5] = sig_creation_time & 0xFF;
        return 6;
    }
    // Issuer key ID: length=9, type=0x10, 8 bytes = last 8 of own fingerprint
    if (out_size < 10) return 0;
    out[0] = 9;
    out[1] = kSubpktTypeIssuerKeyId;
    std::memcpy(out + 2, fp_self + 12, 8);
    return 10;
}

/// RFC 4880 new-format packet length encoding (5-byte form for simplicity).
size_t writeNewFormatLength(uint8_t* out, size_t len)
{
    out[0] = 0xFF;
    out[1] = (len >> 24) & 0xFF;
    out[2] = (len >> 16) & 0xFF;
    out[3] = (len >> 8)  & 0xFF;
    out[4] = len & 0xFF;
    return 5;
}

/// Encode one MPI: 2-byte big-endian bit count + raw bytes (leading zeros
/// stripped). For our R/S the high bit may or may not be set; we always emit
/// the conservative count for 32-byte values.
size_t writeMpi(const uint8_t* data, size_t len, uint8_t* out, size_t out_size)
{
    // Skip leading zero bytes to find the real bit length.
    size_t start = 0;
    while (start < len && data[start] == 0) ++start;
    const size_t real_len = len - start;
    if (real_len == 0) {
        if (out_size < 2) return 0;
        out[0] = 0;
        out[1] = 0;
        return 2;
    }
    if (out_size < 2 + real_len) return 0;

    uint16_t bits = static_cast<uint16_t>(real_len) * 8;
    uint8_t  high = data[start];
    for (int b = 7; b >= 0; --b) {
        if (high & (1u << b)) break;
        --bits;
    }
    out[0] = (bits >> 8) & 0xFF;
    out[1] = bits & 0xFF;
    std::memcpy(out + 2, data + start, real_len);
    return 2 + real_len;
}

/// CRC-24 per RFC 4880 section 6.1.
uint32_t crc24(const uint8_t* data, size_t len)
{
    constexpr uint32_t kCrc24Init = 0x00B704CEu;
    constexpr uint32_t kCrc24Poly = 0x01864CFBu;
    uint32_t crc = kCrc24Init;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 16;
        for (int b = 0; b < 8; ++b) {
            crc <<= 1;
            if (crc & 0x01000000u) crc ^= kCrc24Poly;
        }
    }
    return crc & 0x00FFFFFFu;
}

/// Run base64 + line wrap at 64 chars into `out`.
size_t armorBase64(const uint8_t* data, size_t len, char* out, size_t out_size)
{
    size_t enc_len = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out), out_size,
                              &enc_len, data, len) != 0) {
        return 0;
    }
    // mbedtls base64 emits a flat string; we need 64-char-wrapped lines.
    // Walk backwards to insert \r\n every 64 chars.
    char tmp[2048];
    if (enc_len + (enc_len / 64) * 2 + 4 > sizeof(tmp)) return 0;

    size_t out_idx = 0;
    for (size_t i = 0; i < enc_len; i += 64) {
        size_t chunk = std::min<size_t>(64, enc_len - i);
        std::memcpy(tmp + out_idx, out + i, chunk);
        out_idx += chunk;
        tmp[out_idx++] = '\r';
        tmp[out_idx++] = '\n';
    }
    if (out_idx > out_size) return 0;
    std::memcpy(out, tmp, out_idx);
    return out_idx;
}

/// Wrap assembled binary OpenPGP packets into an ASCII-armored
/// `BEGIN/END PGP PUBLIC KEY BLOCK` with the trailing CRC-24 line.
/// \return `true` on success, `false` on buffer overflow.
bool armorBinaryBlock(const uint8_t* binary, size_t binary_off,
                      char* out, size_t out_size, size_t* out_len)
{
    static constexpr const char* kBegin = "-----BEGIN PGP PUBLIC KEY BLOCK-----\r\n\r\n";
    static constexpr const char* kEnd   = "-----END PGP PUBLIC KEY BLOCK-----\r\n";
    const size_t begin_len = std::strlen(kBegin);
    const size_t end_len   = std::strlen(kEnd);

    if (begin_len + end_len + (binary_off * 2) + 16 > out_size) return false;

    size_t off = 0;
    std::memcpy(out + off, kBegin, begin_len);
    off += begin_len;

    size_t body_written = armorBase64(binary, binary_off, out + off, out_size - off);
    if (body_written == 0) return false;
    off += body_written;

    // CRC24 line: "=" + 4 base64 chars + CRLF.
    uint8_t crc_bytes[3];
    uint32_t crc = crc24(binary, binary_off);
    crc_bytes[0] = (crc >> 16) & 0xFF;
    crc_bytes[1] = (crc >> 8)  & 0xFF;
    crc_bytes[2] = crc & 0xFF;

    if (off + 8 > out_size) return false;
    out[off++] = '=';
    size_t crc_len = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out + off),
                              out_size - off, &crc_len,
                              crc_bytes, sizeof(crc_bytes)) != 0) {
        return false;
    }
    off += crc_len;
    out[off++] = '\r';
    out[off++] = '\n';

    if (off + end_len > out_size) return false;
    std::memcpy(out + off, kEnd, end_len);
    off += end_len;

    if (off < out_size) out[off] = '\0';
    *out_len = off;
    return true;
}

/// Build a Tag 2 UID certification packet body (sig type 0x10) for an arbitrary
/// signer. The signature R || S is supplied; the left-16 hash bits are recomputed
/// from the same inputs the signer hashed. \return body length, 0 on error.
size_t buildUidCertPacket(const uint8_t* pk_body, size_t pk_body_len,
                          const char* user_id, size_t uid_len,
                          uint8_t signer_curve, const uint8_t signer_fp[20],
                          uint32_t sig_created_at, const uint8_t sig_rs[64],
                          uint8_t* out, size_t out_size)
{
    const uint8_t sig_algo = (signer_curve == CDC_CURVE_ED25519)
                                 ? OPENPGP_ALGO_EDDSA : OPENPGP_ALGO_ECDSA;

    uint8_t hashed_subs[8];
    const size_t hashed_subs_len = buildSigSubpackets(true, nullptr, sig_created_at,
                                                      hashed_subs, sizeof(hashed_subs));
    uint8_t unhashed_subs[10];
    const size_t unhashed_subs_len = buildSigSubpackets(false, signer_fp, 0,
                                                        unhashed_subs, sizeof(unhashed_subs));

    if (out_size < 6 + hashed_subs_len + 2 + unhashed_subs_len + 2) return 0;

    size_t off = 0;
    out[off++] = 0x04;
    out[off++] = kSigTypeGenericCert;
    out[off++] = sig_algo;
    out[off++] = kHashAlgoSha256;
    out[off++] = (hashed_subs_len >> 8) & 0xFF;
    out[off++] = hashed_subs_len & 0xFF;
    std::memcpy(out + off, hashed_subs, hashed_subs_len);
    off += hashed_subs_len;
    out[off++] = (unhashed_subs_len >> 8) & 0xFF;
    out[off++] = unhashed_subs_len & 0xFF;
    std::memcpy(out + off, unhashed_subs, unhashed_subs_len);
    off += unhashed_subs_len;

    // Left 16 bits of the signed hash: recompute the hash to obtain them.
    {
        const uint8_t pk_prefix[3] = {
            0x99,
            static_cast<uint8_t>((pk_body_len >> 8) & 0xFF),
            static_cast<uint8_t>(pk_body_len & 0xFF),
        };
        const uint8_t uid_prefix[5] = {
            0xB4,
            static_cast<uint8_t>((uid_len >> 24) & 0xFF),
            static_cast<uint8_t>((uid_len >> 16) & 0xFF),
            static_cast<uint8_t>((uid_len >> 8)  & 0xFF),
            static_cast<uint8_t>(uid_len & 0xFF),
        };
        uint8_t sig_data_header[6] = {
            0x04, kSigTypeGenericCert, sig_algo, kHashAlgoSha256,
            static_cast<uint8_t>((hashed_subs_len >> 8) & 0xFF),
            static_cast<uint8_t>(hashed_subs_len & 0xFF),
        };
        const size_t sig_data_total = sizeof(sig_data_header) + hashed_subs_len;
        const uint8_t trailer[6] = {
            0x04, 0xFF,
            static_cast<uint8_t>((sig_data_total >> 24) & 0xFF),
            static_cast<uint8_t>((sig_data_total >> 16) & 0xFF),
            static_cast<uint8_t>((sig_data_total >> 8)  & 0xFF),
            static_cast<uint8_t>(sig_data_total & 0xFF),
        };

        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, pk_prefix, sizeof(pk_prefix));
        mbedtls_sha256_update(&ctx, pk_body, pk_body_len);
        mbedtls_sha256_update(&ctx, uid_prefix, sizeof(uid_prefix));
        mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(user_id), uid_len);
        mbedtls_sha256_update(&ctx, sig_data_header, sizeof(sig_data_header));
        mbedtls_sha256_update(&ctx, hashed_subs, hashed_subs_len);
        mbedtls_sha256_update(&ctx, trailer, sizeof(trailer));
        uint8_t hash[32];
        mbedtls_sha256_finish(&ctx, hash);
        mbedtls_sha256_free(&ctx);

        if (off + 2 > out_size) return 0;
        out[off++] = hash[0];
        out[off++] = hash[1];
    }

    size_t mpi_off = writeMpi(sig_rs, 32, out + off, out_size - off);
    if (mpi_off == 0) return 0;
    off += mpi_off;
    mpi_off = writeMpi(sig_rs + 32, 32, out + off, out_size - off);
    if (mpi_off == 0) return 0;
    off += mpi_off;

    return off;
}

/// Hashed subpackets for a subkey-binding signature: sig-creation-time + key
/// flags (encryption). \return total length (9 bytes).
size_t buildBindingHashedSubpackets(uint32_t sig_created_at, uint8_t* out)
{
    size_t off = 0;
    out[off++] = 5;
    out[off++] = kSubpktTypeSigCreated;
    out[off++] = (sig_created_at >> 24) & 0xFF;
    out[off++] = (sig_created_at >> 16) & 0xFF;
    out[off++] = (sig_created_at >> 8) & 0xFF;
    out[off++] = sig_created_at & 0xFF;
    out[off++] = 2;
    out[off++] = kSubpktTypeKeyFlags;
    out[off++] = kKeyFlagEncrypt;
    return off;
}

/// SHA-256 over the subkey-binding signature input (RFC 4880 5.2.4 type 0x18):
/// 0x99||primary || 0x99||subkey || sig-data-header || hashed-subs || trailer.
void hashSubkeyBinding(const uint8_t* primary_body, size_t primary_len,
                       const uint8_t* subkey_body, size_t subkey_len,
                       uint8_t sig_algo,
                       const uint8_t* hashed_subs, size_t hashed_subs_len,
                       uint8_t out_hash[32])
{
    const uint8_t sig_data_header[6] = {
        0x04, kSigTypeSubkeyBinding, sig_algo, kHashAlgoSha256,
        static_cast<uint8_t>((hashed_subs_len >> 8) & 0xFF),
        static_cast<uint8_t>(hashed_subs_len & 0xFF),
    };
    const size_t sig_data_total = sizeof(sig_data_header) + hashed_subs_len;
    const uint8_t trailer[6] = {
        0x04, 0xFF,
        static_cast<uint8_t>((sig_data_total >> 24) & 0xFF),
        static_cast<uint8_t>((sig_data_total >> 16) & 0xFF),
        static_cast<uint8_t>((sig_data_total >> 8)  & 0xFF),
        static_cast<uint8_t>(sig_data_total & 0xFF),
    };
    const uint8_t primary_prefix[3] = {
        0x99,
        static_cast<uint8_t>((primary_len >> 8) & 0xFF),
        static_cast<uint8_t>(primary_len & 0xFF),
    };
    const uint8_t subkey_prefix[3] = {
        0x99,
        static_cast<uint8_t>((subkey_len >> 8) & 0xFF),
        static_cast<uint8_t>(subkey_len & 0xFF),
    };

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, primary_prefix, sizeof(primary_prefix));
    mbedtls_sha256_update(&ctx, primary_body, primary_len);
    mbedtls_sha256_update(&ctx, subkey_prefix, sizeof(subkey_prefix));
    mbedtls_sha256_update(&ctx, subkey_body, subkey_len);
    mbedtls_sha256_update(&ctx, sig_data_header, sizeof(sig_data_header));
    mbedtls_sha256_update(&ctx, hashed_subs, hashed_subs_len);
    mbedtls_sha256_update(&ctx, trailer, sizeof(trailer));
    mbedtls_sha256_finish(&ctx, out_hash);
    mbedtls_sha256_free(&ctx);
}

/// Sign a subkey-binding signature (type 0x18) over primary||subkey with the
/// badge's own SIG slot. \return `true` on success.
bool signSubkeyBinding(const uint8_t* primary_body, size_t primary_len,
                       const uint8_t* subkey_body, size_t subkey_len,
                       uint32_t sig_created_at, uint8_t out_sig[64])
{
    gpg_status_t self_status = {};
    if (!gpg_get_status(&self_status)) return false;
    const uint8_t sig_algo = (self_status.curve == CDC_CURVE_ED25519)
                                 ? OPENPGP_ALGO_EDDSA : OPENPGP_ALGO_ECDSA;

    uint8_t hashed_subs[16];
    const size_t hashed_subs_len = buildBindingHashedSubpackets(sig_created_at, hashed_subs);

    uint8_t hash[32];
    hashSubkeyBinding(primary_body, primary_len, subkey_body, subkey_len,
                      sig_algo, hashed_subs, hashed_subs_len, hash);

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;
    const uint8_t slot = gpg_storage_sig_slot();
    size_t sig_len = 64;
    if (sig_algo == OPENPGP_ALGO_EDDSA) {
        return se->eddsaSign(slot, hash, sizeof(hash), out_sig) == cdc::hal::SeResult::OK;
    }
    return se->ecdsaSign(slot, hash, sizeof(hash), out_sig, &sig_len)
           == cdc::hal::SeResult::OK;
}

/// Build a Tag 2 subkey-binding signature packet body (sig type 0x18) for an
/// arbitrary signer. \return body length, 0 on error.
size_t buildSubkeyBindingSigPacket(const uint8_t* primary_body, size_t primary_len,
                                   const uint8_t* subkey_body, size_t subkey_len,
                                   uint8_t signer_curve, const uint8_t signer_fp[20],
                                   uint32_t sig_created_at, const uint8_t sig_rs[64],
                                   uint8_t* out, size_t out_size)
{
    const uint8_t sig_algo = (signer_curve == CDC_CURVE_ED25519)
                                 ? OPENPGP_ALGO_EDDSA : OPENPGP_ALGO_ECDSA;

    uint8_t hashed_subs[16];
    const size_t hashed_subs_len = buildBindingHashedSubpackets(sig_created_at, hashed_subs);
    uint8_t unhashed_subs[10];
    const size_t unhashed_subs_len = buildSigSubpackets(false, signer_fp, 0,
                                                        unhashed_subs, sizeof(unhashed_subs));

    if (out_size < 6 + hashed_subs_len + 2 + unhashed_subs_len + 2) return 0;

    size_t off = 0;
    out[off++] = 0x04;
    out[off++] = kSigTypeSubkeyBinding;
    out[off++] = sig_algo;
    out[off++] = kHashAlgoSha256;
    out[off++] = (hashed_subs_len >> 8) & 0xFF;
    out[off++] = hashed_subs_len & 0xFF;
    std::memcpy(out + off, hashed_subs, hashed_subs_len);
    off += hashed_subs_len;
    out[off++] = (unhashed_subs_len >> 8) & 0xFF;
    out[off++] = unhashed_subs_len & 0xFF;
    std::memcpy(out + off, unhashed_subs, unhashed_subs_len);
    off += unhashed_subs_len;

    uint8_t hash[32];
    hashSubkeyBinding(primary_body, primary_len, subkey_body, subkey_len,
                      sig_algo, hashed_subs, hashed_subs_len, hash);
    if (off + 2 > out_size) return 0;
    out[off++] = hash[0];
    out[off++] = hash[1];

    size_t mpi_off = writeMpi(sig_rs, 32, out + off, out_size - off);
    if (mpi_off == 0) return 0;
    off += mpi_off;
    mpi_off = writeMpi(sig_rs + 32, 32, out + off, out_size - off);
    if (mpi_off == 0) return 0;
    off += mpi_off;

    return off;
}

/// Populate `self` with the badge's own SIG public key from the secure element.
/// \return `true` if a key is configured and readable.
bool loadOwnSigKey(gpg_recv_key_t& self)
{
    gpg_status_t status = {};
    if (!gpg_get_status(&status)) return false;

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;

    uint8_t pubkey[64 + 1] = {0};
    cdc::hal::EccCurve hal_curve = cdc::hal::EccCurve::P256;
    if (se->eccGetPublicKey(gpg_storage_sig_slot(), pubkey, &hal_curve)
        != cdc::hal::SeResult::OK) {
        return false;
    }
    const uint8_t curve = (hal_curve == cdc::hal::EccCurve::ED25519) ? CDC_CURVE_ED25519
                                                                     : CDC_CURVE_P256;
    if (curve == CDC_CURVE_P256 && pubkey[0] == 0x04) {
        std::memmove(pubkey, pubkey + 1, 64);
    }

    self = gpg_recv_key_t{};
    self.curve = curve;
    self.pubkey_len = (curve == CDC_CURVE_ED25519) ? 32 : 64;
    std::memcpy(self.pubkey, pubkey, self.pubkey_len);
    self.created_at = status.created_at;
    std::memcpy(self.fingerprint_v4, status.fingerprint, 20);
    std::strncpy(self.user_id, status.user_id, sizeof(self.user_id) - 1);
    return true;
}

/// Assemble an armored block for a received key: primary + UID + owner's UID
/// self-signature + optionally our cross-certification + the DEC encryption
/// subkey with the owner's binding signature. The owner's signatures are
/// supplied in `key` (the owner computed them; we cannot forge them).
bool buildReceivedKeyArmored(const gpg_recv_key_t& key, bool include_our_cert,
                             char* out, size_t out_size, size_t* out_len)
{
    if (!out || !out_len || out_size < 256) return false;

    uint8_t pk_body[128];
    const size_t pk_body_len = buildPubkeyBody(key, pk_body, sizeof(pk_body));
    if (pk_body_len == 0) return false;
    const size_t uid_len = strnlen(key.user_id, sizeof(key.user_id));

    uint8_t binary[1024];
    size_t binary_off = 0;
    auto emit = [&](uint8_t tag, const uint8_t* body, size_t len) -> bool {
        if (binary_off + 1 + 5 + len > sizeof(binary)) return false;
        binary[binary_off++] = tag;
        binary_off += writeNewFormatLength(binary + binary_off, len);
        std::memcpy(binary + binary_off, body, len);
        binary_off += len;
        return true;
    };

    if (!emit(0xC6, pk_body, pk_body_len)) return false;
    if (!emit(0xCD, reinterpret_cast<const uint8_t*>(key.user_id), uid_len)) return false;

    // Owner's UID self-signature (makes the UID valid and the key usable).
    if (!isAllZero(key.owner_self_sig, 64)) {
        uint8_t sig[256];
        const size_t n = buildUidCertPacket(pk_body, pk_body_len, key.user_id, uid_len,
                                             key.curve, key.fingerprint_v4,
                                             key.created_at, key.owner_self_sig,
                                             sig, sizeof(sig));
        if (n > 0) emit(0xC2, sig, n);
    }

    // Our own cross-certification on the UID (web of trust).
    if (include_our_cert && key.sig_len == 64) {
        uint8_t sig[256];
        const size_t n = buildCertSigPacket(key, sig, sizeof(sig));
        if (n > 0) emit(0xC2, sig, n);
    }

    // DEC encryption subkey + owner's binding signature.
    if (!isAllZero(key.pubkey_dec, 64) && !isAllZero(key.dec_binding_sig, 64)) {
        uint8_t dec_body[128];
        const size_t dec_len = buildEcdhPubkeyBody(key.pubkey_dec, key.created_at_dec,
                                                   dec_body, sizeof(dec_body));
        uint8_t bind[256];
        size_t bind_len = 0;
        if (dec_len > 0) {
            bind_len = buildSubkeyBindingSigPacket(pk_body, pk_body_len, dec_body, dec_len,
                                                   key.curve, key.fingerprint_v4,
                                                   key.created_at_dec, key.dec_binding_sig,
                                                   bind, sizeof(bind));
        }
        if (bind_len > 0) {
            emit(0xCE, dec_body, dec_len);
            emit(0xC2, bind, bind_len);
        }
    }

    return armorBinaryBlock(binary, binary_off, out, out_size, out_len);
}

} // namespace

bool gpgCrossSign(const gpg_recv_key_t& target,
                  uint32_t sig_creation_time,
                  uint8_t out_sig[64])
{
    if (!out_sig) return false;

    gpg_status_t self_status = {};
    if (!gpg_get_status(&self_status)) {
        LOG_W(TAG, "gpgCrossSign: own GPG key not available");
        return false;
    }

    uint8_t pk_body[128];
    const size_t pk_body_len = buildPubkeyBody(target, pk_body, sizeof(pk_body));
    if (pk_body_len == 0) return false;

    const size_t uid_len = strnlen(target.user_id, sizeof(target.user_id));

    // Hashed signature header bytes (the same bytes that are written into
    // the final packet's hashed area, and also fed into the hash).
    uint8_t hashed_subs[8];
    const size_t hashed_subs_len = buildSigSubpackets(
        true, /*fp_self*/ nullptr, sig_creation_time, hashed_subs, sizeof(hashed_subs));

    const uint8_t sig_algo = (self_status.curve == CDC_CURVE_ED25519)
                                 ? OPENPGP_ALGO_EDDSA : OPENPGP_ALGO_ECDSA;

    uint8_t sig_data_header[6];
    sig_data_header[0] = 0x04;
    sig_data_header[1] = kSigTypeGenericCert;
    sig_data_header[2] = sig_algo;
    sig_data_header[3] = kHashAlgoSha256;
    sig_data_header[4] = (hashed_subs_len >> 8) & 0xFF;
    sig_data_header[5] = hashed_subs_len & 0xFF;

    const size_t sig_data_total = sizeof(sig_data_header) + hashed_subs_len;
    const uint8_t trailer[6] = {
        0x04, 0xFF,
        static_cast<uint8_t>((sig_data_total >> 24) & 0xFF),
        static_cast<uint8_t>((sig_data_total >> 16) & 0xFF),
        static_cast<uint8_t>((sig_data_total >> 8)  & 0xFF),
        static_cast<uint8_t>(sig_data_total & 0xFF),
    };

    // SHA-256 over: 0x99 || pk_body_len(2B) || pk_body
    //            || 0xB4 || uid_len(4B)    || uid_body
    //            || sig_data_header || hashed_subs
    //            || trailer
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    const uint8_t pk_prefix[3] = {
        0x99,
        static_cast<uint8_t>((pk_body_len >> 8) & 0xFF),
        static_cast<uint8_t>(pk_body_len & 0xFF),
    };
    mbedtls_sha256_update(&ctx, pk_prefix, sizeof(pk_prefix));
    mbedtls_sha256_update(&ctx, pk_body, pk_body_len);

    const uint8_t uid_prefix[5] = {
        0xB4,
        static_cast<uint8_t>((uid_len >> 24) & 0xFF),
        static_cast<uint8_t>((uid_len >> 16) & 0xFF),
        static_cast<uint8_t>((uid_len >> 8)  & 0xFF),
        static_cast<uint8_t>(uid_len & 0xFF),
    };
    mbedtls_sha256_update(&ctx, uid_prefix, sizeof(uid_prefix));
    mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(target.user_id), uid_len);

    mbedtls_sha256_update(&ctx, sig_data_header, sizeof(sig_data_header));
    mbedtls_sha256_update(&ctx, hashed_subs, hashed_subs_len);
    mbedtls_sha256_update(&ctx, trailer, sizeof(trailer));

    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;
    const uint8_t slot = gpg_storage_sig_slot();
    size_t sig_len = 64;

    if (sig_algo == OPENPGP_ALGO_EDDSA) {
        return se->eddsaSign(slot, hash, sizeof(hash), out_sig) == cdc::hal::SeResult::OK;
    }
    return se->ecdsaSign(slot, hash, sizeof(hash), out_sig, &sig_len)
           == cdc::hal::SeResult::OK;
}

size_t buildCertSigPacket(const gpg_recv_key_t& key, uint8_t* out, size_t out_size)
{
    if (!out || key.sig_len != 64) return 0;

    gpg_status_t self_status = {};
    if (!gpg_get_status(&self_status)) return 0;

    uint8_t pk_body[128];
    const size_t pk_body_len = buildPubkeyBody(key, pk_body, sizeof(pk_body));
    if (pk_body_len == 0) return 0;

    const size_t uid_len = strnlen(key.user_id, sizeof(key.user_id));

    // Our own SIG key is the signer/issuer of this third-party certification.
    return buildUidCertPacket(pk_body, pk_body_len, key.user_id, uid_len,
                              self_status.curve, self_status.fingerprint,
                              key.sig_created_at, key.my_signature, out, out_size);
}

bool gpgBuildReceivedKeyArmored(const gpg_recv_key_t& key,
                                char* out, size_t out_size,
                                size_t* out_len)
{
    return buildReceivedKeyArmored(key, /*include_our_cert=*/true, out, out_size, out_len);
}

bool gpgBuildPublicKeyArmored(const gpg_recv_key_t& key,
                              char* out, size_t out_size,
                              size_t* out_len)
{
    return buildReceivedKeyArmored(key, /*include_our_cert=*/false, out, out_size, out_len);
}

bool gpgBuildOwnSignedKeyArmored(char* out, size_t out_size, size_t* out_len)
{
    if (!out || !out_len || out_size < 256) return false;

    gpg_recv_key_t self = {};
    if (!loadOwnSigKey(self)) return false;

    uint8_t pk_body[128];
    const size_t pk_body_len = buildPubkeyBody(self, pk_body, sizeof(pk_body));
    if (pk_body_len == 0) return false;
    const size_t uid_len = strnlen(self.user_id, sizeof(self.user_id));

    constexpr size_t kBinaryCap = 3072;
    auto binary = ::cdc::core::psramAlloc<uint8_t>(kBinaryCap);
    if (!binary) return false;
    size_t binary_off = 0;
    auto emit = [&](uint8_t tag, const uint8_t* body, size_t len) -> bool {
        if (binary_off + 1 + 5 + len > kBinaryCap) return false;
        binary[binary_off++] = tag;
        binary_off += writeNewFormatLength(binary.get() + binary_off, len);
        std::memcpy(binary.get() + binary_off, body, len);
        binary_off += len;
        return true;
    };

    if (!emit(0xC6, pk_body, pk_body_len)) return false;
    if (!emit(0xCD, reinterpret_cast<const uint8_t*>(self.user_id), uid_len)) return false;

    // Self-signature (Tag 2): certify our own UID with our own SIG key. Without
    // it gpg reports the UID as unsigned and the key as unusable. The signature
    // creation time is the key creation time, for a deterministic export.
    uint8_t self_sig[64];
    if (gpgCrossSign(self, self.created_at, self_sig)) {
        self.sig_len = 64;
        self.sig_created_at = self.created_at;
        std::memcpy(self.my_signature, self_sig, sizeof(self_sig));
        uint8_t sig_body[256];
        const size_t sig_body_off = buildCertSigPacket(self, sig_body, sizeof(sig_body));
        if (sig_body_off > 0) emit(0xC2, sig_body, sig_body_off);
    }

    // One Signature Packet (Tag 2, new format) per stored third-party cert.
    auto& store = GpgSelfCertStore::instance();
    const uint8_t n = store.count();
    for (uint8_t i = 0; i < n; ++i) {
        gpg_self_cert_t cert;
        if (!store.getCert(i, &cert)) continue;
        if (cert.sig_pkt_len == 0 || cert.sig_pkt_len > kGpgSelfCertSigMax) continue;
        if (!emit(0xC2, cert.sig_pkt, cert.sig_pkt_len)) break;
    }

    // Encryption subkey (DEC, RFC 6637 ECDH P-256) + its binding signature.
    uint8_t dec65[P256_PUBKEY_SIZE] = {0};
    if (gpg_get_dec_pubkey(dec65)) {
        const uint32_t dec_time = openpgp_get_gen_time(KEY_DEC);
        uint8_t dec_body[128];
        const size_t dec_len = buildEcdhPubkeyBody(dec65 + 1, dec_time,
                                                   dec_body, sizeof(dec_body));
        uint8_t binding_sig[64];
        uint8_t bind[256];
        size_t bind_len = 0;
        if (dec_len > 0 &&
            signSubkeyBinding(pk_body, pk_body_len, dec_body, dec_len, dec_time, binding_sig)) {
            bind_len = buildSubkeyBindingSigPacket(pk_body, pk_body_len, dec_body, dec_len,
                                                   self.curve, self.fingerprint_v4,
                                                   dec_time, binding_sig, bind, sizeof(bind));
        }
        if (bind_len > 0) {
            emit(0xCE, dec_body, dec_len);
            emit(0xC2, bind, bind_len);
        }
    }

    return armorBinaryBlock(binary.get(), binary_off, out, out_size, out_len);
}

bool gpgBuildOwnSubkeyMaterial(uint8_t dec_pubkey[64], uint32_t* dec_created_at,
                               uint8_t self_sig[64], uint8_t binding_sig[64])
{
    if (!dec_pubkey || !self_sig || !binding_sig) return false;

    gpg_recv_key_t self = {};
    if (!loadOwnSigKey(self)) return false;

    // UID self-signature over our own primary key.
    if (!gpgCrossSign(self, self.created_at, self_sig)) return false;

    // DEC encryption subkey public point and creation time.
    uint8_t dec65[P256_PUBKEY_SIZE] = {0};
    if (!gpg_get_dec_pubkey(dec65)) return false;
    std::memcpy(dec_pubkey, dec65 + 1, 64);
    const uint32_t dec_time = openpgp_get_gen_time(KEY_DEC);
    if (dec_created_at) *dec_created_at = dec_time;

    uint8_t pk_body[128];
    const size_t pk_len = buildPubkeyBody(self, pk_body, sizeof(pk_body));
    uint8_t dec_body[128];
    const size_t dec_len = buildEcdhPubkeyBody(dec_pubkey, dec_time, dec_body, sizeof(dec_body));
    if (pk_len == 0 || dec_len == 0) return false;

    return signSubkeyBinding(pk_body, pk_len, dec_body, dec_len, dec_time, binding_sig);
}

} // namespace cdc::mod_gpg
