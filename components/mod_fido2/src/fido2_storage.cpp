/**
 * \file
 * \brief FIDO2 storage layer using secure-element ECC slots, R-Memory, and NVS counters.
 */

#include "mod_fido2/fido2_storage.h"
#include "mod_fido2/fido2_common.h"
#include "mod_fido2/LargeBlobStore.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_log.h"
#include "esp_attr.h"
#include <mbedtls/sha256.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>

using cdc::mod_fido2::sha256;

static const char* TAG = "FIDO2";

/** \brief Persistent storage layout definitions. */

#define FIDO2_RMEM_MAGIC        "FID2"
#define FIDO2_RMEM_MAGIC_LEN    4
#define NVS_NAMESPACE           "fido2"
#define NVS_KEY_COUNTER         "auth_cnt"
#define NVS_KEY_LARGEBLOB       "lblob"
#define NVS_KEY_ALWAYS_UV       "always_uv"
#define NVS_KEY_MIN_PIN         "min_pin"

#ifdef __DOXYGEN__
namespace cdc::mod_fido2 {
#endif

#pragma pack(push, 1)
typedef struct {
    uint8_t magic[FIDO2_RMEM_MAGIC_LEN];    // "FID2"
    uint8_t rp_id_hash[32];                 // SHA-256 of RP ID
    char rp_id[FIDO2_RP_ID_MAX_LEN];        // RP ID string (for display)
    uint8_t user_id[FIDO2_USER_ID_MAX_LEN]; // User handle
    uint8_t user_id_len;                    // Length of user ID
    char user_name[FIDO2_USER_NAME_MAX_LEN];// Display name
    uint32_t sign_count;                    // Per-credential counter
    uint8_t cred_id_nonce[16];              // Random nonce for credential ID
    uint8_t flags;                          // Flags (resident, cred_protect, etc.)
    uint8_t cred_protect;                   // Credential protection level
    uint8_t curve;                          // CDC_CURVE_P256 or CDC_CURVE_ED25519
    uint8_t reserved[7];                    // Reserved for future use
} fido2_stored_cred_t;                      // Total: ~180 bytes
#pragma pack(pop)

#ifdef __DOXYGEN__
} // namespace cdc::mod_fido2
#endif

#define FIDO2_STORED_SIZE sizeof(fido2_stored_cred_t)

/** \brief Stored-credential flag bits. */
#define FIDO2_FLAG_RESIDENT     0x01

/** \brief Runtime storage/cache state. */

EXT_RAM_BSS_ATTR static struct {
    bool initialized;
    uint32_t auth_counter;
    bool counter_loaded;

    // Cached credential info
    struct {
        bool valid;
        uint8_t rp_id_hash[32];
        char rp_id[FIDO2_RP_ID_MAX_LEN];
        char user_name[FIDO2_USER_NAME_MAX_LEN];
        uint8_t user_id[FIDO2_USER_ID_MAX_LEN];  // User handle for replacement detection
        uint8_t user_id_len;
        uint32_t sign_count;
        bool resident;
        uint8_t cred_protect;
        uint8_t curve;  // CDC_CURVE_P256 or CDC_CURVE_ED25519
    } creds[FIDO2_MAX_CREDENTIALS];

    uint8_t cred_count;
} g_storage = {};

static uint8_t s_ecc_start = 0;
static uint8_t s_ecc_end = 0;
static uint16_t s_rmem_start = 0;
static uint16_t s_rmem_end = 0;

/**
 * \brief Configures FIDO2 storage slot ranges.
 * \param ecc_start First ECC slot.
 * \param ecc_end Last ECC slot.
 * \param rmem_start First RMEM slot.
 * \param rmem_end Last RMEM slot.
 */
void fido2_storage_set_slot_range(uint8_t ecc_start, uint8_t ecc_end,
                                  uint16_t rmem_start, uint16_t rmem_end) {
    s_ecc_start = ecc_start;
    s_ecc_end = ecc_end;
    s_rmem_start = rmem_start;
    s_rmem_end = rmem_end;
}

/**
 * \brief Returns configured ECC start slot.
 * \return ECC start slot index.
 */
uint8_t fido2_storage_ecc_start(void) { return s_ecc_start; }

/**
 * \brief Returns configured ECC end slot.
 * \return ECC end slot index.
 */
uint8_t fido2_storage_ecc_end(void) { return s_ecc_end; }

/**
 * \brief Returns configured RMEM start slot.
 * \return RMEM start slot index.
 */
uint16_t fido2_storage_rmem_start(void) { return s_rmem_start; }

/**
 * \brief Returns configured RMEM end slot.
 * \return RMEM end slot index.
 */
uint16_t fido2_storage_rmem_end(void) { return s_rmem_end; }

/**
 * \brief Validates slot-range configuration.
 * \return `true` if ranges are monotonic.
 */
static bool slot_range_valid(void) {
    return s_ecc_end >= s_ecc_start && s_rmem_end >= s_rmem_start;
}

/**
 * \brief Returns number of configured logical ECC slots.
 * \return ECC slot count.
 */
static uint16_t ecc_count(void) {
    if (!slot_range_valid()) return 0;
    return static_cast<uint16_t>(s_ecc_end - s_ecc_start + 1);
}

/**
 * \brief Returns number of configured logical RMEM slots.
 * \return RMEM slot count.
 */
static uint16_t rmem_count(void) {
    if (!slot_range_valid()) return 0;
    return static_cast<uint16_t>(s_rmem_end - s_rmem_start + 1);
}

/**
 * \brief Checks whether logical slot index is within range.
 * \param slot Logical slot index.
 * \return `true` if valid.
 */
static bool slot_logical_valid(uint8_t slot) {
    uint16_t count = ecc_count();
    return count > 0 && slot < count;
}

/**
 * \brief Maps logical slot to physical ECC slot.
 * \param slot Logical slot index.
 * \return Physical ECC slot.
 */
static uint8_t ecc_slot_for_logical(uint8_t slot) {
    return static_cast<uint8_t>(s_ecc_start + slot);
}

/**
 * \brief Maps logical slot to physical RMEM slot.
 * \param slot Logical slot index.
 * \return Physical RMEM slot.
 */
static uint16_t rmem_slot_for_logical(uint8_t slot) {
    if (!slot_range_valid()) return 0;
    uint16_t offset = static_cast<uint16_t>(slot);
    return static_cast<uint16_t>(s_rmem_start + offset);
}

/** \brief Internal helper functions for slot and cache management. */

/**
 * \brief Reads a stored credential from R-Memory and validates its magic header.
 * \param logical_slot Logical credential slot index.
 * \param stored Output structure receiving the stored credential payload.
 * \return `true` on successful read and validation, otherwise `false`.
 */
static bool read_rmem_credential(uint8_t logical_slot, fido2_stored_cred_t* stored) {
    if (!stored) return false;

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;

    uint16_t rmem_slot = rmem_slot_for_logical(logical_slot);
    uint8_t data[256];
    uint16_t size = 0;

    auto res = se->rmemRead(rmem_slot, data, sizeof(data), &size);
    if (res != cdc::hal::SeResult::OK || size < FIDO2_STORED_SIZE) {
        return false;
    }

    auto* tmp = reinterpret_cast<fido2_stored_cred_t*>(data);
    if (memcmp(tmp->magic, FIDO2_RMEM_MAGIC, FIDO2_RMEM_MAGIC_LEN) != 0) {
        return false;
    }

    memcpy(stored, tmp, sizeof(fido2_stored_cred_t));
    return true;
}

/** \brief Updates cache entry from stored credential payload. */
/**
 * \brief Updates in-memory cache entry from persisted credential structure.
 * \param slot Logical slot index.
 * \param stored Stored credential payload.
 * \param is_resident Resident-key flag.
 */
static void update_cache_from_stored(uint8_t slot, const fido2_stored_cred_t* stored,
                                      bool is_resident) {
    g_storage.creds[slot].valid = true;
    memcpy(g_storage.creds[slot].rp_id_hash, stored->rp_id_hash, 32);
    strncpy(g_storage.creds[slot].rp_id, stored->rp_id, FIDO2_RP_ID_MAX_LEN - 1);
    strncpy(g_storage.creds[slot].user_name, stored->user_name, FIDO2_USER_NAME_MAX_LEN - 1);
    g_storage.creds[slot].user_id_len = stored->user_id_len;
    if (stored->user_id_len > 0) {
        memcpy(g_storage.creds[slot].user_id, stored->user_id, stored->user_id_len);
    }
    g_storage.creds[slot].sign_count = stored->sign_count;
    g_storage.creds[slot].resident = is_resident;
    g_storage.creds[slot].cred_protect = stored->cred_protect;
    g_storage.creds[slot].curve = stored->curve;
}

/**
 * \brief Erases ECC key material and R-Memory data for a logical slot.
 * \param logical_slot Logical credential slot index.
 * \return void
 */
static void erase_slot_data(uint8_t logical_slot) {
    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return;

    uint8_t phys_slot = ecc_slot_for_logical(logical_slot);
    se->eccDelete(phys_slot);

    uint16_t rmem_slot = rmem_slot_for_logical(logical_slot);
    se->rmemErase(rmem_slot);
}

/** \brief DER ASN.1 tags used for ECDSA signature encoding. */
static constexpr uint8_t DER_TAG_SEQUENCE = 0x30;
static constexpr uint8_t DER_TAG_INTEGER = 0x02;
/** \brief MSB mask used to detect when DER INTEGER needs a 0x00 padding byte. */
static constexpr uint8_t DER_INTEGER_MSB_MASK = 0x80;

/**
 * \brief Encodes a single ECDSA P-256 component (R or S) as a DER INTEGER.
 * \param p Output cursor (advances past written bytes).
 * \param mpi Big-endian magnitude buffer of length `FIDO2_SIG_COMPONENT_SIZE`.
 * \return Updated output cursor positioned after the encoded INTEGER.
 *
 * Strips leading zero bytes (keeping at least one) and prepends a 0x00 padding
 * byte when the MSB is set, ensuring the integer remains non-negative in DER.
 */
static uint8_t* encode_der_integer(uint8_t* p, const uint8_t* mpi) {
    // Skip leading zeros, but keep at least one byte. We only strip a zero if
    // the next byte's MSB is clear, otherwise the zero is required as padding.
    size_t skip = 0;
    while (skip + 1 < FIDO2_SIG_COMPONENT_SIZE && mpi[skip] == 0 &&
           !(mpi[skip + 1] & DER_INTEGER_MSB_MASK)) {
        skip++;
    }
    uint8_t pad = (mpi[skip] & DER_INTEGER_MSB_MASK) ? 1 : 0;
    uint8_t actual_len = static_cast<uint8_t>(FIDO2_SIG_COMPONENT_SIZE - skip);

    *p++ = DER_TAG_INTEGER;
    *p++ = static_cast<uint8_t>(pad + actual_len);
    if (pad) {
        *p++ = 0x00;
    }
    memcpy(p, mpi + skip, actual_len);
    return p + actual_len;
}

/** \brief Converts raw 64-byte ECDSA signature (`R||S`) to DER sequence format. */
/**
 * \brief Converts a raw 64-byte ECDSA signature into DER encoding.
 * \param raw_sig Input raw signature buffer (`R || S`).
 * \param der_sig Output buffer that receives DER-encoded signature data.
 * \return Number of bytes written to `der_sig`.
 */
static uint8_t raw_sig_to_der(const uint8_t raw_sig[FIDO2_SIG_SIZE], uint8_t* der_sig) {
    uint8_t* p = der_sig;
    *p++ = DER_TAG_SEQUENCE;

    // Reserve a placeholder for the SEQUENCE length, then encode R and S.
    uint8_t* len_pos = p++;
    uint8_t* r_end = encode_der_integer(p, raw_sig);
    uint8_t* end = encode_der_integer(r_end, raw_sig + FIDO2_SIG_COMPONENT_SIZE);

    *len_pos = static_cast<uint8_t>(end - len_pos - 1);
    return static_cast<uint8_t>(end - der_sig);
}


/**
 * \brief Writes credential metadata to R-Memory after erasing the destination slot.
 * \param logical_slot Logical credential slot index.
 * \param stored Credential payload to persist.
 * \return `true` if write succeeded, otherwise `false`.
 */
static bool write_rmem_credential(uint8_t logical_slot, const fido2_stored_cred_t* stored) {
    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;

    uint16_t rmem_slot = rmem_slot_for_logical(logical_slot);

    // Erase first (R-Memory requires empty slot)
    se->rmemErase(rmem_slot);

    if (se->rmemWrite(rmem_slot, reinterpret_cast<const uint8_t*>(stored),
                      FIDO2_STORED_SIZE) != cdc::hal::SeResult::OK) {
        LOG_E(TAG, "Failed to write credential metadata to slot %d", rmem_slot);
        return false;
    }
    return true;
}

/** \brief NVS-backed global authentication counter operations. */

/**
 * \brief Loads global authentication counter from NVS.
 */
void fido2_storage_counter_load(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            LOG_W(TAG, "Failed to open NVS for counter: %s", esp_err_to_name(err));
        }
        g_storage.auth_counter = 0;
        g_storage.counter_loaded = true;
        return;
    }

    err = nvs_get_u32(nvs, NVS_KEY_COUNTER, &g_storage.auth_counter);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            LOG_W(TAG, "Failed to read counter from NVS: %s", esp_err_to_name(err));
        }
        g_storage.auth_counter = 0;
    } else {
        LOG_I(TAG, "Loaded auth counter: %lu", g_storage.auth_counter);
    }

    nvs_close(nvs);
    g_storage.counter_loaded = true;
}

/**
 * \brief Returns current global authentication counter.
 * \return Counter value.
 */
uint32_t fido2_storage_counter_get(void) {
    if (!g_storage.counter_loaded) {
        fido2_storage_counter_load();
    }
    return g_storage.auth_counter;
}

/**
 * \brief Increments and persists global authentication counter.
 * \return `true` on successful persistence.
 */
bool fido2_storage_counter_increment(void) {
    if (!g_storage.counter_loaded) {
        fido2_storage_counter_load();
    }
    uint32_t new_value = g_storage.auth_counter + 1;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        LOG_E(TAG, "Failed to open NVS for counter write: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_u32(nvs, NVS_KEY_COUNTER, new_value);
    if (err != ESP_OK) {
        LOG_E(TAG, "Failed to set counter in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return false;
    }

    err = nvs_commit(nvs);
    if (err != ESP_OK) {
        LOG_E(TAG, "NVS commit failed for counter: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return false;
    }

    nvs_close(nvs);
    g_storage.auth_counter = new_value;
    return true;
}

/**
 * \brief No-op flush retained for API stability; per-increment path commits.
 * \return Always `true`.
 */
bool fido2_storage_counter_flush(void) {
    return true;
}

/** \brief authenticatorLargeBlobs and authenticatorConfig persistence (NVS). */

uint16_t fido2_storage_largeblob_length(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return cdc::mod_fido2::kLargeBlobEmptyLen;
    }
    size_t sz = 0;
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY_LARGEBLOB, nullptr, &sz);
    nvs_close(nvs);
    if (err != ESP_OK || sz == 0) {
        return cdc::mod_fido2::kLargeBlobEmptyLen;
    }
    return static_cast<uint16_t>(sz);
}

bool fido2_storage_largeblob_get(uint8_t* out, uint16_t max_len, uint16_t* out_len) {
    if (!out || !out_len) return false;

    bool fall_back_to_empty = false;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        fall_back_to_empty = true;
    } else {
        size_t sz = max_len;
        esp_err_t err = nvs_get_blob(nvs, NVS_KEY_LARGEBLOB, out, &sz);
        nvs_close(nvs);
        if (err == ESP_OK) {
            *out_len = static_cast<uint16_t>(sz);
            return true;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            LOG_W(TAG, "largeblob read failed: %s", esp_err_to_name(err));
            return false;
        }
        fall_back_to_empty = true;
    }

    if (fall_back_to_empty) {
        if (max_len < cdc::mod_fido2::kLargeBlobEmptyLen) return false;
        memcpy(out, cdc::mod_fido2::kLargeBlobEmpty, cdc::mod_fido2::kLargeBlobEmptyLen);
        *out_len = cdc::mod_fido2::kLargeBlobEmptyLen;
        return true;
    }
    return false;
}

bool fido2_storage_largeblob_set(const uint8_t* data, uint16_t len) {
    if (!data || len == 0 || len > cdc::mod_fido2::kLargeBlobMaxArray) return false;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(nvs, NVS_KEY_LARGEBLOB, data, len);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

bool fido2_storage_get_always_uv(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(nvs, NVS_KEY_ALWAYS_UV, &v);
    nvs_close(nvs);
    return err == ESP_OK && v != 0;
}

bool fido2_storage_set_always_uv(bool enabled) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(nvs, NVS_KEY_ALWAYS_UV, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

uint8_t fido2_storage_get_min_pin_len(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return 0;
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(nvs, NVS_KEY_MIN_PIN, &v);
    nvs_close(nvs);
    return (err == ESP_OK) ? v : 0;
}

bool fido2_storage_set_min_pin_len(uint8_t min_len) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(nvs, NVS_KEY_MIN_PIN, min_len);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

void fido2_storage_config_reset(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_erase_key(nvs, NVS_KEY_LARGEBLOB);
    nvs_erase_key(nvs, NVS_KEY_ALWAYS_UV);
    nvs_erase_key(nvs, NVS_KEY_MIN_PIN);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/** \brief Initialization and cache rebuild routines. */

/**
 * \brief Initializes FIDO2 storage cache from secure element and NVS.
 * \return Number of discovered credentials.
 */
uint8_t fido2_storage_init(void) {
    LOG_I(TAG, "Initializing storage...");

    memset(&g_storage, 0, sizeof(g_storage));
    fido2_storage_counter_load();

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) {
        LOG_E(TAG, "No secure element available");
        return 0;
    }

    // Load credential metadata from R-Memory
    if (!slot_range_valid()) {
        LOG_E(TAG, "Slot range not configured");
        return 0;
    }

    uint16_t count = ecc_count();
    uint16_t rcount = rmem_count();
    if (rcount < count) {
        LOG_E(TAG, "R-Memory range smaller than ECC range");
        return 0;
    }

    for (uint8_t i = 0; i < count && i < FIDO2_MAX_CREDENTIALS; i++) {
        fido2_stored_cred_t stored;
        if (read_rmem_credential(i, &stored)) {
            bool is_resident = (stored.flags & FIDO2_FLAG_RESIDENT) != 0;
            update_cache_from_stored(i, &stored, is_resident);
            g_storage.cred_count++;
            LOG_D(TAG, "Found credential %d: %s (curve=%d)", i, stored.rp_id, stored.curve);
        }
    }

    g_storage.initialized = true;
    LOG_I(TAG, "Found %d credentials", g_storage.cred_count);
    return g_storage.cred_count;
}

/** \brief Credential lookup operations using in-memory cache only. */

/**
 * \brief Returns number of cached credentials.
 * \return Credential count.
 */
uint8_t fido2_storage_count(void) {
    return g_storage.cred_count;
}

/**
 * \brief Checks whether logical slot is occupied.
 * \param slot Logical slot index.
 * \return `true` if used.
 */
bool fido2_storage_slot_used(uint8_t slot) {
    if (!slot_logical_valid(slot)) return false;
    return g_storage.creds[slot].valid;
}

/**
 * \brief Finds first unused logical slot.
 * \return Logical slot index or `-1` if full.
 */
int8_t fido2_storage_find_free_slot(void) {
    uint16_t count = ecc_count();
    for (uint8_t i = 0; i < count && i < FIDO2_MAX_CREDENTIALS; i++) {
        if (!g_storage.creds[i].valid) {
            return i;
        }
    }
    return -1;
}

/**
 * \brief Finds credentials matching RP hash.
 * \param rp_id_hash RP ID hash (32 bytes).
 * \param out_slots Output slot array.
 * \param max_slots Maximum writable slots.
 * \return Number of matches.
 */
uint8_t fido2_storage_find_by_rp(const uint8_t *rp_id_hash,
                                  uint8_t *out_slots, uint8_t max_slots) {
    uint8_t count = 0;

    uint16_t total = ecc_count();
    for (uint8_t i = 0; i < total && i < FIDO2_MAX_CREDENTIALS && count < max_slots; i++) {
        if (g_storage.creds[i].valid &&
            memcmp(g_storage.creds[i].rp_id_hash, rp_id_hash, 32) == 0) {
            out_slots[count++] = i;
        }
    }

    return count;
}

/**
 * \brief Finds resident credentials matching RP hash.
 * \param rp_id_hash RP ID hash (32 bytes).
 * \param out_slots Output slot array.
 * \param max_slots Maximum writable slots.
 * \return Number of matches.
 */
uint8_t fido2_storage_find_by_rp_resident(const uint8_t *rp_id_hash,
                                          uint8_t *out_slots, uint8_t max_slots) {
    uint8_t count = 0;

    LOG_D(TAG, "Searching for resident creds, total=%d", g_storage.cred_count);
    uint16_t total = ecc_count();
    for (uint8_t i = 0; i < total && i < FIDO2_MAX_CREDENTIALS && count < max_slots; i++) {
        if (g_storage.creds[i].valid) {
            bool rp_match = memcmp(g_storage.creds[i].rp_id_hash, rp_id_hash, 32) == 0;
            LOG_D(TAG, "Slot %d: valid=%d resident=%d rp_match=%d rp=%s",
                  i, g_storage.creds[i].valid, g_storage.creds[i].resident,
                  rp_match, g_storage.creds[i].rp_id);
            if (g_storage.creds[i].resident && rp_match) {
                out_slots[count++] = i;
            }
        }
    }

    return count;
}

/**
 * \brief Returns resident-key flag for slot.
 * \param slot Logical slot index.
 * \return `true` if resident credential.
 */
bool fido2_storage_is_resident(uint8_t slot) {
    if (!slot_logical_valid(slot)) return false;
    return g_storage.creds[slot].valid && g_storage.creds[slot].resident;
}

/**
 * \brief Finds credential by RP hash and user handle for replacement logic.
 * \param rp_id_hash RP ID hash (32 bytes).
 * \param user_id User handle bytes.
 * \param user_id_len User handle length.
 * \return Matching slot index or `-1`.
 */
int8_t fido2_storage_find_by_rp_user(const uint8_t *rp_id_hash,
                                      const uint8_t *user_id,
                                      uint8_t user_id_len) {
    if (!rp_id_hash) return -1;

    uint16_t total = ecc_count();
    for (uint8_t i = 0; i < total && i < FIDO2_MAX_CREDENTIALS; i++) {
        if (!g_storage.creds[i].valid) continue;

        // Check RP ID hash match
        if (memcmp(g_storage.creds[i].rp_id_hash, rp_id_hash, 32) != 0) continue;

        // Check User ID match
        if (g_storage.creds[i].user_id_len != user_id_len) continue;
        if (user_id_len == 0) {
            // Both have empty user_id - match!
            LOG_D(TAG, "Found existing credential in slot %d (empty user_id)", i);
            return i;
        }
        if (user_id && memcmp(g_storage.creds[i].user_id, user_id, user_id_len) == 0) {
            LOG_D(TAG, "Found existing credential in slot %d for replacement", i);
            return i;
        }
    }

    return -1;  // No existing credential found
}

/**
 * \brief Resolves and verifies logical slot from credential-id blob.
 * \param cred_id Credential ID bytes.
 * \param cred_id_len Credential ID length.
 * \return Slot index or `-1` on mismatch.
 */
int8_t fido2_storage_find_slot_by_cred_id(const uint8_t *cred_id, uint16_t cred_id_len) {
    if (!cred_id || cred_id_len != FIDO2_CRED_ID_LEN) return -1;

    uint8_t slot = cred_id[0];
    if (!slot_logical_valid(slot) || !g_storage.creds[slot].valid) {
        return -1;
    }

    uint8_t stored_id[FIDO2_CRED_ID_LEN];
    if (!fido2_storage_get_cred_id(slot, stored_id)) {
        return -1;
    }

    if (memcmp(stored_id, cred_id, FIDO2_CRED_ID_LEN) != 0) {
        return -1;
    }

    return (int8_t)slot;
}

/**
 * \brief Loads user handle and optional user name for a credential slot.
 * \param slot Logical slot index.
 * \param user_id Output user-handle buffer.
 * \param user_id_len Output user-handle length.
 * \param user_name Output user-name buffer.
 * \param user_name_max User-name buffer size.
 * \return `true` on success.
 */
bool fido2_storage_get_user(uint8_t slot,
                            uint8_t *user_id,
                            uint8_t *user_id_len,
                            char *user_name,
                            size_t user_name_max) {
    if (!slot_logical_valid(slot) || !g_storage.creds[slot].valid) {
        return false;
    }

    fido2_stored_cred_t stored;
    if (!read_rmem_credential(slot, &stored)) {
        return false;
    }

    if (user_id && user_id_len) {
        uint8_t len = stored.user_id_len;
        if (len > FIDO2_USER_ID_MAX_LEN) len = FIDO2_USER_ID_MAX_LEN;
        memcpy(user_id, stored.user_id, len);
        *user_id_len = len;
    }

    if (user_name && user_name_max > 0) {
        size_t copy_len = strnlen(stored.user_name, FIDO2_USER_NAME_MAX_LEN);
        if (copy_len >= user_name_max) copy_len = user_name_max - 1;
        memcpy(user_name, stored.user_name, copy_len);
        user_name[copy_len] = '\0';
    }

    return true;
}

/**
 * \brief Verifies credential-id for logical slot.
 * \param slot Logical slot index.
 * \param cred_id Credential ID bytes.
 * \return `true` if credential-id matches slot data.
 */
bool fido2_storage_verify_cred_id(uint8_t slot, const uint8_t *cred_id) {
    if (!cred_id) return false;
    uint8_t stored_id[FIDO2_CRED_ID_LEN];
    if (!fido2_storage_get_cred_id(slot, stored_id)) {
        return false;
    }
    return memcmp(stored_id, cred_id, FIDO2_CRED_ID_LEN) == 0;
}

/**
 * \brief Builds credential-id blob for logical slot.
 * \param slot Logical slot index.
 * \param out_cred_id Output credential-id buffer.
 * \return `true` on success.
 */
bool fido2_storage_get_cred_id(uint8_t slot, uint8_t *out_cred_id) {
    if (!slot_logical_valid(slot) || !g_storage.creds[slot].valid || !out_cred_id) {
        return false;
    }

    fido2_stored_cred_t stored;
    if (!read_rmem_credential(slot, &stored)) {
        LOG_E(TAG, "Failed to read credential %d", slot);
        return false;
    }

    // Build credential ID: slot (1) + nonce (16) + padding (47) = 64 bytes
    memset(out_cred_id, 0, FIDO2_CRED_ID_LEN);
    out_cred_id[0] = slot;
    memcpy(out_cred_id + 1, stored.cred_id_nonce, 16);

    return true;
}

/** \brief Credential create/read/delete operations. */

/**
 * \brief Returns cached credential metadata for slot.
 * \param slot Logical slot index.
 * \param info Output credential info.
 * \return `true` on success.
 */
bool fido2_storage_get_credential(uint8_t slot, fido2_credential_info_t *info) {
    if (!slot_logical_valid(slot) || !g_storage.creds[slot].valid || !info) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    info->slot = slot;
    memcpy(info->rp_id_hash, g_storage.creds[slot].rp_id_hash, 32);
    strncpy(info->rp_id, g_storage.creds[slot].rp_id, FIDO2_RP_ID_MAX_LEN - 1);
    strncpy(info->user_name, g_storage.creds[slot].user_name, FIDO2_USER_NAME_MAX_LEN - 1);
    info->user_id_len = 0;
    info->sign_count = g_storage.creds[slot].sign_count;
    info->resident_key = g_storage.creds[slot].resident;
    info->cred_protect = g_storage.creds[slot].cred_protect;
    info->curve = g_storage.creds[slot].curve;

    // Load user ID from R-Memory (not cached)
    uint8_t user_id_len = 0;
    if (fido2_storage_get_user(slot, info->user_id, &user_id_len,
                               info->user_name, FIDO2_USER_NAME_MAX_LEN)) {
        info->user_id_len = user_id_len;
    }

    return true;
}

/**
 * \brief Returns stored curve identifier for slot.
 * \param slot Logical slot index.
 * \return Curve id or `0xFF` if invalid.
 */
uint8_t fido2_storage_get_curve(uint8_t slot) {
    if (!slot_logical_valid(slot) || !g_storage.creds[slot].valid) {
        return 0xFF;  // Invalid
    }
    return g_storage.creds[slot].curve;
}

/**
 * \brief Creates or replaces credential in secure-element storage.
 * \param rp_id Relying-party id string.
 * \param rp_id_hash RP ID hash (32 bytes).
 * \param user_id User handle bytes.
 * \param user_id_len User handle length.
 * \param user_name User display name.
 * \param resident_key Resident-key flag.
 * \param cred_protect Credential protection policy.
 * \param curve Requested key curve.
 * \param out_slot Output logical slot.
 * \param out_cred_id Output credential-id.
 * \param out_pubkey Output public key bytes.
 * \return `true` on success.
 */
bool fido2_storage_create_credential(
    const char *rp_id,
    const uint8_t *rp_id_hash,
    const uint8_t *user_id,
    uint8_t user_id_len,
    const char *user_name,
    bool resident_key,
    uint8_t cred_protect,
    uint8_t curve,
    uint8_t *out_slot,
    uint8_t *out_cred_id,
    uint8_t *out_pubkey
) {
    if (user_id && user_id_len > FIDO2_USER_ID_MAX_LEN) {
        LOG_E(TAG, "User ID too long: %u", user_id_len);
        return false;
    }

    // FIDO2 spec: If credential with same RP ID + User ID exists, replace it
    int8_t existing_slot = fido2_storage_find_by_rp_user(rp_id_hash, user_id, user_id_len);
    int8_t slot;

    if (existing_slot >= 0) {
        // Replace existing credential
        LOG_I(TAG, "Replacing existing credential in slot %d", existing_slot);
        slot = existing_slot;

        // Erase existing key and metadata
        erase_slot_data(static_cast<uint8_t>(slot));

        // Update cache: mark as invalid temporarily, will be re-validated after creation
        g_storage.creds[slot].valid = false;
        g_storage.cred_count--;
    } else {
        // Find free slot for new credential
        slot = fido2_storage_find_free_slot();
        if (slot < 0) {
            LOG_E(TAG, "No free slots");
            return false;
        }
    }

    const char *curve_name = (curve == CDC_CURVE_ED25519) ? "Ed25519" : "P-256";
    LOG_I(TAG, "Creating %s credential in slot %d for %s", curve_name, slot, rp_id);

    // Explicitly erase ECC slot first to ensure it's empty
    // (handles cache/chip state mismatch)
    LOG_D(TAG, "Erasing slot %d before key generation", slot);
    uint8_t phys_slot = ecc_slot_for_logical(static_cast<uint8_t>(slot));
    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;
    se->eccDelete(phys_slot);

    // Generate ECC key with requested curve
    cdc::hal::EccCurve se_curve =
        (curve == CDC_CURVE_ED25519) ? cdc::hal::EccCurve::ED25519
                                     : cdc::hal::EccCurve::P256;
    if (se->eccGenerate(phys_slot, se_curve) != cdc::hal::SeResult::OK) {
        LOG_E(TAG, "Failed to generate %s key in slot %d", curve_name, slot);
        return false;
    }

    // Read public key
    // P-256: 64 bytes (X||Y without 0x04 prefix)
    // Ed25519: 32 bytes
    uint8_t pubkey[64];
    uint8_t pubkey_size = (curve == CDC_CURVE_ED25519) ? 32 : 64;
    cdc::hal::EccCurve se_read_curve = cdc::hal::EccCurve::P256;
    if (se->eccGetPublicKey(phys_slot, pubkey, &se_read_curve) != cdc::hal::SeResult::OK) {
        LOG_E(TAG, "Failed to read public key from slot %d", slot);
        se->eccDelete(phys_slot);
        return false;
    }

    // Generate random nonce for credential ID
    uint8_t nonce[16];
    if (!se->getRandom(nonce, 16)) {
        LOG_E(TAG, "Failed to generate nonce");
        se->eccDelete(phys_slot);
        return false;
    }

    // Build credential ID (64 bytes)
    // Format: slot (1) + nonce (16) + padding (47)
    // In production, use HMAC for binding
    memset(out_cred_id, 0, FIDO2_CRED_ID_LEN);
    out_cred_id[0] = slot;
    memcpy(out_cred_id + 1, nonce, 16);

    // Prepare stored credential
    fido2_stored_cred_t stored;
    memset(&stored, 0, sizeof(stored));
    memcpy(stored.magic, FIDO2_RMEM_MAGIC, FIDO2_RMEM_MAGIC_LEN);
    memcpy(stored.rp_id_hash, rp_id_hash, 32);
    if (rp_id) {
        strncpy(stored.rp_id, rp_id, FIDO2_RP_ID_MAX_LEN - 1);
    }
    if (user_id && user_id_len > 0) {
        memcpy(stored.user_id, user_id, user_id_len);
        stored.user_id_len = user_id_len;
    }
    if (user_name) {
        strncpy(stored.user_name, user_name, FIDO2_USER_NAME_MAX_LEN - 1);
    }
    stored.sign_count = 0;
    memcpy(stored.cred_id_nonce, nonce, 16);
    stored.flags = resident_key ? FIDO2_FLAG_RESIDENT : 0;
    stored.cred_protect = cred_protect;
    stored.curve = curve;

    // Write to R-Memory
    if (!write_rmem_credential(static_cast<uint8_t>(slot), &stored)) {
        se->eccDelete(phys_slot);
        return false;
    }

    // Update local cache
    update_cache_from_stored(static_cast<uint8_t>(slot), &stored, resident_key);
    g_storage.cred_count++;

    // Copy public key output (32 bytes for Ed25519, 64 for P-256)
    memcpy(out_pubkey, pubkey, pubkey_size);
    *out_slot = slot;

    LOG_I(TAG, "Created %s credential in slot %d", curve_name, slot);
    return true;
}

/**
 * \brief Deletes credential and associated slot data.
 * \param slot Logical slot index.
 * \return `true` on success.
 */
bool fido2_storage_delete_credential(uint8_t slot) {
    if (!slot_logical_valid(slot) || !g_storage.creds[slot].valid) {
        return false;
    }

    LOG_I(TAG, "Deleting credential in slot %d", slot);

    // Erase ECC key and R-Memory
    erase_slot_data(slot);

    // Update local cache
    g_storage.creds[slot].valid = false;
    g_storage.cred_count--;

    LOG_I(TAG, "Deleted credential in slot %d", slot);
    return true;
}

/**
 * \brief Increments per-credential sign counter and persists metadata.
 * \param slot Logical slot index.
 * \return New sign count or `0` on failure.
 */
uint32_t fido2_storage_increment_sign_count(uint8_t slot) {
    if (!slot_logical_valid(slot) || !g_storage.creds[slot].valid) {
        return 0;
    }

    // Increment local cache
    g_storage.creds[slot].sign_count++;
    uint32_t new_count = g_storage.creds[slot].sign_count;

    // Read current stored data from TROPIC01
    fido2_stored_cred_t stored;
    if (read_rmem_credential(slot, &stored)) {
        stored.sign_count = new_count;
        if (!write_rmem_credential(slot, &stored)) {
            LOG_E(TAG, "CRITICAL: Failed to persist sign count for slot %d!", slot);
        }
    }

    return new_count;
}

/** \brief Signing operations requiring secure-element access. */

/**
 * \brief Signs message hash with ECDSA and returns DER signature.
 * \param slot Logical slot index.
 * \param msg Message bytes.
 * \param msg_len Message length.
 * \param signature Output signature buffer.
 * \param sig_len Output signature length.
 * \return `true` on success.
 */
bool fido2_storage_sign(uint8_t slot, const uint8_t *msg, uint16_t msg_len,
                        uint8_t *signature, uint8_t *sig_len) {
    if (!slot_logical_valid(slot) || !g_storage.creds[slot].valid) {
        return false;
    }

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;

    uint8_t raw_sig[FIDO2_SIG_SIZE];
    size_t raw_len = sizeof(raw_sig);
    uint8_t phys_slot = ecc_slot_for_logical(slot);
    if (se->ecdsaSign(phys_slot, msg, msg_len, raw_sig, &raw_len) !=
            cdc::hal::SeResult::OK ||
        raw_len != FIDO2_SIG_SIZE) {
        LOG_E(TAG, "ECDSA sign failed for slot %d", slot);
        return false;
    }

    // Convert to DER format
    *sig_len = raw_sig_to_der(raw_sig, signature);

    LOG_D(TAG, "Signed with slot %d, sig_len=%d", slot, *sig_len);
    return true;
}

/**
 * \brief Signs message and returns raw signature (EdDSA/ECDSA).
 * \param slot Logical slot index.
 * \param msg Message bytes.
 * \param msg_len Message length.
 * \param signature Output raw signature buffer.
 * \param sig_len Output signature length.
 * \return `true` on success.
 */
bool fido2_storage_sign_raw(uint8_t slot, const uint8_t *msg, uint16_t msg_len,
                            uint8_t *signature, uint8_t *sig_len) {
    if (!slot_logical_valid(slot) || !g_storage.creds[slot].valid) {
        return false;
    }

    uint8_t curve = g_storage.creds[slot].curve;

    if (curve == CDC_CURVE_ED25519) {
        // EdDSA sign: sign message directly
        auto* se = cdc::hal::getSecureElementInstance();
        if (!se) return false;

        uint8_t phys_slot = ecc_slot_for_logical(slot);
        if (se->eddsaSign(phys_slot, msg, msg_len, signature) != cdc::hal::SeResult::OK) {
            LOG_E(TAG, "EdDSA sign failed for slot %d", slot);
            return false;
        }
        *sig_len = FIDO2_SIG_SIZE;  // Ed25519 signature is always 64 bytes
        LOG_D(TAG, "EdDSA signed %d bytes with slot %d", msg_len, slot);
    } else {
        auto* se = cdc::hal::getSecureElementInstance();
        if (!se) return false;
        uint8_t phys_slot = ecc_slot_for_logical(slot);
        size_t raw_len = FIDO2_SIG_SIZE;
        if (se->ecdsaSign(phys_slot, msg, msg_len, signature, &raw_len) !=
                cdc::hal::SeResult::OK ||
            raw_len != FIDO2_SIG_SIZE) {
            LOG_E(TAG, "ECDSA sign failed for slot %d", slot);
            return false;
        }
        *sig_len = FIDO2_SIG_SIZE;  // Raw P-256 signature (R||S) is always 64 bytes
        LOG_D(TAG, "ECDSA signed %d bytes with slot %d", msg_len, slot);
    }

    return true;
}

/** \brief Signs data and returns DER-encoded signature for U2F compatibility. */
/**
 * \brief Signs message hash and returns DER-encoded ECDSA signature.
 * \param slot Logical slot index.
 * \param msg Message bytes.
 * \param msg_len Message length.
 * \param signature Output DER buffer.
 * \param sig_len Output DER length.
 * \return `true` on success.
 */
bool fido2_storage_sign_der(uint8_t slot, const uint8_t *msg, uint16_t msg_len,
                            uint8_t *signature, uint8_t *sig_len) {
    if (!slot_logical_valid(slot) || !g_storage.creds[slot].valid) {
        return false;
    }

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;

    uint8_t raw_sig[FIDO2_SIG_SIZE];
    size_t raw_len = sizeof(raw_sig);
    uint8_t phys_slot = ecc_slot_for_logical(slot);
    if (se->ecdsaSign(phys_slot, msg, msg_len, raw_sig, &raw_len) !=
            cdc::hal::SeResult::OK ||
        raw_len != FIDO2_SIG_SIZE) {
        LOG_E(TAG, "ECDSA sign failed for slot %d", slot);
        return false;
    }

    // Convert to DER format
    *sig_len = raw_sig_to_der(raw_sig, signature);

    LOG_D(TAG, "Signed DER %d bytes with slot %d, sig_len=%d", msg_len, slot, *sig_len);
    return true;
}

/**
 * \brief Reads public key from secure-element slot.
 * \param slot Logical slot index.
 * \param pubkey Output public-key buffer.
 * \return `true` on success.
 */
bool fido2_storage_get_pubkey(uint8_t slot, uint8_t *pubkey) {
    if (!slot_logical_valid(slot)) {
        return false;
    }

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) {
        return false;
    }

    uint8_t phys_slot = ecc_slot_for_logical(slot);
    if (!se->eccSlotUsed(phys_slot)) {
        return false;
    }

    cdc::hal::EccCurve curve = cdc::hal::EccCurve::P256;
    return se->eccGetPublicKey(phys_slot, pubkey, &curve) == cdc::hal::SeResult::OK;
}
