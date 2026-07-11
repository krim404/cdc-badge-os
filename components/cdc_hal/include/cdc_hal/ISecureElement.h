#pragma once

#include "cdc_core/IService.h"
#include <cstdint>
#include <cstddef>

namespace cdc::hal {

/**
 * ECC curve types
 */
enum class EccCurve : uint8_t {
    P256,       // NIST P-256 (secp256r1)
    ED25519     // Ed25519
};

/**
 * \brief Maps an EccCurve to the module-level curve byte.
 * \param c Curve identifier.
 * \return `0` for Ed25519, `1` for P-256.
 */
inline uint8_t curveByte(EccCurve c) {
    return (c == EccCurve::ED25519) ? 0 : 1;
}

/**
 * \brief Maps a curve byte to its EccCurve enum.
 * \param b Curve byte (`0` Ed25519, otherwise P-256).
 * \return Corresponding EccCurve value.
 */
inline EccCurve curveFromByte(uint8_t b) {
    return (b == 0) ? EccCurve::ED25519 : EccCurve::P256;
}

/**
 * Secure Element operation result
 */
enum class SeResult : uint8_t {
    OK,                 // Success
    ERROR,              // Generic error
    SESSION_REQUIRED,   // No active session
    SLOT_EMPTY,         // Key slot is empty
    SLOT_OCCUPIED,      // Slot already has a key
    INVALID_PARAM,      // Invalid parameter
    ALARM_MODE,         // Chip in alarm mode (tamper detected)
    NOT_SUPPORTED       // Operation not supported
};

/**
 * Secure Element interface (TROPIC01)
 *
 * Provides:
 * - ECC key storage (32 slots)
 * - ECDSA/EdDSA signing
 * - R-Memory storage (512 slots, 476 bytes each)
 * - Hardware TRNG
 */
class ISecureElement : public core::IService {
public:
    // Slot limits
    static constexpr uint8_t ECC_SLOT_COUNT = 32;
    static constexpr uint16_t RMEM_SLOT_COUNT = 512;
    // Minimum guaranteed slot size across all supported TROPIC01 Application
    // FW versions. Use for static layouts that must remain stable when the
    // chip is reflashed with older FW; query getRmemSlotSize() for the actual
    // runtime capability.
    static constexpr uint16_t RMEM_SLOT_SIZE = 444;
    // Stack-buffer ceiling sized for the largest known FW (>= 2.0 reports 475).
    static constexpr uint16_t RMEM_SLOT_SIZE_MAX = 475;
    static constexpr uint8_t RMEM_NAME_LEN = 16;

    virtual ~ISecureElement() = default;

    // === Session Management ===

    /**
     * Start secure session (required before operations)
     */
    virtual bool sessionStart() = 0;

    /**
     * End secure session
     */
    virtual void sessionEnd() = 0;

    /**
     * Check if session is active
     */
    virtual bool isSessionActive() const = 0;

    /**
     * Put chip to sleep
     */
    virtual void sleep() = 0;

    // === ECC Key Operations ===

    /**
     * Generate new ECC key pair
     * @param slot Slot number (0-31)
     * @param curve Curve type
     */
    virtual SeResult eccGenerate(uint8_t slot, EccCurve curve) = 0;

    /**
     * Import existing private key
     * @param slot Slot number
     * @param privKey Private key bytes (32 bytes)
     * @param curve Curve type
     */
    virtual SeResult eccImport(uint8_t slot, const uint8_t* privKey, EccCurve curve) = 0;

    /**
     * Get public key from slot
     * @param slot Slot number
     * @param pubKey Output buffer (65 bytes for P256, 32 for Ed25519)
     * @param curve Output: curve type of key
     */
    virtual SeResult eccGetPublicKey(uint8_t slot, uint8_t* pubKey, EccCurve* curve = nullptr) = 0;

    /**
     * Delete key from slot
     */
    virtual SeResult eccDelete(uint8_t slot) = 0;

    /**
     * Check if slot has a key
     */
    virtual bool eccSlotUsed(uint8_t slot) const = 0;

    // === Signing Operations ===

    /**
     * ECDSA signature (P-256). Implementation hashes the message internally
     * with SHA-256; callers MUST NOT pre-hash.
     * @param slot Key slot
     * @param msg Message to sign (arbitrary length)
     * @param msgLen Message length in bytes
     * @param sig Output signature (raw R||S, 64 bytes)
     * @param sigLen Output signature length
     */
    virtual SeResult ecdsaSign(uint8_t slot, const uint8_t* msg, size_t msgLen,
                               uint8_t* sig, size_t* sigLen) = 0;

    /**
     * ECDSA signature (P-256) over a caller-supplied 32-byte digest. No
     * internal hashing; use when the protocol delivers a pre-hashed value
     * (e.g. PIV GENERAL AUTHENTICATE).
     * @param slot Key slot
     * @param digest 32-byte digest to sign
     * @param sig Output signature (raw R||S, 64 bytes)
     * @param sigLen Output signature length
     */
    virtual SeResult ecdsaSignDigest(uint8_t slot, const uint8_t digest[32],
                                     uint8_t* sig, size_t* sigLen) = 0;

    /**
     * EdDSA signature (Ed25519)
     * @param slot Key slot
     * @param msg Message to sign
     * @param msgLen Message length
     * @param sig Output signature (64 bytes)
     */
    virtual SeResult eddsaSign(uint8_t slot, const uint8_t* msg, size_t msgLen,
                               uint8_t* sig) = 0;

    // === R-Memory Operations ===

    /**
     * Read from R-Memory slot
     * @param slot Slot number (0-511)
     * @param data Output buffer
     * @param maxLen Buffer size
     * @param actualLen Output: actual data length
     */
    virtual SeResult rmemRead(uint16_t slot, uint8_t* data, uint16_t maxLen,
                              uint16_t* actualLen) = 0;

    /**
     * Write to R-Memory slot
     * @param slot Slot number
     * @param data Data to write
     * @param len Data length (max 476 bytes)
     */
    virtual SeResult rmemWrite(uint16_t slot, const uint8_t* data, uint16_t len) = 0;

    /**
     * Erase R-Memory slot
     */
    virtual SeResult rmemErase(uint16_t slot) = 0;

    /**
     * Check if R-Memory slot has data
     */
    virtual bool rmemSlotUsed(uint16_t slot) const = 0;

    // === R-Memory Header Helpers ===

    struct __attribute__((packed)) RMemHeader {
        uint8_t magic;
        uint8_t checksum;
        uint8_t moduleId;
        uint8_t flags;
        char name[RMEM_NAME_LEN];
        uint16_t payloadLen;
    };

    /**
     * Write R-Memory slot with common header + payload
     */
    virtual SeResult rmemWriteWithHeader(uint16_t slot, uint8_t moduleId,
                                         const char* name, uint8_t flags,
                                         const uint8_t* payload, uint16_t payloadLen) = 0;

    /**
     * Read R-Memory slot with common header + payload
     */
    virtual SeResult rmemReadWithHeader(uint16_t slot, RMemHeader* headerOut,
                                        uint8_t* payloadOut, uint16_t payloadMax,
                                        uint16_t* payloadLenOut) = 0;

    // === Random Number Generator ===

    /**
     * Get random bytes from hardware TRNG, with ESP32 TRNG fallback when the
     * secure-element session is unavailable. A WARN is logged on fallback.
     * @param buffer Output buffer
     * @param size Number of bytes
     * @return true if the buffer was filled (from either source)
     */
    virtual bool getRandom(uint8_t* buffer, uint16_t size) = 0;

    /**
     * Get random bytes from hardware TRNG without falling back. Returns false
     * (and leaves the buffer untouched) when the TROPIC TRNG cannot be reached
     * or returns an error. Use for keys/seeds where software RNG is unacceptable.
     * @param buffer Output buffer
     * @param size Number of bytes
     * @return true only when bytes originated from the secure-element TRNG
     */
    virtual bool getRandomStrict(uint8_t* buffer, uint16_t size) = 0;

    // === Diagnostics ===

    /**
     * Get chip serial number
     */
    virtual bool getChipId(uint8_t* serialNum, uint8_t size) = 0;

    /**
     * Get firmware version. Buffers receive the 4-byte version as reported by
     * the chip: index 3 = major, 2 = minor, 1 = patch, 0 = build.
     */
    virtual bool getFwVersion(uint8_t riscvVer[4], uint8_t spectVer[4]) = 0;

    /**
     * Get the user-data R-Memory slot size in bytes as reported by the chip
     * for the running Application FW. Always >= RMEM_SLOT_SIZE and
     * <= RMEM_SLOT_SIZE_MAX.
     */
    virtual uint16_t getRmemSlotSize() const = 0;
};

// Factory function to get secure element instance
ISecureElement* getSecureElementInstance();

} // namespace cdc::hal
