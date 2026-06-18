/**
 * PinManager Implementation
 *
 * PIN storage in TROPIC01 R-Memory Slot 0
 * Combined format for Badge/FIDO2 and OpenPGP PINs
 */

#include "cdc_core/PinManager.h"
#include "cdc_core/feature_flags.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_log.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"
#include "mbedtls/platform_util.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <cstring>

static const char* TAG = "PinManager";

/** \brief Size of a SHA-256 digest in bytes (FIPS 180-4). */
static constexpr size_t SHA256_DIGEST_SIZE = 32;

namespace cdc::core {

/**
 * \brief Returns singleton PIN manager instance.
 * \return Singleton reference.
 */
PinManager& PinManager::instance() {
    static PinManager instance;
    return instance;
}

/**
 * \brief Initializes PIN state from secure storage or defaults.
 * \return `true` if state is ready for use.
 */
bool PinManager::init() {
    if (pinLoaded_) return true;

    if (!loadFromStorage()) {
        LOG_I(TAG, "Loading default PINs (storage empty or unreadable)");
        loadDefaults();
        saveToStorage();
    }
    pinLoaded_ = true;

    badgeRetries_ = badgeLocked_ ? 0 : 1;
    startLockout();
    LOG_I(TAG, "Badge state after init: locked=%d retries=%u pinSet=%d",
          badgeLocked_, badgeRetries_, badgePinIsSet_);
    return true;
}

/**
 * \brief Loads default badge and OpenPGP PIN material.
 */
void PinManager::loadDefaults() {
    // Badge/FIDO2 hash
    computeBadgeHash(DEFAULT_BADGE_PIN, badgeHash_);
    badgeRetries_ = MAX_RETRIES;
    badgeLocked_ = false;

    // Generate random salts
    generateSalt(pw1Salt_);
    generateSalt(pw3Salt_);

    // Compute KDF hashes with salts
    computeKdfHash(DEFAULT_PW1, pw1Salt_, pw1Hash_);
    computeKdfHash(DEFAULT_PW3, pw3Salt_, pw3Hash_);

    iterations_ = DEFAULT_ITERATIONS;
    pw1Retries_ = MAX_RETRIES;
    pw3Retries_ = MAX_RETRIES;
    badgePinIsSet_ = false;

    // Duress PIN is opt-in: defaults leave it disarmed.
    duressSet_ = false;
    memset(duressSalt_, 0, sizeof(duressSalt_));
    memset(duressHash_, 0, sizeof(duressHash_));

    LOG_I(TAG, "Loaded default PINs");
}

/**
 * \brief Fills a salt buffer using secure-element RNG or ESP fallback RNG.
 * \param salt Output salt buffer.
 */
void PinManager::generateSalt(uint8_t* salt) {
    // Try to get random from SE, fallback to ESP random
    hal::ISecureElement* se = hal::getSecureElementInstance();
    if (se && se->isSessionActive() && se->getRandom(salt, SALT_SIZE)) {
        return;
    }
    // Fallback to ESP32 RNG
    esp_fill_random(salt, SALT_SIZE);
}

/**
 * \brief Returns whether secure storage access is currently available.
 * \return `true` when secure-element session is active.
 */
bool PinManager::isStorageAvailable() const {
    hal::ISecureElement* se = hal::getSecureElementInstance();
    return se && se->isSessionActive();
}

/**
 * \brief Loads serialized PIN/KDF state from secure-element R-Memory.
 * \return `true` on successful load and format validation.
 */
/**
 * \brief Verifies the ECDSA-P256 attestation signature appended to a stored
 *        PIN payload. The signature is produced over the first PAYLOAD_SIZE
 *        bytes with the chip-bound key in ECC slot 0 (AttestationKeyService).
 *
 * If the chip-bound public key has changed (slot 0 was regenerated, e.g. by
 * an attacker who managed to rewrite that slot via the pairing key), the
 * verification will fail and the caller will trigger a re-init with fresh
 * defaults. The signature itself uses random-k ECDSA, so re-saving the same
 * payload produces a different signature — that is fine, only verification
 * matters here.
 */
static bool verify_payload_signature(hal::ISecureElement* se,
                                     const uint8_t* payload, size_t payload_len,
                                     const uint8_t* sig, size_t sig_len) {
    if (sig_len != 64) return false;
    uint8_t pub_raw[64];
    hal::EccCurve curve = hal::EccCurve::P256;
    if (se->eccGetPublicKey(PinManager::ATTESTATION_ECC_SLOT, pub_raw, &curve) != hal::SeResult::OK) {
        LOG_W(TAG, "Attestation pubkey read failed");
        return false;
    }
    if (curve != hal::EccCurve::P256) {
        LOG_W(TAG, "Attestation key is not P-256");
        return false;
    }

    uint8_t pub_sec1[65];
    pub_sec1[0] = 0x04;
    memcpy(pub_sec1 + 1, pub_raw, 64);

    uint8_t hash[SHA256_DIGEST_SIZE];
    mbedtls_sha256(payload, payload_len, hash, 0);

    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
        if (mbedtls_ecp_point_read_binary(&grp, &Q, pub_sec1, sizeof(pub_sec1)) != 0) break;
        if (mbedtls_mpi_read_binary(&r, sig + 0, 32) != 0) break;
        if (mbedtls_mpi_read_binary(&s, sig + 32, 32) != 0) break;
        ok = (mbedtls_ecdsa_verify(&grp, hash, SHA256_DIGEST_SIZE, &Q, &r, &s) == 0);
    } while (0);

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool PinManager::loadFromStorage() {
    hal::ISecureElement* se = hal::getSecureElementInstance();
    if (!se || !se->isSessionActive()) {
        LOG_W(TAG, "SE session not active");
        return false;
    }

    uint8_t data[STORAGE_SIZE];
    uint16_t actualLen = 0;

    hal::SeResult result = se->rmemRead(RMEM_SLOT_PIN, data, STORAGE_SIZE, &actualLen);
    if (result != hal::SeResult::OK) {
        LOG_D(TAG, "No PIN data in R-Memory (read err=%d)",
              static_cast<int>(result));
        return false;
    }

    // Only the signed format is accepted. Any other state (wrong magic,
    // wrong length, or invalid signature) falls back to defaults so the
    // slot ends up freshly signed by the chip-bound attestation key.
    if (actualLen != STORAGE_SIZE || data[0] != MAGIC) {
        LOG_W(TAG, "PIN storage unrecognised (len=%u magic=0x%02X) - using defaults",
              actualLen, actualLen > 0 ? data[0] : 0);
        return false;
    }
    if (!verify_payload_signature(se, data, PAYLOAD_SIZE,
                                  data + PAYLOAD_SIZE, SIGNATURE_SIZE)) {
        LOG_W(TAG, "PIN storage signature invalid - re-initializing");
        return false;
    }

    size_t pos = 1;

    // Badge hash
    memcpy(badgeHash_, &data[pos], BADGE_HASH_SIZE);
    pos += BADGE_HASH_SIZE;

    // Badge locked flag (counter itself is RAM-only)
    badgeLocked_ = (data[pos++] != 0);

    // KDF params (skip algorithm bytes, we know them)
    pos += 2;  // KDF algo + Hash algo

    // Iteration count (big endian)
    iterations_ = (data[pos] << 24) | (data[pos+1] << 16) | (data[pos+2] << 8) | data[pos+3];
    pos += 4;

    // Salts
    memcpy(pw1Salt_, &data[pos], SALT_SIZE);
    pos += SALT_SIZE;
    memcpy(pw3Salt_, &data[pos], SALT_SIZE);
    pos += SALT_SIZE;

    // Hashes
    memcpy(pw1Hash_, &data[pos], KDF_HASH_SIZE);
    pos += KDF_HASH_SIZE;
    memcpy(pw3Hash_, &data[pos], KDF_HASH_SIZE);
    pos += KDF_HASH_SIZE;

    // Retries
    pw1Retries_ = data[pos++];
    pw3Retries_ = data[pos++];

    // Duress / self-destruct PIN
    duressSet_ = (data[pos++] != 0);
    memcpy(duressSalt_, &data[pos], SALT_SIZE);
    pos += SALT_SIZE;
    memcpy(duressHash_, &data[pos], KDF_HASH_SIZE);
    pos += KDF_HASH_SIZE;

    // Mirror starts in sync with whatever is on the chip.
    persistedBadgeLocked_ = badgeLocked_;
    persistedPw1Retries_  = pw1Retries_;
    persistedPw3Retries_  = pw3Retries_;

    // Check if badge PIN differs from default
    uint8_t defaultHash[BADGE_HASH_SIZE];
    computeBadgeHash(DEFAULT_BADGE_PIN, defaultHash);
    badgePinIsSet_ = !compareHash(badgeHash_, defaultHash, BADGE_HASH_SIZE);

    LOG_I(TAG, "Loaded PINs from R-Memory (Badge locked=%s, PW1=%d, PW3=%d retries)",
          badgeLocked_ ? "yes" : "no", pw1Retries_, pw3Retries_);
    return true;
}

/**
 * \brief Persists current PIN/KDF state into secure-element R-Memory.
 * \return `true` if write succeeded.
 */
bool PinManager::saveToStorage() {
    hal::ISecureElement* se = hal::getSecureElementInstance();
    if (!se || !se->isSessionActive()) {
        LOG_E(TAG, "SE session not active");
        return false;
    }

    uint8_t data[STORAGE_SIZE];
    size_t pos = 0;

    data[pos++] = MAGIC;

    // Badge hash
    memcpy(&data[pos], badgeHash_, BADGE_HASH_SIZE);
    pos += BADGE_HASH_SIZE;

    // Badge locked flag (retry counter is RAM-only)
    data[pos++] = badgeLocked_ ? 0x01 : 0x00;

    // KDF params
    data[pos++] = KDF_ITERSALTED_S2K;
    data[pos++] = HASH_SHA256;

    // Iteration count (big endian)
    data[pos++] = (iterations_ >> 24) & 0xFF;
    data[pos++] = (iterations_ >> 16) & 0xFF;
    data[pos++] = (iterations_ >> 8) & 0xFF;
    data[pos++] = iterations_ & 0xFF;

    // Salts
    memcpy(&data[pos], pw1Salt_, SALT_SIZE);
    pos += SALT_SIZE;
    memcpy(&data[pos], pw3Salt_, SALT_SIZE);
    pos += SALT_SIZE;

    // Hashes
    memcpy(&data[pos], pw1Hash_, KDF_HASH_SIZE);
    pos += KDF_HASH_SIZE;
    memcpy(&data[pos], pw3Hash_, KDF_HASH_SIZE);
    pos += KDF_HASH_SIZE;

    // Retries
    data[pos++] = pw1Retries_;
    data[pos++] = pw3Retries_;

    // Duress / self-destruct PIN
    data[pos++] = duressSet_ ? 0x01 : 0x00;
    memcpy(&data[pos], duressSalt_, SALT_SIZE);
    pos += SALT_SIZE;
    memcpy(&data[pos], duressHash_, KDF_HASH_SIZE);
    pos += KDF_HASH_SIZE;

    // pos must now equal PAYLOAD_SIZE — append a P-256 ECDSA signature over
    // bytes [0..PAYLOAD_SIZE) using the chip-bound attestation key in slot 0.
    // A subsequent load that finds the signature invalid (because the slot 0
    // key was regenerated or the payload was tampered with) will silently
    // re-initialize the storage with defaults, exactly as requested by spec.
    if (pos != PAYLOAD_SIZE) {
        LOG_E(TAG, "Payload size mismatch (built=%zu, expected=%u)", pos, PAYLOAD_SIZE);
        return false;
    }
    size_t sig_len = SIGNATURE_SIZE;
    hal::SeResult sign_res = se->ecdsaSign(ATTESTATION_ECC_SLOT,
                                           data, PAYLOAD_SIZE,
                                           data + PAYLOAD_SIZE, &sig_len);
    if (sign_res != hal::SeResult::OK || sig_len != SIGNATURE_SIZE) {
        LOG_E(TAG, "Attestation sign failed (%d)", static_cast<int>(sign_res));
        return false;
    }

    se->rmemErase(RMEM_SLOT_PIN);

    hal::SeResult result = se->rmemWrite(RMEM_SLOT_PIN, data, STORAGE_SIZE);
    if (result != hal::SeResult::OK) {
        LOG_E(TAG, "R-Memory write failed");
        return false;
    }

    persistedBadgeLocked_ = badgeLocked_;
    persistedPw1Retries_  = pw1Retries_;
    persistedPw3Retries_  = pw3Retries_;

    LOG_D(TAG, "PINs saved to R-Memory slot %d (signed, %u bytes)",
          RMEM_SLOT_PIN, STORAGE_SIZE);
    return true;
}

/**
 * \brief Computes truncated SHA-256 badge PIN hash.
 * \param pin Input badge PIN.
 * \param hashOut Output hash buffer.
 * \return `true` on success.
 */
bool PinManager::computeBadgeHash(const char* pin, uint8_t* hashOut) {
    if (!pin || !hashOut) return false;

    uint8_t fullHash[SHA256_DIGEST_SIZE];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t*)pin, strlen(pin));
    mbedtls_sha256_finish(&ctx, fullHash);
    mbedtls_sha256_free(&ctx);

    memcpy(hashOut, fullHash, BADGE_HASH_SIZE);
    return true;
}

/**
 * \brief Computes Iterated+Salted S2K SHA-256 hash for OpenPGP PINs.
 * \param pin Input PIN text.
 * \param salt Salt bytes.
 * \param hashOut Output hash buffer.
 * \return `true` on success.
 */
bool PinManager::computeKdfHash(const char* pin, const uint8_t* salt, uint8_t* hashOut) const {
    if (!pin) return false;
    return computeKdfHash(reinterpret_cast<const uint8_t*>(pin), strlen(pin), salt, hashOut);
}

bool PinManager::computeKdfHash(const uint8_t* data, size_t len, const uint8_t* salt,
                                uint8_t* hashOut) const {
    if (!data || !salt || !hashOut) return false;
    // Salt + input must fit the iteration buffer; the KDF-DO path supplies a
    // pre-hash of up to 64 bytes, the cleartext path a PIN of up to PIN_MAX.
    if (len > 64) return false;

    // OpenPGP Iterated+Salted S2K (RFC 4880): hash iteration-count bytes of
    // (salt + input) repeated.
    size_t combined = SALT_SIZE + len;
    size_t totalBytes = iterations_;

    uint8_t buffer[SALT_SIZE + 64];
    memcpy(buffer, salt, SALT_SIZE);
    memcpy(buffer + SALT_SIZE, data, len);

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    size_t processed = 0;
    while (processed < totalBytes) {
        size_t chunk = (totalBytes - processed < combined) ? (totalBytes - processed) : combined;
        mbedtls_sha256_update(&ctx, buffer, chunk);
        processed += chunk;
    }

    mbedtls_sha256_finish(&ctx, hashOut);
    mbedtls_sha256_free(&ctx);
    mbedtls_platform_zeroize(buffer, sizeof(buffer));

    return true;
}

/**
 * \brief Compares hash buffers in constant-time style.
 * \param h1 First hash buffer.
 * \param h2 Second hash buffer.
 * \param len Number of bytes to compare.
 * \return `true` when buffers are equal.
 */
bool PinManager::compareHash(const uint8_t* h1, const uint8_t* h2, size_t len) const {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= h1[i] ^ h2[i];
    }
    return diff == 0;
}

/**
 * \brief Badge/FIDO2 PIN workflow.
 */

/**
 * \brief Unified PIN verification routine for Badge, PW1, and PW3 slots.
 *
 * Centralizes retry-counter management, lockout handling, and persistence so
 * the per-slot wrappers stay thin. Hash computation and storage routing depend
 * on the requested slot.
 *
 * \param slot Target PIN slot.
 * \param pin Candidate PIN string.
 * \return `true` if PIN matches the stored hash for the requested slot.
 */
bool PinManager::verifyPin(PinSlot slot, const char* pin) {
    if (!pin) return false;
    if (!pinLoaded_) init();

    if (slot == PinSlot::BADGE) {
        checkAndResetExpiredLockout();
        if (badgeRetries_ == 0) {
            LOG_W(TAG, "Badge PIN blocked");
            return false;
        }
        uint8_t inputHash[BADGE_HASH_SIZE];
        if (!computeBadgeHash(pin, inputHash)) return false;

        badgeRetries_--;

        if (compareHash(badgeHash_, inputHash, BADGE_HASH_SIZE)) {
            badgeRetries_ = MAX_RETRIES;
            lockoutActive_ = false;
            if (persistedBadgeLocked_) {
                badgeLocked_ = false;
                saveToStorage();
            }
            LOG_I(TAG, "Badge PIN verified");
            return true;
        }

        LOG_W(TAG, "Wrong Badge PIN, %d retries left", badgeRetries_);
        if (badgeRetries_ == 0) {
            badgeLocked_ = true;
            saveToStorage();
            startLockout();
        }
        return false;
    }

    // PW1/PW3 use the binary-capable path; the cleartext PIN is just its bytes.
    return verifyPinRaw(slot, reinterpret_cast<const uint8_t*>(pin), strlen(pin));
}

bool PinManager::verifyPinRaw(PinSlot slot, const uint8_t* data, size_t len) {
    if (!data) return false;
    if (!pinLoaded_) init();

    // PW1/PW3: smartcard semantics. Pre-decrement is persisted synchronously
    // before the verify so a power-cycle between hash and persist cannot
    // resurrect the counter. Reaching zero is terminal until an admin reset.
    const char* label = nullptr;
    uint8_t* retries = nullptr;
    uint8_t* storedHash = nullptr;
    uint8_t* salt = nullptr;
    uint8_t* mirror = nullptr;

    switch (slot) {
        case PinSlot::PW1:
            label = "PW1";
            retries = &pw1Retries_;
            storedHash = pw1Hash_;
            salt = pw1Salt_;
            mirror = &persistedPw1Retries_;
            break;
        case PinSlot::PW3:
            label = "PW3";
            retries = &pw3Retries_;
            storedHash = pw3Hash_;
            salt = pw3Salt_;
            mirror = &persistedPw3Retries_;
            break;
        case PinSlot::BADGE:
            return false;  // unreachable
    }

    if (*retries == 0) {
        LOG_W(TAG, "%s blocked", label);
        return false;
    }

    uint8_t inputHash[KDF_HASH_SIZE];
    if (!computeKdfHash(data, len, salt, inputHash)) return false;

    const uint8_t before = *retries;
    (*retries)--;
    if (*retries < *mirror) {
        if (!saveToStorage()) {
            *retries = before;
            return false;
        }
    }

    if (compareHash(storedHash, inputHash, KDF_HASH_SIZE)) {
        *retries = MAX_RETRIES;
        if (*mirror != MAX_RETRIES) {
            saveToStorage();
        }
        LOG_I(TAG, "%s verified", label);
        return true;
    }

    LOG_W(TAG, "Wrong %s, %d retries left", label, *retries);
    return false;
}

/**
 * \brief Verifies badge PIN, updates retries, and handles lockout transitions.
 * \param pin Candidate badge PIN.
 * \return `true` if PIN is valid.
 */
bool PinManager::verifyBadgePin(const char* pin) {
    return verifyPin(PinSlot::BADGE, pin);
}

/**
 * \brief Changes badge PIN after validating current PIN.
 * \param currentPin Current PIN.
 * \param newPin New PIN.
 * \return `true` if change succeeded.
 */
bool PinManager::changeBadgePin(const char* currentPin, const char* newPin) {
    if (!verifyBadgePin(currentPin)) return false;
    return setBadgePin(newPin);
}

/**
 * \brief Sets badge PIN directly with format validation.
 * \param newPin New PIN value.
 * \return `true` if update succeeded.
 */
void PinManager::setMinPinLengthFloor(uint8_t minLen) {
    if (minLen > BADGE_PIN_MAX) minLen = BADGE_PIN_MAX;
    if (minLen < BADGE_PIN_MIN) minLen = BADGE_PIN_MIN;
    minPinFloor_ = minLen;
}

bool PinManager::setBadgePin(const char* newPin) {
    if (!newPin) return false;
    size_t len = strlen(newPin);
    uint8_t minLen = minPinFloor_ > BADGE_PIN_MIN ? minPinFloor_ : BADGE_PIN_MIN;
    if (len < minLen || len > BADGE_PIN_MAX) {
        LOG_E(TAG, "Badge PIN must be %d-%d digits", minLen, BADGE_PIN_MAX);
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (newPin[i] < '0' || newPin[i] > '9') {
            LOG_E(TAG, "PIN must contain only digits");
            return false;
        }
    }

    if (duressSet_ && isDuressPin(newPin)) {
        LOG_E(TAG, "Badge PIN must differ from duress PIN");
        return false;
    }

    computeBadgeHash(newPin, badgeHash_);
    badgeRetries_ = MAX_RETRIES;
    badgeLocked_ = false;
    lockoutActive_ = false;

    uint8_t defaultHash[BADGE_HASH_SIZE];
    computeBadgeHash(DEFAULT_BADGE_PIN, defaultHash);
    badgePinIsSet_ = !compareHash(badgeHash_, defaultHash, BADGE_HASH_SIZE);

    saveToStorage();
    LOG_I(TAG, "Badge PIN changed");
    return true;
}

/**
 * \brief Resets badge retry counter to maximum.
 */
void PinManager::resetBadgeRetries() {
    badgeRetries_ = MAX_RETRIES;
    lockoutActive_ = false;
    if (badgeLocked_) {
        badgeLocked_ = false;
        saveToStorage();
    }
}

/**
 * \brief Copies stored badge PIN hash into caller buffer.
 * \param hashOut Output hash buffer.
 * \return `true` if hash was copied.
 */
bool PinManager::getBadgePinHash(uint8_t* hashOut) const {
    if (!hashOut) return false;
    memcpy(hashOut, badgeHash_, BADGE_HASH_SIZE);
    return true;
}

/**
 * \brief Verifies provided hash against stored badge hash.
 * \param hashIn Candidate hash buffer.
 * \return `true` if hashes match.
 */
bool PinManager::verifyBadgePinHash(const uint8_t* hashIn) const {
    if (!hashIn) return false;
    return compareHash(badgeHash_, hashIn, BADGE_HASH_SIZE);
}

/**
 * \brief Duress / self-destruct PIN workflow.
 */

/**
 * \brief Sets the duress PIN, arming the self-destruct trigger.
 *
 * Reuses the PW1/PW3 KDF/salt machinery. The candidate must satisfy the badge
 * PIN format and must differ from the current badge PIN so the unlock path can
 * distinguish a duress entry from a normal unlock.
 *
 * \param pin Candidate duress PIN.
 * \return `true` if set; `false` on invalid format or equality with the badge PIN.
 */
bool PinManager::setDuressPin(const char* pin) {
    if (!pin) return false;
    if (!pinLoaded_) init();

    size_t len = strlen(pin);
    if (len < BADGE_PIN_MIN || len > BADGE_PIN_MAX) {
        LOG_E(TAG, "Duress PIN must be %d-%d digits", BADGE_PIN_MIN, BADGE_PIN_MAX);
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            LOG_E(TAG, "Duress PIN must contain only digits");
            return false;
        }
    }

    // Must be distinct from the badge PIN: an ambiguous match would make the
    // unlock outcome non-deterministic.
    uint8_t candidateBadgeHash[BADGE_HASH_SIZE];
    if (!computeBadgeHash(pin, candidateBadgeHash)) return false;
    if (compareHash(badgeHash_, candidateBadgeHash, BADGE_HASH_SIZE)) {
        LOG_E(TAG, "Duress PIN must differ from badge PIN");
        return false;
    }

    generateSalt(duressSalt_);
    if (!computeKdfHash(pin, duressSalt_, duressHash_)) return false;
    duressSet_ = true;

    saveToStorage();
    LOG_I(TAG, "Duress PIN set");
    return true;
}

/**
 * \brief Clears the duress PIN, disarming the self-destruct trigger.
 * \return `true` if the record was updated.
 */
bool PinManager::clearDuressPin() {
    if (!pinLoaded_) init();
    if (!duressSet_) return true;

    duressSet_ = false;
    memset(duressSalt_, 0, sizeof(duressSalt_));
    memset(duressHash_, 0, sizeof(duressHash_));

    saveToStorage();
    LOG_I(TAG, "Duress PIN cleared");
    return true;
}

/**
 * \brief Constant-time check whether a candidate matches the duress PIN.
 * \param pin Candidate PIN string.
 * \return `true` if a duress PIN is armed and the candidate matches it.
 */
bool PinManager::isDuressPin(const char* pin) const {
    if (!duressSet_ || !pin) return false;
    size_t len = strlen(pin);
    if (len < BADGE_PIN_MIN || len > BADGE_PIN_MAX) return false;

    uint8_t inputHash[KDF_HASH_SIZE];
    if (!computeKdfHash(pin, duressSalt_, inputHash)) {
        return false;
    }
    return compareHash(duressHash_, inputHash, KDF_HASH_SIZE);
}

/**
 * \brief OpenPGP PW1 (user PIN) workflow.
 */

/**
 * \brief Verifies OpenPGP PW1 and updates retry counters.
 * \param pin Candidate PW1 value.
 * \return `true` if PW1 is valid.
 */
bool PinManager::verifyPW1(const char* pin) {
    return verifyPin(PinSlot::PW1, pin);
}

/**
 * \brief Changes PW1 after validating the current value.
 * \param currentPin Current PW1 value.
 * \param newPin New PW1 value.
 * \return `true` if change succeeded.
 */
bool PinManager::changePW1(const char* currentPin, const char* newPin) {
    if (!verifyPW1(currentPin)) return false;
    return setPW1(newPin);
}

/**
 * \brief Sets PW1 directly and refreshes salt/hash material.
 * \param newPin New PW1 value.
 * \return `true` if update succeeded.
 */
bool PinManager::setPW1(const char* newPin) {
    if (!newPin) return false;
    size_t len = strlen(newPin);
    if (len < PW1_MIN || len > PIN_MAX) {
        LOG_E(TAG, "PW1 must be %d-%d digits", PW1_MIN, PIN_MAX);
        return false;
    }

    // Generate new salt
    generateSalt(pw1Salt_);
    computeKdfHash(newPin, pw1Salt_, pw1Hash_);
    pw1Retries_ = MAX_RETRIES;

    saveToStorage();
    LOG_I(TAG, "PW1 changed");
    return true;
}

bool PinManager::verifyPW1Raw(const uint8_t* data, size_t len) {
    return verifyPinRaw(PinSlot::PW1, data, len);
}

bool PinManager::setPW1Raw(const uint8_t* data, size_t len) {
    if (!data || (len != 32 && len != 64)) return false;
    generateSalt(pw1Salt_);
    if (!computeKdfHash(data, len, pw1Salt_, pw1Hash_)) return false;
    pw1Retries_ = MAX_RETRIES;
    saveToStorage();
    LOG_I(TAG, "PW1 set from KDF reference");
    return true;
}

/**
 * \brief Copies stored PW1 hash into caller buffer.
 * \param hashOut Output hash buffer.
 * \return `true` if copied.
 */
bool PinManager::getPW1Hash(uint8_t* hashOut) const {
    if (!hashOut) return false;
    memcpy(hashOut, pw1Hash_, KDF_HASH_SIZE);
    return true;
}

/**
 * \brief Copies stored PW1 salt into caller buffer.
 * \param saltOut Output salt buffer.
 * \return `true` if copied.
 */
bool PinManager::getPW1Salt(uint8_t* saltOut) const {
    if (!saltOut) return false;
    memcpy(saltOut, pw1Salt_, SALT_SIZE);
    return true;
}

/**
 * \brief Resets PW1 retry counter to maximum.
 */
void PinManager::resetPW1Retries() {
    if (pw1Retries_ < MAX_RETRIES) {
        pw1Retries_ = MAX_RETRIES;
        saveToStorage();
    }
}

/**
 * \brief OpenPGP PW3 (admin PIN) workflow.
 */

/**
 * \brief Verifies OpenPGP PW3 and updates retry counters.
 * \param pin Candidate PW3 value.
 * \return `true` if PW3 is valid.
 */
bool PinManager::verifyPW3(const char* pin) {
    return verifyPin(PinSlot::PW3, pin);
}

/**
 * \brief Changes PW3 after validating the current value.
 * \param currentPin Current PW3 value.
 * \param newPin New PW3 value.
 * \return `true` if change succeeded.
 */
bool PinManager::changePW3(const char* currentPin, const char* newPin) {
    if (!verifyPW3(currentPin)) return false;
    return setPW3(newPin);
}

/**
 * \brief Sets PW3 directly and refreshes salt/hash material.
 * \param newPin New PW3 value.
 * \return `true` if update succeeded.
 */
bool PinManager::setPW3(const char* newPin) {
    if (!newPin) return false;
    size_t len = strlen(newPin);
    if (len < PW3_MIN || len > PIN_MAX) {
        LOG_E(TAG, "PW3 must be %d-%d digits", PW3_MIN, PIN_MAX);
        return false;
    }

    generateSalt(pw3Salt_);
    computeKdfHash(newPin, pw3Salt_, pw3Hash_);
    pw3Retries_ = MAX_RETRIES;

    saveToStorage();
    LOG_I(TAG, "PW3 changed");
    return true;
}

bool PinManager::verifyPW3Raw(const uint8_t* data, size_t len) {
    return verifyPinRaw(PinSlot::PW3, data, len);
}

bool PinManager::setPW3Raw(const uint8_t* data, size_t len) {
    if (!data || (len != 32 && len != 64)) return false;
    generateSalt(pw3Salt_);
    if (!computeKdfHash(data, len, pw3Salt_, pw3Hash_)) return false;
    pw3Retries_ = MAX_RETRIES;
    saveToStorage();
    LOG_I(TAG, "PW3 set from KDF reference");
    return true;
}

/**
 * \brief Copies stored PW3 hash into caller buffer.
 * \param hashOut Output hash buffer.
 * \return `true` if copied.
 */
bool PinManager::getPW3Hash(uint8_t* hashOut) const {
    if (!hashOut) return false;
    memcpy(hashOut, pw3Hash_, KDF_HASH_SIZE);
    return true;
}

/**
 * \brief Copies stored PW3 salt into caller buffer.
 * \param saltOut Output salt buffer.
 * \return `true` if copied.
 */
bool PinManager::getPW3Salt(uint8_t* saltOut) const {
    if (!saltOut) return false;
    memcpy(saltOut, pw3Salt_, SALT_SIZE);
    return true;
}

/**
 * \brief Resets PW3 retry counter to maximum.
 */
void PinManager::resetPW3Retries() {
    if (pw3Retries_ < MAX_RETRIES) {
        pw3Retries_ = MAX_RETRIES;
        saveToStorage();
    }
}

/**
 * \brief Lockout timer handling.
 */

/**
 * \brief Returns whether badge PIN entry is currently blocked by lockout.
 * \return `true` if blocked.
 */
bool PinManager::isBadgeBlocked() const {
    return badgeRetries_ == 0;
}

/**
 * \brief Starts the badge recovery timer.
 */
void PinManager::startLockout() {
    lockoutStartMs_ = esp_timer_get_time() / 1000;
    lockoutActive_ = true;
    LOG_I(TAG, "Badge recovery timer started (%lu ms)", LOCKOUT_DURATION_MS);
}

/**
 * \brief Returns remaining badge lockout duration.
 * \return Remaining lockout time in milliseconds.
 */
uint32_t PinManager::getLockoutRemainingMs() const {
    if (!lockoutActive_) {
        return 0;
    }

    uint32_t nowMs = esp_timer_get_time() / 1000;
    uint32_t elapsed = nowMs - lockoutStartMs_;

    if (elapsed >= LOCKOUT_DURATION_MS) {
        return 0;
    }
    return LOCKOUT_DURATION_MS - elapsed;
}

/**
 * \brief Returns whether lockout is currently active without mutating state.
 * \return `true` if lockout is still active.
 */
bool PinManager::isLockoutActive() const {
    if (!lockoutActive_) {
        return false;
    }
    return getLockoutRemainingMs() > 0;
}

/**
 * \brief Clears expired lockout state and persists updated retry counter.
 *
 * This is the non-const counterpart to `isLockoutActive()`. Callers in
 * non-const contexts use this to perform the lazy state update without
 * needing `const_cast`.
 */
void PinManager::checkAndResetExpiredLockout() {
    if (!lockoutActive_) return;
    if (getLockoutRemainingMs() > 0) return;

    lockoutActive_ = false;
    badgeRetries_ = MAX_RETRIES;
    if (badgeLocked_) {
        badgeLocked_ = false;
        saveToStorage();
    }
    LOG_I(TAG, "Badge recovery timer expired, retries restored to %u", MAX_RETRIES);
}

} // namespace cdc::core
