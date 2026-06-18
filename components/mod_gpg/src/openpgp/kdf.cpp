/**
 * \brief OpenPGP KDF Data Object (tag F9) byte codec (see kdf.h).
 */

#include "mod_gpg/openpgp/kdf.h"
#include <string.h>

void kdf_do_clear(kdf_do_t* out) {
    if (out) memset(out, 0, sizeof(*out));
}

/** \brief Accepts the hash identifiers the codec understands. */
static bool valid_hash(uint8_t h) {
    return h == KDF_HASH_NONE || h == KDF_HASH_SHA256 || h == KDF_HASH_SHA512;
}

/** \brief Emits a single 1-byte-length inner TLV; returns false on overflow. */
static bool emit_tlv(uint8_t* out, size_t cap, size_t* pos, uint8_t tag,
                     const uint8_t* v, uint8_t l) {
    if (*pos + 2u + l > cap) return false;
    out[(*pos)++] = tag;
    out[(*pos)++] = l;
    memcpy(out + *pos, v, l);
    *pos += l;
    return true;
}

kdf_status_t kdf_do_parse(const uint8_t* bytes, size_t len, kdf_do_t* out) {
    if (!bytes || !out) return KDF_ERR_NULL;
    kdf_do_clear(out);

    size_t pos = 0;
    while (pos < len) {
        if (pos + 2 > len) return KDF_ERR_BAD_LENGTH;
        const uint8_t tag = bytes[pos++];
        const uint8_t l = bytes[pos++];
        if (pos + l > len) return KDF_ERR_BAD_LENGTH;
        const uint8_t* v = bytes + pos;
        pos += l;

        switch (tag) {
            case 0x81:
                if (l != 1) return KDF_ERR_BAD_LENGTH;
                if (v[0] != KDF_ALGO_NONE && v[0] != KDF_ALGO_PBKDF2) return KDF_ERR_BAD_ALGO;
                out->algo = static_cast<kdf_algo_t>(v[0]);
                break;
            case 0x82:
                if (l != 1) return KDF_ERR_BAD_LENGTH;
                if (!valid_hash(v[0])) return KDF_ERR_BAD_HASH;
                out->hash = static_cast<kdf_hash_t>(v[0]);
                break;
            case 0x83:
                if (l != 4) return KDF_ERR_BAD_LENGTH;
                out->iter_count = (static_cast<uint32_t>(v[0]) << 24) |
                                  (static_cast<uint32_t>(v[1]) << 16) |
                                  (static_cast<uint32_t>(v[2]) << 8) |
                                  static_cast<uint32_t>(v[3]);
                break;
            case 0x84:
                if (l != KDF_SALT_LEN) return KDF_ERR_BAD_LENGTH;
                memcpy(out->pw1_salt, v, KDF_SALT_LEN);
                out->has_pw1_salt = true;
                break;
            case 0x85:
                if (l != KDF_SALT_LEN) return KDF_ERR_BAD_LENGTH;
                memcpy(out->rc_salt, v, KDF_SALT_LEN);
                out->has_rc_salt = true;
                break;
            case 0x86:
                if (l != KDF_SALT_LEN) return KDF_ERR_BAD_LENGTH;
                memcpy(out->pw3_salt, v, KDF_SALT_LEN);
                out->has_pw3_salt = true;
                break;
            case 0x87:
                if (l != 32 && l != 64) return KDF_ERR_BAD_HASH_LEN;
                memcpy(out->pw1_initial, v, l);
                out->pw1_initial_len = l;
                out->has_pw1_initial = true;
                break;
            case 0x88:
                if (l != 32 && l != 64) return KDF_ERR_BAD_HASH_LEN;
                memcpy(out->pw3_initial, v, l);
                out->pw3_initial_len = l;
                out->has_pw3_initial = true;
                break;
            default:
                return KDF_ERR_BAD_TAG;
        }
    }
    return KDF_OK;
}

kdf_status_t kdf_do_build(const kdf_do_t* kdf, uint8_t* out, size_t out_cap, size_t* out_len) {
    if (!kdf || !out || !out_len) return KDF_ERR_NULL;
    size_t pos = 0;
    const uint8_t algo = static_cast<uint8_t>(kdf->algo);
    const uint8_t hash = static_cast<uint8_t>(kdf->hash);
    if (!emit_tlv(out, out_cap, &pos, 0x81, &algo, 1)) return KDF_ERR_BUF_TOO_SMALL;
    if (!emit_tlv(out, out_cap, &pos, 0x82, &hash, 1)) return KDF_ERR_BUF_TOO_SMALL;

    if (kdf->algo == KDF_ALGO_PBKDF2) {
        const uint8_t it[4] = {
            static_cast<uint8_t>((kdf->iter_count >> 24) & 0xFF),
            static_cast<uint8_t>((kdf->iter_count >> 16) & 0xFF),
            static_cast<uint8_t>((kdf->iter_count >> 8) & 0xFF),
            static_cast<uint8_t>(kdf->iter_count & 0xFF),
        };
        if (!emit_tlv(out, out_cap, &pos, 0x83, it, 4)) return KDF_ERR_BUF_TOO_SMALL;
        if (kdf->has_pw1_salt && !emit_tlv(out, out_cap, &pos, 0x84, kdf->pw1_salt, KDF_SALT_LEN))
            return KDF_ERR_BUF_TOO_SMALL;
        if (kdf->has_rc_salt && !emit_tlv(out, out_cap, &pos, 0x85, kdf->rc_salt, KDF_SALT_LEN))
            return KDF_ERR_BUF_TOO_SMALL;
        if (kdf->has_pw3_salt && !emit_tlv(out, out_cap, &pos, 0x86, kdf->pw3_salt, KDF_SALT_LEN))
            return KDF_ERR_BUF_TOO_SMALL;
        if (kdf->has_pw1_initial &&
            !emit_tlv(out, out_cap, &pos, 0x87, kdf->pw1_initial, kdf->pw1_initial_len))
            return KDF_ERR_BUF_TOO_SMALL;
        if (kdf->has_pw3_initial &&
            !emit_tlv(out, out_cap, &pos, 0x88, kdf->pw3_initial, kdf->pw3_initial_len))
            return KDF_ERR_BUF_TOO_SMALL;
    }
    *out_len = pos;
    return KDF_OK;
}

kdf_status_t kdf_do_build_disabled(uint8_t* out, size_t out_cap, size_t* out_len) {
    if (!out || !out_len) return KDF_ERR_NULL;
    if (out_cap < 3) return KDF_ERR_BUF_TOO_SMALL;
    out[0] = 0x81;
    out[1] = 0x01;
    out[2] = KDF_ALGO_NONE;
    *out_len = 3;
    return KDF_OK;
}
