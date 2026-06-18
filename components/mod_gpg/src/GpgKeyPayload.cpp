/**
 * \file
 * \brief Transport-agnostic (de)serialisation of a GPG public key payload.
 */

#include "mod_gpg/GpgKeyPayload.h"
#include "mod_gpg/GpgStorage.h"
#include "mod_gpg/gpg.h"
#include "openpgp/fingerprint.h"

#include "cdc_hal/ISecureElement.h"

#include <cstring>
#include <ctime>

namespace cdc::mod_gpg {

namespace {

void writeBe32(uint8_t* out, uint32_t v) {
    out[0] = (v >> 24) & 0xFF;
    out[1] = (v >> 16) & 0xFF;
    out[2] = (v >> 8)  & 0xFF;
    out[3] = v & 0xFF;
}

uint32_t readBe32(const uint8_t* in) {
    return (static_cast<uint32_t>(in[0]) << 24) |
           (static_cast<uint32_t>(in[1]) << 16) |
           (static_cast<uint32_t>(in[2]) << 8)  |
           static_cast<uint32_t>(in[3]);
}

} // namespace

size_t gpgBuildOwnKeyPayload(uint8_t* out, size_t out_size) {
    gpg_status_t status = {};
    if (!gpg_get_status(&status)) return 0;

    const uint8_t curve = status.curve;
    const uint8_t pubkey_len = (curve == CDC_CURVE_ED25519) ? 32 : 64;

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return 0;

    uint8_t pubkey[64] = {0};
    cdc::hal::EccCurve hal_curve = cdc::hal::EccCurve::P256;
    if (se->eccGetPublicKey(gpg_storage_sig_slot(), pubkey, &hal_curve)
        != cdc::hal::SeResult::OK) {
        return 0;
    }

    const size_t uid_len = strnlen(status.user_id, sizeof(status.user_id));
    const size_t total = 1 + 1 + pubkey_len + 4 + 20 + 1 + uid_len;
    if (total > out_size) return 0;

    size_t off = 0;
    out[off++] = curve;
    out[off++] = pubkey_len;
    std::memcpy(out + off, pubkey, pubkey_len);
    off += pubkey_len;
    writeBe32(out + off, status.created_at);
    off += 4;
    std::memcpy(out + off, status.fingerprint, 20);
    off += 20;
    out[off++] = static_cast<uint8_t>(uid_len);
    std::memcpy(out + off, status.user_id, uid_len);
    off += uid_len;
    return off;
}

size_t gpgBuildRecvKeyPayload(const gpg_recv_key_t& key, uint8_t* out, size_t out_size) {
    const uint8_t pubkey_len = key.pubkey_len;
    if (pubkey_len != 32 && pubkey_len != 64) return 0;

    const size_t uid_len = strnlen(key.user_id, sizeof(key.user_id));
    const size_t total = 1 + 1 + pubkey_len + 4 + 20 + 1 + uid_len;
    if (total > out_size) return 0;

    size_t off = 0;
    out[off++] = key.curve;
    out[off++] = pubkey_len;
    std::memcpy(out + off, key.pubkey, pubkey_len);
    off += pubkey_len;
    writeBe32(out + off, key.created_at);
    off += 4;
    std::memcpy(out + off, key.fingerprint_v4, 20);
    off += 20;
    out[off++] = static_cast<uint8_t>(uid_len);
    std::memcpy(out + off, key.user_id, uid_len);
    off += uid_len;
    return off;
}

bool gpgParseKeyPayload(const uint8_t* data, size_t len, gpg_recv_key_t* out) {
    if (!data || !out) return false;
    if (len < kGpgKeyPayloadMin) return false;

    size_t off = 0;
    const uint8_t curve = data[off++];
    if (curve != CDC_CURVE_ED25519 && curve != CDC_CURVE_P256) return false;

    const uint8_t pubkey_len = data[off++];
    if (curve == CDC_CURVE_ED25519 && pubkey_len != 32) return false;
    if (curve == CDC_CURVE_P256    && pubkey_len != 64) return false;
    if (off + pubkey_len + 4 + 20 + 1 > len) return false;

    std::memset(out, 0, sizeof(*out));
    out->curve = curve;
    out->pubkey_len = pubkey_len;
    std::memcpy(out->pubkey, data + off, pubkey_len);
    off += pubkey_len;
    out->created_at = readBe32(data + off);
    off += 4;
    std::memcpy(out->fingerprint_v4, data + off, 20);
    off += 20;

    const uint8_t uid_len = data[off++];
    if (uid_len > 63 || off + uid_len > len) return false;
    std::memcpy(out->user_id, data + off, uid_len);
    out->user_id[uid_len] = '\0';

    // Reject the payload unless the transmitted fingerprint reproduces from the
    // transmitted creation time, so the stored key (and any certification over
    // it) binds to the peer's real OpenPGP key.
    uint8_t recomputed_fp[20] = {0};
    if (!calculateFingerprintV4(curve, out->pubkey, out->pubkey_len,
                                out->created_at, recomputed_fp)) {
        return false;
    }
    if (std::memcmp(recomputed_fp, out->fingerprint_v4, 20) != 0) return false;

    out->received_at = static_cast<uint32_t>(std::time(nullptr));
    calculateFingerprintV5(curve, out->pubkey, out->pubkey_len,
                           out->created_at, out->fingerprint_v5);
    return true;
}

} // namespace cdc::mod_gpg
