#pragma once

#include "cdc_core/IModule.h"
#include "cdc_core/SlotManager.h"
#include <cstdint>
#include <cstddef>
#include <ctime>

namespace cdc::mod_2fa {

/**
 * \brief Hash algorithm used by an OATH entry's HMAC engine.
 */
enum class OathAlgorithm : uint8_t {
    SHA1 = 0,
    SHA256 = 1,
    SHA512 = 2
};

/**
 * \brief OATH entry type discriminator.
 */
enum class OathType : uint8_t {
    TOTP = 0,
    HOTP = 1,
    CR = 2
};

/**
 * \brief Per-entry flag bits stored in `OathEntry::flags`.
 */
namespace OathFlag {
    /// Require an on-device touch confirmation before answering a CR request.
    constexpr uint8_t TOUCH_REQUIRED = 0x01;
    /// Designate this CR entry as the USB OTP-HID slot-2 responder.
    constexpr uint8_t USB_CR_SLOT = 0x02;
}

/**
 * \brief Unified OATH credential record (TOTP, HOTP, and reserved CR).
 */
struct OathEntry {
    uint8_t type;            ///< OathType discriminator.
    char name[16 + 1];       ///< Account label.
    char issuer[32 + 1];     ///< Optional issuer text.
    uint8_t secret[64];      ///< Raw HMAC key.
    uint8_t secretLen;       ///< Valid bytes in \ref secret.
    uint8_t algorithm;       ///< OathAlgorithm value.
    uint8_t digits;          ///< Output digit count (TOTP/HOTP).
    uint32_t period;         ///< TOTP step in seconds (TOTP only).
    uint64_t counter;        ///< Moving factor (HOTP only).
    uint8_t flags;           ///< Reserved entry flags.
};

class OathStore {
public:
    static constexpr uint8_t NAME_LEN = 16;
    static constexpr uint8_t ISSUER_LEN = 32;
    static constexpr uint8_t SECRET_LEN = 64;
    static constexpr uint8_t DEFAULT_DIGITS = 6;
    static constexpr uint32_t DEFAULT_PERIOD = 30;

    bool readAccount(uint16_t slot, OathEntry* out);
    bool addAccount(uint8_t type, const char* name, const char* issuer,
                    const char* secretBase32, uint8_t digits, uint32_t period,
                    uint8_t algorithm, uint64_t counter, uint8_t flags = 0);

    /**
     * \brief Adds a new OATH entry from raw secret bytes.
     *
     * Same semantics as \ref addAccount, but takes the HMAC key directly
     * (YKOATH PUT delivers key bytes, not Base32).
     *
     * \param key Raw HMAC key.
     * \param keyLen Key length (1..SECRET_LEN).
     */
    bool addAccountRaw(uint8_t type, const char* name, const char* issuer,
                       const uint8_t* key, uint8_t keyLen, uint8_t digits,
                       uint32_t period, uint8_t algorithm, uint64_t counter,
                       uint8_t flags = 0);

    /**
     * \brief YKOATH CALCULATE: dynamic truncation for an explicit challenge.
     *
     * TOTP: HMACs the host-supplied 8-byte big-endian challenge (time step
     * computed by the host). HOTP: ignores the challenge, consumes the
     * internal moving counter and persists the increment. CR entries are
     * rejected (not part of the YKOATH surface).
     *
     * \param slot Logical slot index.
     * \param challenge 8-byte big-endian challenge from the host.
     * \param truncatedOut Receives the 4-byte truncated value (RFC 4226
     *        dynamic truncation, MSB already masked, before modulo).
     * \param digitsOut Receives the entry's digit count.
     * \return `true` on success.
     */
    bool calculateForChallenge(uint16_t slot, const uint8_t challenge[8],
                               uint8_t truncatedOut[4], uint8_t* digitsOut);
    bool updateAccount(uint16_t slot, uint8_t type, const char* name, const char* issuer,
                       const char* secretBase32, uint8_t digits, uint32_t period,
                       uint8_t algorithm, uint64_t counter, uint8_t flags = 0);
    bool deleteAccount(uint16_t slot);

    /**
     * \brief Computes the raw HMAC challenge-response for a CR entry by name.
     *
     * Looks up the named entry, requires it to be of type CR, and computes the
     * full untruncated `HMAC(secret, challenge)` using the entry's algorithm.
     * No dynamic truncation is applied (unlike TOTP/HOTP).
     *
     * \param entryName Account label to look up.
     * \param challenge Challenge bytes.
     * \param clen Challenge length in bytes.
     * \param out Output buffer for the digest (must hold at least 32 bytes).
     * \param touchRequiredOut Optional; receives the entry's touch-required flag.
     * \return Response length (20 for SHA1, 32 for SHA256), or `-1` on failure
     *         or when the named entry is missing or not a CR entry.
     */
    int challengeResponse(const char* entryName, const uint8_t* challenge, size_t clen,
                          uint8_t* out, bool* touchRequiredOut = nullptr);

    /**
     * \brief Computes the raw HMAC challenge-response for the USB-CR slot entry.
     *
     * Resolves the single CR entry flagged \ref OathFlag::USB_CR_SLOT (the
     * designated YubiKey slot-2 responder for the USB OTP-HID transport) and
     * computes its untruncated `HMAC(secret, challenge)`. No touch gate is
     * applied here; the caller withholds the response until confirmed.
     *
     * \param challenge Challenge bytes.
     * \param clen Challenge length in bytes.
     * \param out Output buffer for the digest (must hold at least 32 bytes).
     * \param touchRequiredOut Optional; receives the entry's touch-required flag.
     * \return Response length (20 for SHA1, 32 for SHA256), or `-1` when no
     *         entry is designated or computation fails.
     */
    int challengeResponseUsbSlot(const uint8_t* challenge, size_t clen, uint8_t* out,
                                 bool* touchRequiredOut = nullptr);

    /**
     * \brief Finds the logical slot of the CR entry flagged as the USB-CR slot.
     * \param slotOut Receives the logical slot index on success.
     * \return `true` if a USB-CR-designated entry exists.
     */
    bool findUsbCrSlot(uint16_t* slotOut) const;

    /**
     * \brief Clears the USB-CR-slot flag on every entry except \p keepSlot.
     *
     * Enforces the "exactly one USB-CR responder" invariant: when an entry is
     * designated, any previously designated entry is demoted.
     *
     * \param keepSlot Logical slot to keep designated (use `0xFFFF` to clear all).
     */
    void clearUsbCrFlagExcept(uint16_t keepSlot);

    /**
     * \brief Finds a logical slot index by account name.
     * \param name Account label to search for.
     * \param slotOut Receives the logical slot index on success.
     * \return `true` if an entry with that name exists.
     */
    bool findByName(const char* name, uint16_t* slotOut) const;

    /**
     * \brief Renders the current code for an entry into \p codeOut.
     *
     * For TOTP this uses the wall-clock time step; for HOTP it consumes and
     * persists the moving counter (incremented and written back to the slot).
     *
     * \param slot Logical slot index.
     * \param codeOut Output buffer (must hold at least 9 bytes for 8-digit codes).
     * \param codeOutLen Size of \p codeOut in bytes.
     * \return Remaining seconds in the current TOTP step, `0` for HOTP, or `-1`
     *         on failure.
     */
    int8_t generateCode(uint16_t slot, char* codeOut, size_t codeOutLen);

    bool isTimeValid() const;
    uint8_t timeRemaining(uint32_t period) const;

    static OathStore& instance();

    void setSlotRange(const cdc::core::IModule::SlotRange& range);
    uint16_t capacity() const { return slots_.capacity(); }
    bool toPhysicalSlot(uint16_t logicalIndex, uint16_t* slotOut) const {
        return slots_.toPhysicalSlot(logicalIndex, slotOut);
    }
    bool toLogicalSlot(uint16_t slot, uint16_t* logicalIndexOut) const {
        return slots_.toLogicalSlot(slot, logicalIndexOut);
    }
    bool hasSlotRange() const { return slots_.hasSlotRange(); }
    uint8_t moduleId() const { return slots_.moduleId(); }
    uint16_t rmemStart() const { return slots_.rmemStart(); }
    uint16_t rmemEnd() const { return slots_.rmemEnd(); }

private:
    OathStore() = default;

    uint32_t generate(const uint8_t* secret, size_t secretLen, uint64_t counter,
                      uint8_t digits, OathAlgorithm algorithm) const;
    bool computeTruncated(const uint8_t* secret, size_t secretLen, uint64_t counter,
                          OathAlgorithm algorithm, uint32_t* binaryOut) const;
    bool hmacCompute(OathAlgorithm algo, const uint8_t* key, size_t keyLen,
                     const uint8_t* data, size_t dataLen,
                     uint8_t* output, size_t* outputLen) const;
    bool persistCounter(uint16_t slot, const OathEntry& entry, uint64_t counter);

    cdc::core::SlotManager slots_;
};

} // namespace cdc::mod_2fa
