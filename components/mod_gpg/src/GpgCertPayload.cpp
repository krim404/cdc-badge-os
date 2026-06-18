/**
 * \file
 * \brief (De)serialisation of a badge-to-badge certification return message.
 */

#include "mod_gpg/GpgCertPayload.h"
#include "mod_gpg/gpg.h"
#include "openpgp/xsig.h"

#include <cstring>
#include <ctime>

namespace cdc::mod_gpg {

size_t gpgBuildCertPayload(const gpg_recv_key_t& signed_key,
                           uint8_t* out, size_t out_size) {
    if (!out || signed_key.sig_len != 64) return 0;

    gpg_status_t status = {};
    if (!gpg_get_status(&status)) return 0;

    uint8_t sig_pkt[kGpgSelfCertSigMax];
    const size_t sig_len = buildCertSigPacket(signed_key, sig_pkt, sizeof(sig_pkt));
    if (sig_len == 0) return 0;

    const size_t uid_len = strnlen(status.user_id, sizeof(status.user_id));
    const size_t total = 20 + 20 + 1 + uid_len + 2 + sig_len;
    if (total > out_size) return 0;

    size_t off = 0;
    std::memcpy(out + off, signed_key.fingerprint_v4, 20);
    off += 20;
    std::memcpy(out + off, status.fingerprint, 20);
    off += 20;
    out[off++] = static_cast<uint8_t>(uid_len);
    std::memcpy(out + off, status.user_id, uid_len);
    off += uid_len;
    out[off++] = (sig_len >> 8) & 0xFF;
    out[off++] = sig_len & 0xFF;
    std::memcpy(out + off, sig_pkt, sig_len);
    off += sig_len;
    return off;
}

bool gpgParseCertPayload(const uint8_t* data, size_t len,
                         gpg_self_cert_t* out, uint8_t out_target_fp[20]) {
    if (!data || !out || !out_target_fp) return false;
    if (len < kGpgCertPayloadMin) return false;

    std::memset(out, 0, sizeof(*out));

    size_t off = 0;
    std::memcpy(out_target_fp, data + off, 20);
    off += 20;
    std::memcpy(out->issuer_fp_v4, data + off, 20);
    off += 20;

    const uint8_t uid_len = data[off++];
    if (uid_len > 63 || off + uid_len + 2 > len) return false;
    std::memcpy(out->issuer_uid, data + off, uid_len);
    out->issuer_uid[uid_len] = '\0';
    off += uid_len;

    const uint16_t sig_len = (static_cast<uint16_t>(data[off]) << 8) | data[off + 1];
    off += 2;
    if (sig_len == 0 || sig_len > kGpgSelfCertSigMax) return false;
    if (off + sig_len != len) return false;
    std::memcpy(out->sig_pkt, data + off, sig_len);
    out->sig_pkt_len = sig_len;

    out->received_at = static_cast<uint32_t>(std::time(nullptr));
    return true;
}

} // namespace cdc::mod_gpg
