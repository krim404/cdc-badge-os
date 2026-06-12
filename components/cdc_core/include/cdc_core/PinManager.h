#pragma once

#include <cstdint>
#include <cstddef>

namespace cdc::core {

/**
 * PIN Manager - Manages all device PINs in TROPIC01 R-Memory Slot 0
 *
 * Storage Format (147 bytes):
 * [Magic 0xE0]           (1)  - Format identifier
 * [Badge/FIDO2 Hash]     (16) - LEFT(SHA256(PIN), 16)
 * [Badge Locked]         (1)  - 0x00 unlocked, 0x01 locked (recovery on next boot)
 * [KDF Algorithm]        (1)  - 0x03 = KDF_ITERSALTED_S2K
 * [Hash Algorithm]       (1)  - 0x08 = SHA256
 * [Iteration Count]      (4)  - Default 100000
 * [PW1 Salt]             (8)  - Random salt for User PIN
 * [PW3 Salt]             (8)  - Random salt for Admin PIN
 * [PW1 Hash]             (32) - KDF hash of User PIN
 * [PW3 Hash]             (32) - KDF hash of Admin PIN
 * [PW1 Retries]          (1)  - Remaining attempts (smartcard-style, no recovery)
 * [PW3 Retries]          (1)  - Remaining attempts (smartcard-style, no recovery)
 * [Duress Set]           (1)  - 0x00 not set, 0x01 set (self-destruct armed)
 * [Duress Salt]          (8)  - Random salt for duress PIN
 * [Duress Hash]          (32) - KDF hash of duress PIN
 *
 * The whole payload, including the duress fields, is covered by the slot-0
 * attestation signature, so tampering with the duress state invalidates the
 * record and forces a reset to defaults.
 *
 * Badge PIN: retry counter lives in RAM only. R-Memory persists just a binary
 * "locked" flag. Boot grants one attempt (or zero if locked) and starts the
 * 60-second recovery timer; on expiry the counter is restored to MAX_RETRIES
 * and the locked flag cleared. A crash mid-verify cannot brick the badge PIN.
 *
 * PW1/PW3: smartcard semantics. Pre-decrement is persisted synchronously
 * before the verify so a power-cycle cannot reset the counter, and reaching
 * zero is terminal until an admin reset.
 *
 * Defaults:
 * - Badge/FIDO2: "123456"
 * - OpenPGP PW1 (User): "123456" (min 6 digits)
 * - OpenPGP PW3 (Admin): "12345678" (min 8 digits)
 */
class PinManager {
public:
    // PIN constraints
    static constexpr uint8_t BADGE_PIN_MIN = 4;
    static constexpr uint8_t BADGE_PIN_MAX = 8;
    static constexpr uint8_t PW1_MIN = 6;
    static constexpr uint8_t PW3_MIN = 8;
    static constexpr uint8_t PIN_MAX = 16;

    // Storage
    static constexpr uint16_t RMEM_SLOT_PIN = 0;

    // Chip-bound attestation key in ECC slot 0 (managed by AttestationKeyService).
    // Used by saveToStorage / loadFromStorage to sign and verify the PIN payload
    // so a tampered or regenerated slot triggers a silent reset to defaults.
    static constexpr uint8_t ATTESTATION_ECC_SLOT = 0;

    // Hash sizes
    static constexpr uint8_t BADGE_HASH_SIZE = 16;  // LEFT(SHA256, 16)
    static constexpr uint8_t KDF_HASH_SIZE = 32;    // Full SHA256
    static constexpr uint8_t SALT_SIZE = 8;

    // KDF parameters (OpenPGP spec)
    static constexpr uint8_t KDF_ITERSALTED_S2K = 0x03;
    static constexpr uint8_t HASH_SHA256 = 0x08;
    static constexpr uint32_t DEFAULT_ITERATIONS = 100000;

    // Defaults
    static constexpr const char* DEFAULT_BADGE_PIN = "123456";
    static constexpr const char* DEFAULT_PW1 = "123456";
    static constexpr const char* DEFAULT_PW3 = "12345678";

    static PinManager& instance();
    bool init();

    // === Badge/FIDO2 PIN ===
    bool verifyBadgePin(const char* pin);
    bool changeBadgePin(const char* currentPin, const char* newPin);
    bool setBadgePin(const char* newPin);
    bool getBadgePinHash(uint8_t* hashOut) const;
    bool verifyBadgePinHash(const uint8_t* hashIn) const;
    uint8_t getBadgeRetries() const { return badgeRetries_; }
    bool isBadgeBlocked() const;  // Checks retries=0 OR time lockout active
    void resetBadgeRetries();

    // === Lockout Timer (RAM only, not persistent) ===
    static constexpr uint32_t LOCKOUT_DURATION_MS = 60000;  // 60 seconds
    void startLockout();
    uint32_t getLockoutRemainingMs() const;
    bool isLockoutActive() const;

    /**
     * \brief Clears expired lockout state and resets retry counter.
     *
     * Call this from non-const contexts to perform the lazy state update
     * that `isLockoutActive()` only observes.
     */
    void checkAndResetExpiredLockout();

    // === OpenPGP PW1 (User PIN) ===
    bool verifyPW1(const char* pin);
    bool changePW1(const char* currentPin, const char* newPin);
    bool setPW1(const char* newPin);
    bool getPW1Hash(uint8_t* hashOut) const;
    bool getPW1Salt(uint8_t* saltOut) const;
    uint8_t getPW1Retries() const { return pw1Retries_; }
    bool isPW1Blocked() const { return pw1Retries_ == 0; }
    void resetPW1Retries();

    // === OpenPGP PW3 (Admin PIN) ===
    bool verifyPW3(const char* pin);
    bool changePW3(const char* currentPin, const char* newPin);
    bool setPW3(const char* newPin);
    bool getPW3Hash(uint8_t* hashOut) const;
    bool getPW3Salt(uint8_t* saltOut) const;
    uint8_t getPW3Retries() const { return pw3Retries_; }
    bool isPW3Blocked() const { return pw3Retries_ == 0; }
    void resetPW3Retries();

    // === Duress / Self-Destruct PIN (optional, default NOT set) ===
    /**
     * \brief Sets the duress PIN, arming the self-destruct trigger.
     *
     * The duress PIN must be distinct from the current badge PIN so the
     * unlock path can tell them apart unambiguously. Setting it again
     * overwrites the previous duress PIN.
     *
     * \param pin Candidate duress PIN (4-8 digits).
     * \return `true` if set; `false` on invalid format or if equal to the
     *         current badge PIN.
     */
    bool setDuressPin(const char* pin);

    /**
     * \brief Clears the duress PIN, disarming the self-destruct trigger.
     * \return `true` if the record was updated.
     */
    bool clearDuressPin();

    /** \brief Returns whether a duress PIN is currently armed. */
    bool hasDuressPin() const { return duressSet_; }

    /**
     * \brief Constant-time check whether a candidate matches the duress PIN.
     * \param pin Candidate PIN string.
     * \return `true` if a duress PIN is set and the candidate matches it.
     */
    bool isDuressPin(const char* pin) const;

    // === KDF Parameters (for OpenPGP KDF-DO) ===
    uint8_t getKdfAlgorithm() const { return KDF_ITERSALTED_S2K; }
    uint8_t getHashAlgorithm() const { return HASH_SHA256; }
    uint32_t getIterationCount() const { return iterations_; }

    // === Status ===
    bool isPinSet() const { return badgePinIsSet_; }
    bool isStorageAvailable() const;

private:
    PinManager() = default;

    static constexpr uint8_t MAX_RETRIES = 3;
    static constexpr uint8_t MAGIC = 0xE0;
    static constexpr uint8_t SIGNATURE_SIZE = 64;        // P-256 ECDSA raw R||S
    static constexpr uint8_t PAYLOAD_SIZE = 147;
    // Stored buffer: [PAYLOAD_SIZE bytes payload][SIGNATURE_SIZE bytes ECDSA sig]
    static constexpr uint16_t STORAGE_SIZE = PAYLOAD_SIZE + SIGNATURE_SIZE;

    // Badge/FIDO2 (retry counter is RAM-only)
    uint8_t badgeHash_[BADGE_HASH_SIZE] = {};
    uint8_t badgeRetries_ = MAX_RETRIES;
    bool    badgeLocked_  = false;

    // OpenPGP KDF data
    uint32_t iterations_ = DEFAULT_ITERATIONS;
    uint8_t pw1Salt_[SALT_SIZE] = {};
    uint8_t pw3Salt_[SALT_SIZE] = {};
    uint8_t pw1Hash_[KDF_HASH_SIZE] = {};
    uint8_t pw3Hash_[KDF_HASH_SIZE] = {};
    uint8_t pw1Retries_ = MAX_RETRIES;
    uint8_t pw3Retries_ = MAX_RETRIES;

    // Duress / self-destruct PIN (optional, default not set). KDF-hashed with
    // the same machinery as PW1/PW3 and covered by the same attestation.
    bool    duressSet_ = false;
    uint8_t duressSalt_[SALT_SIZE] = {};
    uint8_t duressHash_[KDF_HASH_SIZE] = {};

    // Mirrors of what is currently persisted in R-Memory. Updated by
    // saveToStorage() after a successful write. Used to skip redundant
    // writes when the in-RAM state already matches the on-chip value.
    bool    persistedBadgeLocked_ = false;
    uint8_t persistedPw1Retries_  = MAX_RETRIES;
    uint8_t persistedPw3Retries_  = MAX_RETRIES;

    bool pinLoaded_ = false;
    bool badgePinIsSet_ = false;

    // Badge recovery timer (RAM only). Runs from boot and after every
    // transition of badgeRetries_ to zero. On expiry: badgeRetries_ is
    // restored to MAX_RETRIES and badgeLocked_ is cleared (and persisted
    // if it was set).
    uint32_t lockoutStartMs_ = 0;
    bool lockoutActive_ = false;

    bool loadFromStorage();
    bool saveToStorage();

    // Badge hash: LEFT(SHA256(PIN), 16)
    bool computeBadgeHash(const char* pin, uint8_t* hashOut);

    // OpenPGP KDF hash: SHA256 iterated with salt
    bool computeKdfHash(const char* pin, const uint8_t* salt, uint8_t* hashOut) const;

    bool compareHash(const uint8_t* h1, const uint8_t* h2, size_t len) const;
    void generateSalt(uint8_t* salt);
    void loadDefaults();

    /**
     * \brief Identifies the PIN slot operated on by `verifyPin()`.
     */
    enum class PinSlot : uint8_t {
        BADGE,
        PW1,
        PW3
    };

    /**
     * \brief Unified PIN verification routine handling counters and lockout.
     * \param slot Target PIN slot.
     * \param pin Candidate PIN string.
     * \return `true` if PIN matches the stored hash.
     */
    bool verifyPin(PinSlot slot, const char* pin);
};

} // namespace cdc::core
