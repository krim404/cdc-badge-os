/**
 * \brief Encrypted persistent storage for OpenPGP secret material.
 *
 * Encrypts payloads with AES-256-GCM. The wrapping key is derived via HKDF
 * over (chip_id || pin_hash) with the slot index appended; AAD binds the
 * record to (slot_id, magic) to defeat slot-shuffle attacks.
 */

#include "mod_gpg/GpgStorage.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_core/Crypto.h"
#include "cdc_log.h"
#include <mbedtls/sha256.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <esp_random.h>
#include <esp_attr.h>
#include <string.h>

static const char* TAG = "GPGStorage";

// Relative offsets within the mod_gpg R-Memory range. Each offset is the
// metadata slot for the matching ECC key slot (RMEM slot N pairs with ECC
// slot N). The range starts at slot 1 (= ECC slot 1 = SIG), which is
// hardware-only and needs no software metadata, so offset 0 is unused.
/** \brief R-Memory slot offset for the DEC private key payload (= ECC slot 2). */
static constexpr uint16_t RMEM_SLOT_DEC_KEY = 1;

/** \brief R-Memory slot offset for the symmetric AES key payload (= ECC slot 3). */
static constexpr uint16_t RMEM_SLOT_AES_KEY = 2;

/** \brief Magic marker for encrypted DEC private key records. */
static constexpr uint8_t DEC_KEY_MAGIC[4] = {'E', 'C', 'D', 'H'};

/** \brief Magic marker for symmetric AES key records (DO 0xD5). */
static constexpr uint8_t AES_KEY_MAGIC[4] = {'A', 'E', 'S', '1'};

static constexpr size_t MAGIC_SIZE = 4;
static constexpr size_t NONCE_SIZE = 12;
static constexpr size_t TAG_SIZE = 16;
static constexpr size_t PRIVKEY_SIZE = 32;
static constexpr size_t AES_MAX_KEY_SIZE = 32;
static constexpr size_t DEC_TOTAL_SIZE = MAGIC_SIZE + NONCE_SIZE + PRIVKEY_SIZE + TAG_SIZE;
static constexpr size_t AES_RECORD_PAYLOAD = 1 + AES_MAX_KEY_SIZE;
static constexpr size_t AES_TOTAL_SIZE = MAGIC_SIZE + NONCE_SIZE + AES_RECORD_PAYLOAD + TAG_SIZE;

/** \brief HKDF info string for storage key derivation. */
static constexpr char HKDF_INFO[] = "GPG-STORAGE-V2";

#ifdef __DOXYGEN__
namespace cdc::mod_gpg {
#endif

#pragma pack(push, 1)
struct DecKeyStorage {
    uint8_t magic[MAGIC_SIZE];
    uint8_t nonce[NONCE_SIZE];
    uint8_t encrypted[PRIVKEY_SIZE];
    uint8_t tag[TAG_SIZE];
};

struct AesKeyStorage {
    uint8_t magic[MAGIC_SIZE];
    uint8_t nonce[NONCE_SIZE];
    uint8_t encrypted[AES_RECORD_PAYLOAD];
    uint8_t tag[TAG_SIZE];
};
#pragma pack(pop)

#ifdef __DOXYGEN__
} // namespace cdc::mod_gpg
#endif

static_assert(sizeof(DecKeyStorage) == DEC_TOTAL_SIZE, "DecKeyStorage size mismatch");
static_assert(sizeof(AesKeyStorage) == AES_TOTAL_SIZE, "AesKeyStorage size mismatch");

namespace {

template <size_t N>
inline void secureWipe(uint8_t (&buf)[N]) {
    mbedtls_platform_zeroize(buf, N);
}

template <typename T>
inline void secureWipeObject(T& obj) {
    mbedtls_platform_zeroize(&obj, sizeof(obj));
}

} // namespace

static struct {
    bool ready = false;
    uint16_t eccStart = 0;
    uint16_t eccEnd = 0;
    uint16_t rmemStart = 0;
    uint16_t rmemEnd = 0;
    uint8_t sigSlot = 0;
    uint8_t decSlot = 0;
    uint8_t autSlot = 0;

    bool sessionActive = false;
    uint8_t sessionKey[32];
} s_storage;

static cdc::hal::ISecureElement* get_se() {
    return cdc::hal::getSecureElementInstance();
}

/**
 * \brief Computes SHA-256 over a PIN string.
 * \param pin PIN string; may be empty/`nullptr`.
 * \param hash_out 32-byte output buffer.
 * \return `true` on success.
 */
static bool pin_to_hash(const char* pin, uint8_t* hash_out) {
    if (!hash_out) return false;
    const uint8_t* in = pin ? reinterpret_cast<const uint8_t*>(pin) : reinterpret_cast<const uint8_t*>("");
    size_t in_len = pin ? strlen(pin) : 0;
    return mbedtls_sha256(in, in_len, hash_out, 0) == 0;
}

/**
 * \brief Derives a 32-byte storage key for a specific slot.
 * \param slot_id Absolute R-Memory slot identifier (used in HKDF salt).
 * \param pin_hash Optional 32-byte PIN hash; `nullptr` falls back to chip-bound key.
 * \param key_out 32-byte output buffer.
 * \return `true` on success.
 */
static bool derive_storage_key(uint16_t slot_id, const uint8_t* pin_hash, uint8_t* key_out) {
    if (!key_out) return false;

    uint8_t ikm[16 + 32] = {};
    size_t ikm_len = 16;
    auto* se = get_se();
    if (se) {
        se->getChipId(ikm, 16);
    }
    if (pin_hash) {
        memcpy(ikm + 16, pin_hash, 32);
        ikm_len = 16 + 32;
    }

    uint8_t salt[2];
    salt[0] = static_cast<uint8_t>((slot_id >> 8) & 0xFF);
    salt[1] = static_cast<uint8_t>(slot_id & 0xFF);

    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) {
        mbedtls_platform_zeroize(ikm, sizeof(ikm));
        return false;
    }

    int ret = mbedtls_hkdf(
        md,
        salt, sizeof(salt),
        ikm, ikm_len,
        reinterpret_cast<const uint8_t*>(HKDF_INFO), strlen(HKDF_INFO),
        key_out, 32
    );

    mbedtls_platform_zeroize(ikm, sizeof(ikm));
    return ret == 0;
}

/**
 * \brief Builds the 6-byte AAD for a slot: slot_id (BE) || magic (4).
 * \param slot_id Absolute R-Memory slot identifier.
 * \param magic 4-byte magic marker.
 * \param aad_out 6-byte output buffer.
 */
static void build_aad(uint16_t slot_id, const uint8_t magic[MAGIC_SIZE], uint8_t aad_out[6]) {
    aad_out[0] = static_cast<uint8_t>((slot_id >> 8) & 0xFF);
    aad_out[1] = static_cast<uint8_t>(slot_id & 0xFF);
    memcpy(aad_out + 2, magic, MAGIC_SIZE);
}

/**
 * \brief Resolves an absolute R-Memory slot index relative to the module range.
 * \param rel_index Offset within the module range.
 * \return Absolute slot index.
 */
static uint16_t resolve_slot(uint16_t rel_index) {
    return static_cast<uint16_t>(s_storage.rmemStart + rel_index);
}

void gpg_storage_set_slot_range(uint16_t eccStart, uint16_t eccEnd) {
    s_storage.ready = false;
    s_storage.eccStart = eccStart;
    s_storage.eccEnd = eccEnd;

    if (eccStart == 0 || eccEnd == 0 || eccStart > eccEnd || (eccEnd - eccStart + 1) < 3) {
        return;
    }

    s_storage.sigSlot = static_cast<uint8_t>(eccStart);
    s_storage.decSlot = static_cast<uint8_t>(eccStart + 1);
    s_storage.autSlot = static_cast<uint8_t>(eccStart + 2);
    s_storage.ready = true;
}

void gpg_storage_set_rmem_range(uint16_t rmemStart, uint16_t rmemEnd) {
    s_storage.rmemStart = rmemStart;
    s_storage.rmemEnd = rmemEnd;
}

bool gpg_storage_ready(void) { return s_storage.ready; }
uint8_t gpg_storage_sig_slot(void) { return s_storage.sigSlot; }
uint8_t gpg_storage_dec_slot(void) { return s_storage.decSlot; }
uint8_t gpg_storage_aut_slot(void) { return s_storage.autSlot; }

/**
 * \brief Encrypts and writes an arbitrary payload to a slot.
 * \param slot_id Absolute R-Memory slot.
 * \param magic 4-byte magic marker.
 * \param payload Plaintext bytes.
 * \param payload_len Plaintext length.
 * \param record_buf Scratch buffer; must hold MAGIC + NONCE + payload_len + TAG.
 * \param record_buf_len Length of `record_buf`.
 * \param pin Session PIN; `nullptr` falls back to chip-bound key.
 * \return `true` on success.
 */
static bool save_slot_encrypted(uint16_t slot_id,
                                const uint8_t magic[MAGIC_SIZE],
                                const uint8_t* payload, size_t payload_len,
                                uint8_t* record_buf, size_t record_buf_len,
                                const char* pin) {
    auto* se = get_se();
    if (!se) return false;
    if (record_buf_len < MAGIC_SIZE + NONCE_SIZE + payload_len + TAG_SIZE) return false;

    uint8_t pin_hash[32];
    bool have_pin = (pin && pin[0] != '\0');
    if (have_pin && !pin_to_hash(pin, pin_hash)) {
        mbedtls_platform_zeroize(pin_hash, sizeof(pin_hash));
        return false;
    }

    uint8_t enc_key[32];
    bool ok = derive_storage_key(slot_id, have_pin ? pin_hash : nullptr, enc_key);
    mbedtls_platform_zeroize(pin_hash, sizeof(pin_hash));
    if (!ok) {
        secureWipe(enc_key);
        return false;
    }

    uint8_t* p_magic = record_buf;
    uint8_t* p_nonce = record_buf + MAGIC_SIZE;
    uint8_t* p_ct    = record_buf + MAGIC_SIZE + NONCE_SIZE;
    uint8_t* p_tag   = p_ct + payload_len;

    memcpy(p_magic, magic, MAGIC_SIZE);
    if (!se->getRandomStrict(p_nonce, NONCE_SIZE)) {
        secureWipe(enc_key);
        LOG_E(TAG, "Cannot get hardware entropy for nonce on slot %u", slot_id);
        return false;
    }

    uint8_t aad[6];
    build_aad(slot_id, magic, aad);

    bool enc_ok = cdc::core::aesGcm256Seal(
        enc_key, p_nonce, NONCE_SIZE,
        aad, sizeof(aad),
        payload, payload_len,
        p_ct, p_tag);
    secureWipe(enc_key);
    if (!enc_ok) {
        LOG_E(TAG, "GCM encrypt failed");
        return false;
    }

    se->rmemErase(slot_id);
    size_t total_len = MAGIC_SIZE + NONCE_SIZE + payload_len + TAG_SIZE;
    if (se->rmemWrite(slot_id, record_buf, static_cast<uint16_t>(total_len)) != cdc::hal::SeResult::OK) {
        LOG_E(TAG, "rmemWrite slot %u failed", slot_id);
        return false;
    }
    return true;
}

/**
 * \brief Reads and decrypts a payload from a slot.
 * \param slot_id Absolute R-Memory slot.
 * \param magic 4-byte magic marker.
 * \param payload_out Output buffer.
 * \param payload_len Expected plaintext length.
 * \param pin Session PIN; `nullptr` falls back to chip-bound key.
 * \return `true` on success.
 */
static bool load_slot_decrypted(uint16_t slot_id,
                                const uint8_t magic[MAGIC_SIZE],
                                uint8_t* payload_out, size_t payload_len,
                                const char* pin) {
    auto* se = get_se();
    if (!se) return false;

    uint8_t buf[128];
    uint16_t buf_len = 0;
    size_t expected = MAGIC_SIZE + NONCE_SIZE + payload_len + TAG_SIZE;
    if (expected > sizeof(buf)) return false;

    if (se->rmemRead(slot_id, buf, sizeof(buf), &buf_len) != cdc::hal::SeResult::OK || buf_len < expected) {
        return false;
    }
    if (memcmp(buf, magic, MAGIC_SIZE) != 0) {
        return false;
    }

    uint8_t pin_hash[32];
    bool have_pin = (pin && pin[0] != '\0');
    if (have_pin && !pin_to_hash(pin, pin_hash)) {
        mbedtls_platform_zeroize(pin_hash, sizeof(pin_hash));
        mbedtls_platform_zeroize(buf, sizeof(buf));
        return false;
    }

    uint8_t dec_key[32];
    bool ok = derive_storage_key(slot_id, have_pin ? pin_hash : nullptr, dec_key);
    mbedtls_platform_zeroize(pin_hash, sizeof(pin_hash));
    if (!ok) {
        secureWipe(dec_key);
        mbedtls_platform_zeroize(buf, sizeof(buf));
        return false;
    }

    const uint8_t* p_nonce = buf + MAGIC_SIZE;
    const uint8_t* p_ct    = buf + MAGIC_SIZE + NONCE_SIZE;
    const uint8_t* p_tag   = p_ct + payload_len;

    uint8_t aad[6];
    build_aad(slot_id, magic, aad);

    bool dec_ok = cdc::core::aesGcm256Open(
        dec_key, p_nonce, NONCE_SIZE,
        aad, sizeof(aad),
        p_ct, payload_len,
        p_tag, payload_out);
    secureWipe(dec_key);
    mbedtls_platform_zeroize(buf, sizeof(buf));
    if (!dec_ok) {
        mbedtls_platform_zeroize(payload_out, payload_len);
        return false;
    }
    return true;
}

bool gpg_storage_save_dec_privkey(const uint8_t* privkey, const char* pin) {
    if (!privkey) return false;
    uint16_t slot = resolve_slot(RMEM_SLOT_DEC_KEY);
    uint8_t record[DEC_TOTAL_SIZE];
    bool ok = save_slot_encrypted(slot, DEC_KEY_MAGIC, privkey, PRIVKEY_SIZE,
                                  record, sizeof(record), pin);
    secureWipe(record);
    if (ok) {
        LOG_I(TAG, "Saved DEC private key (slot %u)", slot);
    }
    return ok;
}

bool gpg_storage_load_dec_privkey(uint8_t* privkey_out, const char* pin) {
    if (!privkey_out) return false;
    uint16_t slot = resolve_slot(RMEM_SLOT_DEC_KEY);
    return load_slot_decrypted(slot, DEC_KEY_MAGIC, privkey_out, PRIVKEY_SIZE, pin);
}

bool gpg_storage_has_dec_privkey(void) {
    auto* se = get_se();
    if (!se) return false;
    uint16_t slot = resolve_slot(RMEM_SLOT_DEC_KEY);
    uint8_t buf[DEC_TOTAL_SIZE];
    uint16_t buf_len = 0;
    if (se->rmemRead(slot, buf, sizeof(buf), &buf_len) != cdc::hal::SeResult::OK || buf_len < MAGIC_SIZE) {
        return false;
    }
    return memcmp(buf, DEC_KEY_MAGIC, MAGIC_SIZE) == 0;
}

bool gpg_storage_delete_dec_privkey(void) {
    auto* se = get_se();
    if (!se) return false;
    uint16_t slot = resolve_slot(RMEM_SLOT_DEC_KEY);
    return se->rmemErase(slot) == cdc::hal::SeResult::OK;
}

bool gpg_storage_save_aes_key(const uint8_t* key, size_t key_len, const char* pin) {
    if (!key) return false;
    if (key_len != 16 && key_len != 32) return false;
    uint16_t slot = resolve_slot(RMEM_SLOT_AES_KEY);

    uint8_t payload[AES_RECORD_PAYLOAD];
    payload[0] = static_cast<uint8_t>(key_len);
    memcpy(payload + 1, key, key_len);
    if (key_len < AES_MAX_KEY_SIZE) {
        memset(payload + 1 + key_len, 0, AES_MAX_KEY_SIZE - key_len);
    }

    uint8_t record[AES_TOTAL_SIZE];
    bool ok = save_slot_encrypted(slot, AES_KEY_MAGIC, payload, sizeof(payload),
                                  record, sizeof(record), pin);
    secureWipe(payload);
    secureWipe(record);
    if (ok) {
        LOG_I(TAG, "Saved AES key (slot %u, %zu bytes)", slot, key_len);
    }
    return ok;
}

bool gpg_storage_load_aes_key(uint8_t* key_out, size_t* key_len_out, const char* pin) {
    if (!key_out || !key_len_out) return false;
    uint16_t slot = resolve_slot(RMEM_SLOT_AES_KEY);

    uint8_t payload[AES_RECORD_PAYLOAD];
    if (!load_slot_decrypted(slot, AES_KEY_MAGIC, payload, sizeof(payload), pin)) {
        return false;
    }
    size_t len = payload[0];
    if (len != 16 && len != 32) {
        secureWipe(payload);
        return false;
    }
    memcpy(key_out, payload + 1, len);
    *key_len_out = len;
    secureWipe(payload);
    return true;
}

bool gpg_storage_has_aes_key(void) {
    auto* se = get_se();
    if (!se) return false;
    uint16_t slot = resolve_slot(RMEM_SLOT_AES_KEY);
    uint8_t buf[MAGIC_SIZE];
    uint16_t buf_len = 0;
    if (se->rmemRead(slot, buf, sizeof(buf), &buf_len) != cdc::hal::SeResult::OK || buf_len < MAGIC_SIZE) {
        return false;
    }
    return memcmp(buf, AES_KEY_MAGIC, MAGIC_SIZE) == 0;
}

bool gpg_storage_delete_aes_key(void) {
    auto* se = get_se();
    if (!se) return false;
    uint16_t slot = resolve_slot(RMEM_SLOT_AES_KEY);
    return se->rmemErase(slot) == cdc::hal::SeResult::OK;
}

/** \brief Magic marker for encrypted RSA private-key records. */
static constexpr uint8_t RSA_KEY_MAGIC[MAGIC_SIZE] = {'R', 'S', 'A', '1'};

static constexpr uint16_t RSA_SLOTS_PER_ROLE = 2;
static constexpr uint8_t  RSA_ROLE_COUNT = 3;
static constexpr size_t   RSA_REC_HEADER = 2;  // u16 LE record-body length prefix
static constexpr size_t   RSA_ENVELOPE = MAGIC_SIZE + NONCE_SIZE + TAG_SIZE;
static constexpr size_t   RSA_SLOT_CAP = cdc::hal::ISecureElement::RMEM_SLOT_SIZE;

static uint16_t s_rsa_rmem_start = 0;
static uint16_t s_rsa_rmem_end = 0;

// Single record scratch shared by the (single-threaded) RSA save/load path.
EXT_RAM_BSS_ATTR static uint8_t s_rsa_record[RSA_REC_HEADER + GPG_RSA_BLOB_MAX + RSA_ENVELOPE];

void gpg_storage_set_rsa_slot_range(uint16_t start, uint16_t end) {
    s_rsa_rmem_start = start;
    s_rsa_rmem_end = end;
}

/**
 * \brief Resolves the two consecutive R-Memory slots for an RSA key role.
 * \param role 0 = SIG, 1 = DEC, 2 = AUT.
 * \param slot0 First slot output.
 * \param slot1 Second slot output (RSA-4096 spills into it).
 * \return `true` if the range is configured and holds both slots.
 */
static bool rsa_role_slots(uint8_t role, uint16_t* slot0, uint16_t* slot1) {
    if (role >= RSA_ROLE_COUNT || s_rsa_rmem_start == 0) return false;
    uint16_t base = static_cast<uint16_t>(s_rsa_rmem_start + role * RSA_SLOTS_PER_ROLE);
    if (base + 1 > s_rsa_rmem_end) return false;
    *slot0 = base;
    *slot1 = static_cast<uint16_t>(base + 1);
    return true;
}

bool gpg_storage_save_rsa_key(uint8_t role, const uint8_t* blob, size_t blob_len, const char* pin) {
    if (!blob || blob_len == 0 || blob_len > GPG_RSA_BLOB_MAX) return false;
    uint16_t slot0 = 0, slot1 = 0;
    if (!rsa_role_slots(role, &slot0, &slot1)) return false;
    auto* se = get_se();
    if (!se) return false;

    uint8_t pin_hash[32];
    bool have_pin = (pin && pin[0] != '\0');
    if (have_pin && !pin_to_hash(pin, pin_hash)) {
        mbedtls_platform_zeroize(pin_hash, sizeof(pin_hash));
        return false;
    }
    uint8_t enc_key[32];
    bool ok = derive_storage_key(slot0, have_pin ? pin_hash : nullptr, enc_key);
    mbedtls_platform_zeroize(pin_hash, sizeof(pin_hash));
    if (!ok) {
        secureWipe(enc_key);
        return false;
    }

    const size_t rec_body = RSA_ENVELOPE + blob_len;  // magic + nonce + ct + tag
    const size_t total = RSA_REC_HEADER + rec_body;
    s_rsa_record[0] = static_cast<uint8_t>(rec_body & 0xFF);
    s_rsa_record[1] = static_cast<uint8_t>((rec_body >> 8) & 0xFF);
    uint8_t* p_magic = s_rsa_record + RSA_REC_HEADER;
    uint8_t* p_nonce = p_magic + MAGIC_SIZE;
    uint8_t* p_ct    = p_nonce + NONCE_SIZE;
    uint8_t* p_tag   = p_ct + blob_len;
    memcpy(p_magic, RSA_KEY_MAGIC, MAGIC_SIZE);
    if (!se->getRandomStrict(p_nonce, NONCE_SIZE)) {
        secureWipe(enc_key);
        return false;
    }
    uint8_t aad[6];
    build_aad(slot0, RSA_KEY_MAGIC, aad);
    bool enc_ok = cdc::core::aesGcm256Seal(enc_key, p_nonce, NONCE_SIZE, aad, sizeof(aad),
                                           blob, blob_len, p_ct, p_tag);
    secureWipe(enc_key);
    if (!enc_ok) {
        mbedtls_platform_zeroize(s_rsa_record, total);
        return false;
    }

    const size_t c0 = (total > RSA_SLOT_CAP) ? RSA_SLOT_CAP : total;
    se->rmemErase(slot0);
    se->rmemErase(slot1);
    bool wrote = se->rmemWrite(slot0, s_rsa_record, static_cast<uint16_t>(c0)) == cdc::hal::SeResult::OK;
    if (wrote && total > c0) {
        wrote = se->rmemWrite(slot1, s_rsa_record + c0,
                              static_cast<uint16_t>(total - c0)) == cdc::hal::SeResult::OK;
    }
    mbedtls_platform_zeroize(s_rsa_record, total);
    if (!wrote) {
        LOG_E(TAG, "RSA key write failed (role %u)", role);
        return false;
    }
    LOG_I(TAG, "Saved RSA key (role %u, %zu bytes, slots %u/%u)", role, blob_len, slot0, slot1);
    return true;
}

bool gpg_storage_load_rsa_key(uint8_t role, uint8_t* blob_out, size_t blob_cap,
                              size_t* blob_len_out, const char* pin) {
    if (!blob_out || !blob_len_out) return false;
    uint16_t slot0 = 0, slot1 = 0;
    if (!rsa_role_slots(role, &slot0, &slot1)) return false;
    auto* se = get_se();
    if (!se) return false;

    uint16_t l0 = 0;
    if (se->rmemRead(slot0, s_rsa_record, sizeof(s_rsa_record), &l0) != cdc::hal::SeResult::OK ||
        l0 < RSA_REC_HEADER + RSA_ENVELOPE) {
        return false;
    }
    const size_t rec_body = static_cast<size_t>(s_rsa_record[0]) |
                            (static_cast<size_t>(s_rsa_record[1]) << 8);
    const size_t total = RSA_REC_HEADER + rec_body;
    if (rec_body < RSA_ENVELOPE || total > sizeof(s_rsa_record)) {
        return false;
    }
    size_t have = l0;
    if (total > l0) {
        uint16_t l1 = 0;
        if (se->rmemRead(slot1, s_rsa_record + l0,
                         static_cast<uint16_t>(sizeof(s_rsa_record) - l0), &l1) != cdc::hal::SeResult::OK) {
            return false;
        }
        have = static_cast<size_t>(l0) + l1;
    }
    if (have < total) return false;
    if (memcmp(s_rsa_record + RSA_REC_HEADER, RSA_KEY_MAGIC, MAGIC_SIZE) != 0) {
        return false;
    }
    const size_t blob_len = rec_body - RSA_ENVELOPE;
    if (blob_len == 0 || blob_len > GPG_RSA_BLOB_MAX || blob_len > blob_cap) {
        return false;
    }

    uint8_t pin_hash[32];
    bool have_pin = (pin && pin[0] != '\0');
    if (have_pin && !pin_to_hash(pin, pin_hash)) {
        mbedtls_platform_zeroize(pin_hash, sizeof(pin_hash));
        mbedtls_platform_zeroize(s_rsa_record, total);
        return false;
    }
    uint8_t dec_key[32];
    bool ok = derive_storage_key(slot0, have_pin ? pin_hash : nullptr, dec_key);
    mbedtls_platform_zeroize(pin_hash, sizeof(pin_hash));
    if (!ok) {
        secureWipe(dec_key);
        mbedtls_platform_zeroize(s_rsa_record, total);
        return false;
    }

    const uint8_t* p_nonce = s_rsa_record + RSA_REC_HEADER + MAGIC_SIZE;
    const uint8_t* p_ct    = p_nonce + NONCE_SIZE;
    const uint8_t* p_tag   = p_ct + blob_len;
    uint8_t aad[6];
    build_aad(slot0, RSA_KEY_MAGIC, aad);
    bool dec_ok = cdc::core::aesGcm256Open(dec_key, p_nonce, NONCE_SIZE, aad, sizeof(aad),
                                           p_ct, blob_len, p_tag, blob_out);
    secureWipe(dec_key);
    mbedtls_platform_zeroize(s_rsa_record, total);
    if (!dec_ok) {
        mbedtls_platform_zeroize(blob_out, blob_len);
        return false;
    }
    *blob_len_out = blob_len;
    return true;
}

bool gpg_storage_has_rsa_key(uint8_t role) {
    uint16_t slot0 = 0, slot1 = 0;
    if (!rsa_role_slots(role, &slot0, &slot1)) return false;
    auto* se = get_se();
    if (!se) return false;
    uint8_t buf[RSA_REC_HEADER + MAGIC_SIZE];
    uint16_t buf_len = 0;
    if (se->rmemRead(slot0, buf, sizeof(buf), &buf_len) != cdc::hal::SeResult::OK ||
        buf_len < sizeof(buf)) {
        return false;
    }
    return memcmp(buf + RSA_REC_HEADER, RSA_KEY_MAGIC, MAGIC_SIZE) == 0;
}

bool gpg_storage_delete_rsa_key(uint8_t role) {
    uint16_t slot0 = 0, slot1 = 0;
    if (!rsa_role_slots(role, &slot0, &slot1)) return false;
    auto* se = get_se();
    if (!se) return false;
    bool a = se->rmemErase(slot0) == cdc::hal::SeResult::OK;
    bool b = se->rmemErase(slot1) == cdc::hal::SeResult::OK;
    return a && b;
}

void gpg_storage_set_session_pin(const char* pin) {
    if (!pin) {
        gpg_storage_clear_session();
        return;
    }
    uint8_t hash[32];
    if (pin_to_hash(pin, hash)) {
        memcpy(s_storage.sessionKey, hash, sizeof(hash));
        s_storage.sessionActive = true;
    }
    mbedtls_platform_zeroize(hash, sizeof(hash));
}

bool gpg_storage_get_session_key(uint8_t* key_out) {
    if (!s_storage.sessionActive || !key_out) {
        return false;
    }
    memcpy(key_out, s_storage.sessionKey, 32);
    return true;
}

void gpg_storage_clear_session(void) {
    mbedtls_platform_zeroize(s_storage.sessionKey, sizeof(s_storage.sessionKey));
    s_storage.sessionActive = false;
}
