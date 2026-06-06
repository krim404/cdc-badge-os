/**
 * \file host_api_crypto.cpp
 * \brief Crypto + encoding host APIs - mbedTLS wrappers + TROPIC01 TRNG.
 */

#include "plugin_manager/host_api.h"
#include "cdc_hal/ISecureElement.h"
#include "HexUtil.h"

#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "mbedtls/gcm.h"
#include "mbedtls/base64.h"
#include "esp_random.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

namespace {

const char BASE32_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

}  // namespace

extern "C" {

int host_random(uint8_t* buf, size_t len)
{
    if (!buf) return HOST_ERR_INVALID_ARG;
    esp_fill_random(buf, len);
    return HOST_OK;
}

int host_random_strict(uint8_t* buf, size_t len)
{
    if (!buf) return HOST_ERR_INVALID_ARG;
    auto* se = cdc::hal::getSecureElementInstance();
    if (se && se->getRandomStrict(buf, len)) return HOST_OK;
    return HOST_ERR_NOT_SUPPORTED;
}

int host_sha256(const uint8_t* data, size_t len, uint8_t out[32])
{
    if (!out || (!data && len > 0)) return HOST_ERR_INVALID_ARG;
    if (mbedtls_sha256(data, len, out, 0) != 0) return HOST_ERR_GENERIC;
    return HOST_OK;
}

int host_hmac_sha256(const uint8_t* key, size_t klen,
                     const uint8_t* data, size_t dlen, uint8_t out[32])
{
    if (!out || !key || !data) return HOST_ERR_INVALID_ARG;
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return HOST_ERR_GENERIC;
    if (mbedtls_md_hmac(info, key, klen, data, dlen, out) != 0) return HOST_ERR_GENERIC;
    return HOST_OK;
}

int host_aes_gcm_encrypt(const uint8_t* key, const uint8_t* iv,
                         const uint8_t* aad, size_t aad_len,
                         const uint8_t* pt, size_t pt_len,
                         uint8_t* ct, uint8_t tag[16])
{
    if (!key || !iv || !ct || !tag) return HOST_ERR_INVALID_ARG;
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
                                                 pt_len, iv, 12, aad, aad_len,
                                                 pt, ct, 16, tag);
    mbedtls_gcm_free(&ctx);
    return rc == 0 ? HOST_OK : HOST_ERR_GENERIC;
}

int host_aes_gcm_decrypt(const uint8_t* key, const uint8_t* iv,
                         const uint8_t* aad, size_t aad_len,
                         const uint8_t* ct, size_t ct_len,
                         const uint8_t tag[16], uint8_t* pt)
{
    if (!key || !iv || !ct || !tag || !pt) return HOST_ERR_INVALID_ARG;
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&ctx, ct_len, iv, 12, aad, aad_len,
                                                tag, 16, ct, pt);
    mbedtls_gcm_free(&ctx);
    return rc == 0 ? HOST_OK : HOST_ERR_GENERIC;
}

int host_base32_encode(const uint8_t* in, size_t in_len, char* out, size_t out_size)
{
    if (!out || (!in && in_len > 0)) return HOST_ERR_INVALID_ARG;
    // RFC 4648 base32, no padding: ceil(in_len*8 / 5) symbols + NUL. Streaming
    // emit (symmetric to host_base32_decode), so a partial final group yields
    // exactly its symbols instead of a zero-padded full 8-symbol group.
    size_t needed = (in_len * 8 + 4) / 5 + 1;
    if (out_size < needed) return HOST_ERR_NO_MEMORY;
    size_t out_pos = 0;
    int bits = 0;
    uint32_t buffer = 0;
    for (size_t i = 0; i < in_len; ++i) {
        buffer = (buffer << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            out[out_pos++] = BASE32_ALPHABET[(buffer >> (bits - 5)) & 0x1F];
            bits -= 5;
        }
    }
    if (bits > 0) {
        out[out_pos++] = BASE32_ALPHABET[(buffer << (5 - bits)) & 0x1F];
    }
    out[out_pos] = '\0';
    return HOST_OK;
}

int host_base32_decode(const char* in, size_t in_len, uint8_t* out, size_t out_size)
{
    if (!in || !out) return HOST_ERR_INVALID_ARG;
    size_t out_pos = 0;
    int bits = 0;
    uint32_t buffer = 0;
    for (size_t i = 0; i < in_len; ++i) {
        char c = in[i];
        if (c == '=' || c == ' ' || c == '\n' || c == '\r') continue;
        int v = -1;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else return HOST_ERR_INVALID_ARG;
        buffer = (buffer << 5) | static_cast<uint32_t>(v);
        bits += 5;
        if (bits >= 8) {
            if (out_pos >= out_size) return HOST_ERR_NO_MEMORY;
            out[out_pos++] = static_cast<uint8_t>((buffer >> (bits - 8)) & 0xFF);
            bits -= 8;
        }
    }
    return static_cast<int>(out_pos);
}

int host_base64_encode(const uint8_t* in, size_t in_len, char* out, size_t out_size)
{
    if (!out || (!in && in_len > 0)) return HOST_ERR_INVALID_ARG;
    size_t olen = 0;
    int rc = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out), out_size,
                                    &olen, in, in_len);
    if (rc != 0) return HOST_ERR_NO_MEMORY;
    // Matrix/vodozemac use unpadded base64 for crypto fields; strip '=' padding so
    // the output round-trips with strict unpadded decoders on the receiving side.
    while (olen > 0 && out[olen - 1] == '=') --olen;
    out[olen] = '\0';
    return HOST_OK;
}

int host_base64_decode(const char* in, size_t in_len, uint8_t* out, size_t out_size)
{
    if (!in || !out) return HOST_ERR_INVALID_ARG;
    // mbedtls_base64_decode requires the input padded to a multiple of 4: it
    // drops the trailing partial group otherwise. Some inputs (e.g. Matrix SSSS
    // secrets) are unpadded, so normalise first - strip whitespace, then pad
    // with '=' to a multiple of 4 - before decoding with mbedTLS.
    std::string norm;
    norm.reserve(in_len + 4);
    for (size_t i = 0; i < in_len; ++i) {
        char c = in[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        norm.push_back(c);
    }
    while (norm.size() % 4 != 0) norm.push_back('=');
    size_t olen = 0;
    int rc = mbedtls_base64_decode(out, out_size, &olen,
                                    reinterpret_cast<const unsigned char*>(norm.data()),
                                    norm.size());
    if (rc != 0) return HOST_ERR_GENERIC;
    return static_cast<int>(olen);
}

int host_hex_encode(const uint8_t* in, size_t in_len, char* out, size_t out_size)
{
    if (!out || (!in && in_len > 0)) return HOST_ERR_INVALID_ARG;
    if (out_size < in_len * 2 + 1) return HOST_ERR_NO_MEMORY;
    static const char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0; i < in_len; ++i) {
        out[i * 2]     = HEX[in[i] >> 4];
        out[i * 2 + 1] = HEX[in[i] & 0x0F];
    }
    out[in_len * 2] = '\0';
    return HOST_OK;
}

int host_hex_decode(const char* in, size_t in_len, uint8_t* out, size_t out_size)
{
    if (!in || !out) return HOST_ERR_INVALID_ARG;
    if (in_len % 2 != 0) return HOST_ERR_INVALID_ARG;
    size_t need = in_len / 2;
    if (out_size < need) return HOST_ERR_NO_MEMORY;
    for (size_t i = 0; i < need; ++i) {
        int hi = cdc::plugin_manager::hex_val(in[i * 2]);
        int lo = cdc::plugin_manager::hex_val(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return HOST_ERR_INVALID_ARG;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return static_cast<int>(need);
}

}  // extern "C"
