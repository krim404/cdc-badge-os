#pragma once

#include <cstdint>
#include <cstddef>

namespace cdc::mod_gpg {

constexpr uint8_t kGpgRecvFlagVerified = 0x01;

/**
 * \brief One GPG public key received from another badge.
 *
 * Persisted as a single NVS blob per key. The on-wire BLE payload supplies
 * `curve`, `pubkey`, `pubkey_len`, `created_at`, `fingerprint_v4` and
 * `user_id`; the badge computes `fingerprint_v5` locally and fills
 * `received_at` from the RTC. `created_at` is the originator's key-creation
 * time and must reproduce `fingerprint_v4` (validated on receive) so the
 * certification binds to the peer's real OpenPGP key. `my_signature` /
 * `sig_len` / `sig_created_at` / `flags` start out as zeros and are filled by
 * `setSignature()` after a cross-sign action; `sig_created_at` is the
 * signature creation time embedded into both the signed hash and the export.
 */
#pragma pack(push, 1)
struct gpg_recv_key_t {
    uint8_t  curve;
    char     user_id[64];
    uint8_t  pubkey[64];
    uint8_t  pubkey_len;
    uint32_t created_at;
    uint8_t  fingerprint_v4[20];
    uint8_t  fingerprint_v5[32];
    uint32_t received_at;
    uint8_t  my_signature[64];
    uint8_t  sig_len;
    uint32_t sig_created_at;
    // DEC (RFC 6637 ECDH P-256) encryption subkey and the two signatures the
    // peer produced over it. Cannot be forged locally; replayed verbatim into
    // the armored export. All-zero when absent.
    uint8_t  pubkey_dec[64];
    uint32_t created_at_dec;
    uint8_t  owner_self_sig[64];
    uint8_t  dec_binding_sig[64];
    uint8_t  flags;
};
#pragma pack(pop)

static_assert(sizeof(gpg_recv_key_t) ==
              (1 + 64 + 64 + 1 + 4 + 20 + 32 + 4 + 64 + 1 + 4 +
               64 + 4 + 64 + 64 + 1),
              "gpg_recv_key_t layout drift");

/**
 * \brief Sort entry used to expose a stable ordered index over NVS keys.
 *
 * NVS iteration order is unspecified, so callers build a sorted snapshot
 * before reading by index. Allocate `gpg_recv_index_entry_t[kMaxKeys]` in
 * PSRAM (see `cdc::core::psramAlloc`).
 */
struct gpg_recv_index_entry_t {
    char     nvs_key[16];
    uint32_t received_at;
    uint8_t  flags;
};

/**
 * \brief NVS-backed store for cross-sign target keys received via BLE.
 *
 * Singleton. All public methods open and close their own NVS handle, so the
 * store is safe to call from any task without external synchronisation
 * (NVS itself serialises writes).
 */
class GpgRecvStore {
public:
    /// Hard ceiling. Past this `addKey` rejects further inserts.
    static constexpr uint8_t kMaxKeys = 128;

    static GpgRecvStore& instance();

    /// Persist a new key. Replaces an existing entry if the fingerprint matches.
    bool addKey(const gpg_recv_key_t& key);

    /// Number of stored keys.
    uint8_t count();

    /**
     * \brief Build the sorted index (oldest first).
     * \param out Caller-owned buffer of at least `max` entries (use PSRAM).
     * \param max Capacity of `out`.
     * \return Number of entries written (capped at `max` and `kMaxKeys`).
     */
    uint8_t listIndex(gpg_recv_index_entry_t* out, uint8_t max);

    /// Load one key by sorted index (0..count()-1).
    bool getKey(uint8_t index, gpg_recv_key_t* out);

    /// Remove one key by sorted index. No-op if `index` is out of range.
    bool deleteKey(uint8_t index);

    /// Attach a cross-signature, its creation time and flag bits to an entry.
    bool setSignature(uint8_t index,
                      const uint8_t* sig, uint8_t sig_len,
                      uint32_t sig_created_at,
                      uint8_t flags);

private:
    GpgRecvStore() = default;
    GpgRecvStore(const GpgRecvStore&) = delete;
    GpgRecvStore& operator=(const GpgRecvStore&) = delete;

    /// Derive the NVS key name from the first 4 bytes of the V4 fingerprint.
    static void deriveKeyName(const uint8_t fp_v4[20], char out[16]);

    /// Read a blob into `out` by NVS key name. Returns true on success.
    bool readByName(const char* nvs_key, gpg_recv_key_t* out);

    /// Write a blob and commit. Returns true on success.
    bool writeByName(const char* nvs_key, const gpg_recv_key_t& key);

    /// Resolve `index` to its NVS key name via the sorted snapshot.
    bool resolveKeyName(uint8_t index, char out[16]);
};

} // namespace cdc::mod_gpg
