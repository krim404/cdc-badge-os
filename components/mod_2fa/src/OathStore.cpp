#include "mod_2fa/OathStore.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_core/TropicStorage.h"
#include "cdc_log.h"
#include <mbedtls/md.h>
#include <cstring>
#include <ctime>
#include <cstdint>

static const char* TAG = "2FA";

namespace cdc::mod_2fa {

/**
 * \brief Allowed OATH digit count range per RFC 4226 / RFC 6238.
 */
static constexpr uint8_t OATH_DIGITS_MIN = 6;
static constexpr uint8_t OATH_DIGITS_MAX = 8;

/**
 * \brief Allowed TOTP period range in seconds.
 */
static constexpr uint32_t TOTP_PERIOD_MIN = 15;
static constexpr uint32_t TOTP_PERIOD_MAX = 300;

/**
 * \brief Validates OATH entry parameters and clamps to defaults when invalid.
 * \param type In/out entry type, rejected if out of the supported range.
 * \param digits In/out digit count, replaced by default if zero.
 * \param period In/out period seconds (TOTP only), replaced by default if zero.
 * \param algorithm In/out algorithm code.
 * \return `true` if parameters are valid (after clamping).
 */
static bool validateOathParams(uint8_t type, uint8_t& digits, uint32_t& period,
                               uint8_t& algorithm) {
    if (type > static_cast<uint8_t>(OathType::CR)) {
        LOG_W(TAG, "Invalid OATH type %u", type);
        return false;
    }

    // CR produces a raw HMAC and ignores digits/period; only the algorithm
    // matters, and it is restricted to the HMAC sizes the transports support.
    if (type == static_cast<uint8_t>(OathType::CR)) {
        if (algorithm != static_cast<uint8_t>(OathAlgorithm::SHA1) &&
            algorithm != static_cast<uint8_t>(OathAlgorithm::SHA256)) {
            LOG_W(TAG, "CR algorithm %u unsupported (expected SHA1 or SHA256)", algorithm);
            return false;
        }
        return true;
    }

    if (digits == 0) {
        digits = OathStore::DEFAULT_DIGITS;
    } else if (digits < OATH_DIGITS_MIN || digits > OATH_DIGITS_MAX) {
        LOG_W(TAG, "Invalid OATH digits %u, expected %u-%u",
              digits, OATH_DIGITS_MIN, OATH_DIGITS_MAX);
        return false;
    }

    // Period only constrains TOTP; HOTP ignores it.
    if (type == static_cast<uint8_t>(OathType::TOTP)) {
        if (period == 0) {
            period = OathStore::DEFAULT_PERIOD;
        } else if (period < TOTP_PERIOD_MIN || period > TOTP_PERIOD_MAX) {
            LOG_W(TAG, "Invalid TOTP period %lu, expected %u-%u",
                  static_cast<unsigned long>(period), TOTP_PERIOD_MIN, TOTP_PERIOD_MAX);
            return false;
        }
    }

    if (algorithm > static_cast<uint8_t>(OathAlgorithm::SHA512)) {
        LOG_W(TAG, "Invalid OATH algorithm %u", algorithm);
        return false;
    }

    return true;
}

#pragma pack(push, 1)
struct OathPayload {
    uint8_t type;
    char issuer[OathStore::ISSUER_LEN];
    uint8_t secret[OathStore::SECRET_LEN];
    uint8_t secretLen;
    uint8_t digits;
    uint32_t period;
    uint64_t counter;
    uint8_t algorithm;
    uint8_t flags;
};
#pragma pack(pop)

/**
 * \brief Converts one Base32 character into 5-bit value.
 * \param c Input character.
 * \return Value in range 0..31, or `-1` if invalid.
 */
static int base32CharValue(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '2' && c <= '7') return c - '2' + 26;
    return -1;
}

/**
 * \brief Decodes Base32 secret into raw bytes.
 * \param encoded Base32 input string.
 * \param out Output byte buffer.
 * \param outMax Output capacity.
 * \return Number of decoded bytes, or `-1` on error.
 */
static int base32Decode(const char* encoded, uint8_t* out, size_t outMax) {
    if (!encoded || !out) return -1;

    int buffer = 0;
    int bitsLeft = 0;
    size_t count = 0;

    for (const char* p = encoded; *p; ++p) {
        if (*p == '=' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            continue;
        }
        int value = base32CharValue(*p);
        if (value < 0) {
            return -1;
        }
        buffer = (buffer << 5) | value;
        bitsLeft += 5;
        if (bitsLeft >= 8) {
            bitsLeft -= 8;
            if (count >= outMax) {
                return -1;
            }
            out[count++] = static_cast<uint8_t>((buffer >> bitsLeft) & 0xFF);
        }
    }

    return static_cast<int>(count);
}

static const uint32_t POWERS_10[] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000
};

/**
 * \brief Returns singleton OATH store instance.
 * \return Store singleton reference.
 */
OathStore& OathStore::instance() {
    static OathStore inst;
    return inst;
}

/**
 * \brief Configures logical-to-physical slot mapping for OATH entries.
 * \param range Slot range descriptor (RMEM fields are consumed).
 */
void OathStore::setSlotRange(const cdc::core::IModule::SlotRange& range) {
    slots_.setSlotRange(range);
}

/**
 * \brief Reads one OATH entry from secure-element storage.
 * \param slot Logical slot index.
 * \param out Output entry structure.
 * \return `true` on successful read.
 */
bool OathStore::readAccount(uint16_t slot, OathEntry* out) {
    if (!out) return false;
    if (!slots_.hasSlotRange()) return false;
    uint16_t physSlot = 0;
    if (!toPhysicalSlot(slot, &physSlot)) return false;

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;

    cdc::hal::ISecureElement::RMemHeader header = {};
    uint8_t payloadBuf[sizeof(OathPayload)] = {};
    uint16_t payloadLen = 0;

    auto res = se->rmemReadWithHeader(physSlot, &header, payloadBuf, sizeof(payloadBuf), &payloadLen);
    if (res != cdc::hal::SeResult::OK) {
        return false;
    }

    if (header.moduleId != slots_.moduleId()) {
        return false;
    }

    // Reject records that do not match the current record layout (NO-MIGRATION:
    // legacy TOTP entries are simply not readable and get reinitialized).
    if (payloadLen != sizeof(OathPayload)) {
        LOG_W(TAG, "Slot %u payload size mismatch (%u != %u), rejecting",
              physSlot, payloadLen, static_cast<unsigned>(sizeof(OathPayload)));
        return false;
    }

    OathPayload payload = {};
    memcpy(&payload, payloadBuf, sizeof(payload));

    memset(out, 0, sizeof(*out));
    header.name[cdc::hal::ISecureElement::RMEM_NAME_LEN - 1] = '\0';
    payload.issuer[sizeof(payload.issuer) - 1] = '\0';
    out->type = payload.type;
    strncpy(out->name, header.name, sizeof(out->name) - 1);
    strncpy(out->issuer, payload.issuer, sizeof(out->issuer) - 1);
    memcpy(out->secret, payload.secret, sizeof(out->secret));
    // Clamp the persisted length to the fixed buffer so a tampered/corrupt slot
    // cannot drive an out-of-bounds read in later consumers (e.g. base32Encode).
    out->secretLen = payload.secretLen > sizeof(out->secret)
                         ? static_cast<uint8_t>(sizeof(out->secret))
                         : payload.secretLen;
    out->digits = payload.digits ? payload.digits : DEFAULT_DIGITS;
    out->period = payload.period ? payload.period : DEFAULT_PERIOD;
    out->counter = payload.counter;
    out->algorithm = payload.algorithm;
    out->flags = payload.flags;

    return true;
}

/**
 * \brief Builds and persists an OATH payload to a physical slot.
 * \param physSlot Physical R-Memory slot.
 * \param name Account label.
 * \param type Entry type discriminator.
 * \param issuer Optional issuer text.
 * \param secret Raw secret bytes.
 * \param secretLen Secret length.
 * \param digits Output digit count.
 * \param period TOTP period seconds.
 * \param counter HOTP moving factor.
 * \param algorithm Hash algorithm identifier.
 * \param flags Entry flags.
 * \return `true` on successful write.
 */
static bool writePayload(uint16_t physSlot, const char* name, uint8_t type,
                         const char* issuer, const uint8_t* secret, uint8_t secretLen,
                         uint8_t digits, uint32_t period, uint64_t counter,
                         uint8_t algorithm, uint8_t flags) {
    OathPayload payload = {};
    payload.type = type;
    if (issuer) {
        strncpy(payload.issuer, issuer, sizeof(payload.issuer) - 1);
    }
    memcpy(payload.secret, secret, secretLen);
    payload.secretLen = secretLen;
    payload.digits = digits ? digits : OathStore::DEFAULT_DIGITS;
    payload.period = period ? period : OathStore::DEFAULT_PERIOD;
    payload.counter = counter;
    payload.algorithm = algorithm;
    payload.flags = flags;

    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;

    uint8_t moduleId = OathStore::instance().moduleId();
    auto res = se->rmemWriteWithHeader(
        physSlot,
        moduleId,
        name,
        flags,
        reinterpret_cast<const uint8_t*>(&payload),
        sizeof(payload)
    );

    if (res != cdc::hal::SeResult::OK) {
        LOG_E(TAG, "Failed to write slot %u", physSlot);
        return false;
    }

    cdc::core::TropicStorage::instance().writeSlot(moduleId, physSlot, name, flags);
    return true;
}

/**
 * \brief Adds a new OATH entry from a Base32 secret.
 * \param type Entry type (`OathType`).
 * \param name Account label.
 * \param issuer Optional issuer text.
 * \param secretBase32 Base32 secret.
 * \param digits Desired output digits.
 * \param period TOTP period in seconds.
 * \param algorithm Hash algorithm identifier.
 * \param counter Initial HOTP counter (ignored for TOTP).
 * \param flags Entry flag bits (`OathFlag`).
 * \return `true` on successful write.
 */
bool OathStore::addAccount(uint8_t type, const char* name, const char* issuer,
                           const char* secretBase32, uint8_t digits, uint32_t period,
                           uint8_t algorithm, uint64_t counter, uint8_t flags) {
    if (!name || !secretBase32) return false;
    if (!slots_.hasSlotRange()) return false;

    if (strlen(name) >= cdc::hal::ISecureElement::RMEM_NAME_LEN) {
        LOG_W(TAG, "OATH name too long (max %u)",
              cdc::hal::ISecureElement::RMEM_NAME_LEN - 1);
        return false;
    }

    if (!validateOathParams(type, digits, period, algorithm)) {
        return false;
    }

    uint16_t slot = 0;
    if (!slots_.findFreeSlot(&slot)) {
        LOG_W(TAG, "No free OATH slots");
        return false;
    }

    uint8_t secret[SECRET_LEN];
    int secretLen = base32Decode(secretBase32, secret, SECRET_LEN);
    if (secretLen <= 0) {
        LOG_E(TAG, "Invalid Base32 secret");
        return false;
    }

    return writePayload(slot, name, type, issuer, secret,
                        static_cast<uint8_t>(secretLen), digits, period,
                        counter, algorithm, flags);
}

/**
 * \brief Updates an existing OATH entry.
 * \param slot Logical slot index.
 * \param type Entry type (`OathType`).
 * \param name Account label.
 * \param issuer Optional issuer text.
 * \param secretBase32 Base32 secret.
 * \param digits Desired output digits.
 * \param period TOTP period in seconds.
 * \param algorithm Hash algorithm identifier.
 * \param counter HOTP counter (ignored for TOTP).
 * \param flags Entry flag bits (`OathFlag`).
 * \return `true` on successful update.
 */
bool OathStore::updateAccount(uint16_t slot, uint8_t type, const char* name, const char* issuer,
                              const char* secretBase32, uint8_t digits, uint32_t period,
                              uint8_t algorithm, uint64_t counter, uint8_t flags) {
    if (!name || !secretBase32) return false;
    if (!slots_.hasSlotRange()) return false;
    if (strlen(name) >= cdc::hal::ISecureElement::RMEM_NAME_LEN) {
        LOG_W(TAG, "OATH name too long (max %u)",
              cdc::hal::ISecureElement::RMEM_NAME_LEN - 1);
        return false;
    }
    if (!validateOathParams(type, digits, period, algorithm)) {
        return false;
    }
    uint16_t physSlot = 0;
    if (!toPhysicalSlot(slot, &physSlot)) return false;

    uint8_t secret[SECRET_LEN];
    int secretLen = base32Decode(secretBase32, secret, SECRET_LEN);
    if (secretLen <= 0) {
        LOG_E(TAG, "Invalid Base32 secret");
        return false;
    }

    return writePayload(physSlot, name, type, issuer, secret,
                        static_cast<uint8_t>(secretLen), digits, period,
                        counter, algorithm, flags);
}

/**
 * \brief Deletes account in logical slot.
 * \param slot Logical slot index.
 * \return `true` on successful erase.
 */
bool OathStore::deleteAccount(uint16_t slot) {
    if (!slots_.hasSlotRange()) return false;
    uint16_t physSlot = 0;
    if (!toPhysicalSlot(slot, &physSlot)) return false;
    auto* se = cdc::hal::getSecureElementInstance();
    if (!se) return false;

    auto res = se->rmemErase(physSlot);
    if (res != cdc::hal::SeResult::OK) {
        return false;
    }

    cdc::core::TropicStorage::instance().eraseSlot(slots_.moduleId(), physSlot);

    return true;
}

/**
 * \brief Generates a truncated HOTP/TOTP value for a given moving factor.
 *
 * Implements the dynamic-truncation step shared by RFC 4226 (HOTP) and
 * RFC 6238 (TOTP); the caller supplies the moving factor (time step for TOTP,
 * counter for HOTP).
 *
 * \param secret Secret byte buffer.
 * \param secretLen Secret length.
 * \param counter Moving factor (8-byte big-endian HMAC input).
 * \param digits Number of output digits.
 * \param algorithm Hash algorithm.
 * \return Truncated code value.
 */
uint32_t OathStore::generate(const uint8_t* secret, size_t secretLen, uint64_t counter,
                             uint8_t digits, OathAlgorithm algorithm) const {
    if (!secret || secretLen == 0 || secretLen > SECRET_LEN) {
        return 0;
    }

    if (digits < OATH_DIGITS_MIN || digits > OATH_DIGITS_MAX) {
        digits = DEFAULT_DIGITS;
    }

    uint8_t counterBytes[8];
    uint64_t c = counter;
    for (int i = 7; i >= 0; i--) {
        counterBytes[i] = static_cast<uint8_t>(c & 0xFF);
        c >>= 8;
    }

    uint8_t hmac[64] = {};
    size_t hmacLen = 0;
    if (!hmacCompute(algorithm, secret, secretLen, counterBytes, 8, hmac, &hmacLen)) {
        return 0;
    }

    int offset = hmac[hmacLen - 1] & 0x0F;
    uint32_t binary =
        ((hmac[offset] & 0x7F) << 24) |
        ((hmac[offset + 1] & 0xFF) << 16) |
        ((hmac[offset + 2] & 0xFF) << 8) |
        (hmac[offset + 3] & 0xFF);

    return binary % POWERS_10[digits];
}

/**
 * \brief Computes HMAC for selected OATH algorithm.
 * \param algo Hash algorithm.
 * \param key HMAC key.
 * \param keyLen Key length.
 * \param data Input data.
 * \param dataLen Data length.
 * \param output Output digest buffer.
 * \param outputLen Optional output length.
 * \return `true` on success.
 */
bool OathStore::hmacCompute(OathAlgorithm algo, const uint8_t* key, size_t keyLen,
                            const uint8_t* data, size_t dataLen,
                            uint8_t* output, size_t* outputLen) const {
    mbedtls_md_type_t mdType;
    size_t expectedLen;

    switch (algo) {
        case OathAlgorithm::SHA256:
            mdType = MBEDTLS_MD_SHA256;
            expectedLen = 32;
            break;
        case OathAlgorithm::SHA512:
            mdType = MBEDTLS_MD_SHA512;
            expectedLen = 64;
            break;
        default:
            mdType = MBEDTLS_MD_SHA1;
            expectedLen = 20;
            break;
    }

    const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(mdType);
    if (!mdInfo) {
        return false;
    }

    int ret = mbedtls_md_hmac(mdInfo, key, keyLen, data, dataLen, output);
    if (ret != 0) {
        return false;
    }

    if (outputLen) {
        *outputLen = expectedLen;
    }
    return true;
}

/**
 * \brief Persists an updated HOTP counter back to the entry's slot.
 * \param slot Logical slot index.
 * \param entry Entry whose stored fields are preserved.
 * \param counter New counter value to store.
 * \return `true` on successful write.
 */
bool OathStore::persistCounter(uint16_t slot, const OathEntry& entry, uint64_t counter) {
    uint16_t physSlot = 0;
    if (!toPhysicalSlot(slot, &physSlot)) return false;
    return writePayload(physSlot, entry.name, entry.type,
                        entry.issuer[0] ? entry.issuer : nullptr,
                        entry.secret, entry.secretLen, entry.digits, entry.period,
                        counter, entry.algorithm, entry.flags);
}

/**
 * \brief Formats a numeric code into \p codeOut with leading zeros.
 * \param code Numeric code value.
 * \param digits Number of digits to pad to.
 * \param codeOut Output text buffer.
 * \param codeOutLen Output buffer size.
 */
static void formatCode(uint32_t code, uint8_t digits, char* codeOut, size_t codeOutLen) {
    char fmt[8];
    snprintf(fmt, sizeof(fmt), "%%0%ulu",
             digits >= OATH_DIGITS_MIN && digits <= OATH_DIGITS_MAX ? digits : OATH_DIGITS_MIN);
    snprintf(codeOut, codeOutLen, fmt, static_cast<unsigned long>(code));
}

/**
 * \brief Generates a formatted code string for an account slot.
 * \param slot Logical slot index.
 * \param codeOut Output text buffer.
 * \param codeOutLen Output buffer size.
 * \return Remaining seconds for the current TOTP step, `0` for HOTP, or `-1`
 *         on failure.
 */
int8_t OathStore::generateCode(uint16_t slot, char* codeOut, size_t codeOutLen) {
    if (!codeOut || codeOutLen == 0) return -1;
    codeOut[0] = '\0';

    OathEntry account = {};
    if (!readAccount(slot, &account)) {
        return -1;
    }

    if (account.type == static_cast<uint8_t>(OathType::HOTP)) {
        uint64_t counter = account.counter;
        uint32_t code = generate(account.secret, account.secretLen, counter,
                                 account.digits,
                                 static_cast<OathAlgorithm>(account.algorithm));
        // Persist the incremented counter so the next code differs and survives
        // a reboot (RFC 4226 moving factor).
        if (!persistCounter(slot, account, counter + 1)) {
            LOG_E(TAG, "Failed to persist HOTP counter for slot %u", slot);
            return -1;
        }
        formatCode(code, account.digits, codeOut, codeOutLen);
        return 0;
    }

    // TOTP path.
    if (!isTimeValid()) {
        snprintf(codeOut, codeOutLen, "------");
        return -1;
    }

    uint32_t period = account.period ? account.period : DEFAULT_PERIOD;
    uint64_t counter = static_cast<uint64_t>(time(nullptr)) / period;
    uint32_t code = generate(account.secret, account.secretLen, counter,
                             account.digits,
                             static_cast<OathAlgorithm>(account.algorithm));
    formatCode(code, account.digits, codeOut, codeOutLen);
    return static_cast<int8_t>(timeRemaining(account.period));
}

/**
 * \brief Returns seconds remaining in current TOTP time step.
 * \param period TOTP period in seconds.
 * \return Remaining seconds.
 */
uint8_t OathStore::timeRemaining(uint32_t period) const {
    if (period == 0) period = DEFAULT_PERIOD;
    return static_cast<uint8_t>(period - (time(nullptr) % period));
}

/**
 * \brief Returns whether system time is considered valid for TOTP.
 * \return `true` when date is at least year 2024.
 */
bool OathStore::isTimeValid() const {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year >= 124; // 2024+
}

/**
 * \brief Finds the logical slot index of an entry by account name.
 * \param name Account label to search for.
 * \param slotOut Receives the logical slot index on success.
 * \return `true` if an entry with that name was found.
 */
bool OathStore::findByName(const char* name, uint16_t* slotOut) const {
    if (!name || !slotOut) return false;
    if (!slots_.hasSlotRange()) return false;

    struct Ctx {
        const char* target;
        uint16_t slot;
        bool found;
    } ctx = { name, 0, false };

    auto cb = [](uint16_t slot, const cdc::core::TropicStorage::CacheEntry& entry, void* user) {
        auto* c = static_cast<Ctx*>(user);
        if (c->found) return;
        if (strncmp(entry.name, c->target, cdc::hal::ISecureElement::RMEM_NAME_LEN) == 0) {
            uint16_t logical = 0;
            if (OathStore::instance().toLogicalSlot(slot, &logical)) {
                c->slot = logical;
                c->found = true;
            }
        }
    };

    cdc::core::TropicStorage::instance().forEachSlot(
        slots_.moduleId(),
        slots_.rmemStart(),
        slots_.rmemEnd(),
        cb, &ctx);

    if (!ctx.found) return false;
    *slotOut = ctx.slot;
    return true;
}

/**
 * \brief Computes the raw HMAC challenge-response for a named CR entry.
 *
 * Shares the same HMAC engine as TOTP/HOTP via \ref hmacCompute, but returns
 * the full digest without dynamic truncation. SHA512 is rejected here because
 * neither transport carries it and the unified validator already limits CR to
 * SHA1/SHA256 at write time.
 *
 * \param entryName Account label to look up.
 * \param challenge Challenge bytes.
 * \param clen Challenge length in bytes.
 * \param out Output digest buffer (>= 32 bytes).
 * \param touchRequiredOut Optional; receives the entry's touch-required flag.
 * \return Digest length, or `-1` on failure.
 */
int OathStore::challengeResponse(const char* entryName, const uint8_t* challenge, size_t clen,
                                 uint8_t* out, bool* touchRequiredOut) {
    if (!entryName || !out) return -1;
    if (clen != 0 && !challenge) return -1;

    uint16_t slot = 0;
    if (!findByName(entryName, &slot)) {
        return -1;
    }

    OathEntry entry = {};
    if (!readAccount(slot, &entry)) {
        return -1;
    }

    if (entry.type != static_cast<uint8_t>(OathType::CR)) {
        LOG_W(TAG, "Entry '%s' is not a CR entry", entryName);
        return -1;
    }
    if (entry.secretLen == 0) {
        return -1;
    }

    auto algo = static_cast<OathAlgorithm>(entry.algorithm);
    if (algo != OathAlgorithm::SHA1 && algo != OathAlgorithm::SHA256) {
        LOG_W(TAG, "CR entry '%s' has unsupported algorithm %u", entryName, entry.algorithm);
        return -1;
    }

    size_t outLen = 0;
    if (!hmacCompute(algo, entry.secret, entry.secretLen, challenge, clen, out, &outLen)) {
        return -1;
    }

    if (touchRequiredOut) {
        *touchRequiredOut = (entry.flags & OathFlag::TOUCH_REQUIRED) != 0;
    }
    return static_cast<int>(outLen);
}

/**
 * \brief Finds the logical slot of the entry flagged as the USB-CR responder.
 * \param slotOut Receives the logical slot index on success.
 * \return `true` if a USB-CR-designated CR entry exists.
 */
bool OathStore::findUsbCrSlot(uint16_t* slotOut) const {
    if (!slotOut) return false;
    if (!slots_.hasSlotRange()) return false;

    struct Ctx {
        uint16_t slot;
        bool found;
    } ctx = { 0, false };

    auto cb = [](uint16_t slot, const cdc::core::TropicStorage::CacheEntry&, void* user) {
        auto* c = static_cast<Ctx*>(user);
        if (c->found) return;
        uint16_t logical = 0;
        if (!OathStore::instance().toLogicalSlot(slot, &logical)) return;
        OathEntry entry = {};
        if (!OathStore::instance().readAccount(logical, &entry)) return;
        if (entry.type == static_cast<uint8_t>(OathType::CR) &&
            (entry.flags & OathFlag::USB_CR_SLOT) != 0) {
            c->slot = logical;
            c->found = true;
        }
    };

    cdc::core::TropicStorage::instance().forEachSlot(
        slots_.moduleId(),
        slots_.rmemStart(),
        slots_.rmemEnd(),
        cb, &ctx);

    if (!ctx.found) return false;
    *slotOut = ctx.slot;
    return true;
}

/**
 * \brief Computes the raw HMAC challenge-response for the USB-CR slot entry.
 * \param challenge Challenge bytes.
 * \param clen Challenge length in bytes.
 * \param out Output digest buffer (>= 32 bytes).
 * \param touchRequiredOut Optional; receives the entry's touch-required flag.
 * \return Digest length, or `-1` on failure.
 */
int OathStore::challengeResponseUsbSlot(const uint8_t* challenge, size_t clen,
                                        uint8_t* out, bool* touchRequiredOut) {
    if (!out) return -1;

    uint16_t slot = 0;
    if (!findUsbCrSlot(&slot)) {
        return -1;
    }

    OathEntry entry = {};
    if (!readAccount(slot, &entry)) {
        return -1;
    }
    return challengeResponse(entry.name, challenge, clen, out, touchRequiredOut);
}

/**
 * \brief Clears the USB-CR-slot flag on every entry except \p keepSlot.
 * \param keepSlot Logical slot to keep designated (`0xFFFF` clears all).
 */
void OathStore::clearUsbCrFlagExcept(uint16_t keepSlot) {
    if (!slots_.hasSlotRange()) return;

    struct Ctx {
        uint16_t keep;
    } ctx = { keepSlot };

    auto cb = [](uint16_t slot, const cdc::core::TropicStorage::CacheEntry&, void* user) {
        auto* c = static_cast<Ctx*>(user);
        auto& store = OathStore::instance();
        uint16_t logical = 0;
        if (!store.toLogicalSlot(slot, &logical)) return;
        if (logical == c->keep) return;
        OathEntry entry = {};
        if (!store.readAccount(logical, &entry)) return;
        if ((entry.flags & OathFlag::USB_CR_SLOT) == 0) return;
        uint8_t cleared = static_cast<uint8_t>(entry.flags & ~OathFlag::USB_CR_SLOT);
        uint16_t physSlot = 0;
        if (!store.toPhysicalSlot(logical, &physSlot)) return;
        writePayload(physSlot, entry.name, entry.type,
                     entry.issuer[0] ? entry.issuer : nullptr,
                     entry.secret, entry.secretLen, entry.digits, entry.period,
                     entry.counter, entry.algorithm, cleared);
    };

    cdc::core::TropicStorage::instance().forEachSlot(
        slots_.moduleId(),
        slots_.rmemStart(),
        slots_.rmemEnd(),
        cb, &ctx);
}

} // namespace cdc::mod_2fa
