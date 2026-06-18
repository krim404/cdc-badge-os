#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::mod_gpg {

/// Maximum length of a stored certification signature packet body (Tag 2).
constexpr size_t kGpgSelfCertSigMax = 160;

/**
 * \brief One third-party certification received over the badge's own key.
 *
 * Persisted as a single NVS blob per issuer. `sig_pkt` holds the verbatim
 * OpenPGP Signature Packet body (Tag 2 content, no tag/length header) produced
 * by the issuer over this badge's public key; it is re-emitted unchanged when
 * the badge exports its own public key, so the certification merges onto the
 * key in a GPG keyring.
 */
#pragma pack(push, 1)
struct gpg_self_cert_t {
    uint8_t  issuer_fp_v4[20];          ///< Issuer (signer) v4 fingerprint.
    char     issuer_uid[64];            ///< Issuer user-id (display only).
    uint32_t received_at;               ///< Receive timestamp (RTC).
    uint16_t sig_pkt_len;               ///< Length of `sig_pkt`.
    uint8_t  sig_pkt[kGpgSelfCertSigMax];
};
#pragma pack(pop)

static_assert(sizeof(gpg_self_cert_t) ==
              (20 + 64 + 4 + 2 + kGpgSelfCertSigMax),
              "gpg_self_cert_t layout drift");

/**
 * \brief Sort entry exposing a stable ordered index over the NVS blobs.
 */
struct gpg_self_cert_index_entry_t {
    char     nvs_key[16];
    uint32_t received_at;
};

/**
 * \brief NVS-backed store of third-party certifications on the badge's own key.
 *
 * Singleton. All public methods open and close their own NVS handle, so the
 * store is safe to call from any task without external synchronisation.
 */
class GpgSelfCertStore {
public:
    /// Hard ceiling. Past this `addCert` rejects further inserts.
    static constexpr uint8_t kMaxCerts = 16;

    static GpgSelfCertStore& instance();

    /// Persist a certification. Replaces an existing entry from the same issuer.
    bool addCert(const gpg_self_cert_t& cert);

    /// Number of stored certifications.
    uint8_t count();

    /**
     * \brief Build the sorted index (oldest first).
     * \param out Caller-owned buffer of at least `max` entries.
     * \param max Capacity of `out`.
     * \return Number of entries written (capped at `max` and `kMaxCerts`).
     */
    uint8_t listIndex(gpg_self_cert_index_entry_t* out, uint8_t max);

    /// Load one certification by sorted index (0..count()-1).
    bool getCert(uint8_t index, gpg_self_cert_t* out);

    /// Remove one certification by sorted index. No-op if out of range.
    bool deleteCert(uint8_t index);

private:
    GpgSelfCertStore() = default;
    GpgSelfCertStore(const GpgSelfCertStore&) = delete;
    GpgSelfCertStore& operator=(const GpgSelfCertStore&) = delete;

    static void deriveKeyName(const uint8_t issuer_fp_v4[20], char out[16]);
    bool readByName(const char* nvs_key, gpg_self_cert_t* out);
    bool writeByName(const char* nvs_key, const gpg_self_cert_t& cert);
    bool resolveKeyName(uint8_t index, char out[16]);
};

} // namespace cdc::mod_gpg
