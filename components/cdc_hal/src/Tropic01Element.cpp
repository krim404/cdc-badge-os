/**
 * \file
 * \brief TROPIC01 secure-element HAL implementation with session-managed libtropic access.
 *
 * Synchronization model:
 *   - The shared SPI bus (display + TROPIC01) is the single serialization point.
 *     Every public method acquires `device_.spi` via `spi_device_acquire_bus`
 *     for the entire duration of its libtropic interaction and releases it on
 *     exit. The libtropic port layer does NOT acquire the bus per frame, so
 *     a multi-frame libtropic operation is atomic with respect to other SPI
 *     users (display, future SPI peripherals) and other tasks.
 *   - Internal `_unlocked` variants assume the caller already holds the bus
 *     and never re-acquire.
 *   - On detection of `LT_L1_CHIP_ALARM_MODE` the global SystemLock is
 *     latched and the system enters deep-sleep lockdown from the main loop.
 */

#include "cdc_hal/ISecureElement.h"
#include "cdc_hal/libtropic_port_esp32.h"
#include "cdc_hal/hw_config.h"
#include "cdc_hal/pairing_key_config.h"
#include "cdc_core/SystemLock.h"
#include "cdc_spi_lock.h"
#include "cdc_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "psa/crypto.h"
#include <atomic>
#include <cstring>

/** \brief libtropic headers (already guarded for C/C++ linkage). */
#include "libtropic.h"
#include "libtropic_common.h"
#include "libtropic_l2.h"
#include "libtropic_l3.h"
#include "libtropic_port.h"
#include "lt_l2_api_structs.h"
#include "libtropic_mbedtls_v4.h"

static const char* TAG = "TR01";
static constexpr uint8_t RMEM_HEADER_MAGIC = 0xCD;

// PAIRING_KEY_PRIV / PAIRING_KEY_PUB / PAIRING_KEY_SLOT come from
// cdc_hal/pairing_key_config.h (build-time selectable, default production key).

namespace cdc::hal {

/**
 * \brief Secure-element implementation backed by libtropic.
 */
class Tropic01Element : public ISecureElement {
public:
    Tropic01Element() = default;

    // IService implementation
    bool init() override;
    bool start() override;
    void stop() override;
    core::ServiceState getState() const override { return state_; }
    const char* getName() const override { return "secure_element"; }

    // Session Management
    bool sessionStart() override;
    void sessionEnd() override;
    bool isSessionActive() const override { return sessionActive_.load(std::memory_order_acquire); }
    void sleep() override;

    // Pairing-Key (SH0) Management — DANGEROUS, IRREVERSIBLE
    SeResult pairingKeyWrite(uint8_t slot, const uint8_t pub[32]) override;
    SeResult pairingKeyInvalidate(uint8_t slot) override;
    uint8_t activePairingSlot() const override { return static_cast<uint8_t>(PAIRING_KEY_SLOT); }

    // ECC Operations
    SeResult eccGenerate(uint8_t slot, EccCurve curve) override;
    SeResult eccImport(uint8_t slot, const uint8_t* privKey, EccCurve curve) override;
    SeResult eccGetPublicKey(uint8_t slot, uint8_t* pubKey, EccCurve* curve) override;
    SeResult eccDelete(uint8_t slot) override;
    bool eccSlotUsed(uint8_t slot) const override;

    bool getFwVersion(uint8_t riscvVer[4], uint8_t spectVer[4]) override;
    uint16_t getRmemSlotSize() const override { return rmemSlotSize_; }

    // Signing
    SeResult ecdsaSign(uint8_t slot, const uint8_t* msg, size_t msgLen,
                       uint8_t* sig, size_t* sigLen) override;
    SeResult ecdsaSignDigest(uint8_t slot, const uint8_t digest[32],
                             uint8_t* sig, size_t* sigLen) override;
    SeResult eddsaSign(uint8_t slot, const uint8_t* msg, size_t msgLen,
                       uint8_t* sig) override;

    // R-Memory
    SeResult rmemRead(uint16_t slot, uint8_t* data, uint16_t maxLen,
                      uint16_t* actualLen) override;
    SeResult rmemWrite(uint16_t slot, const uint8_t* data, uint16_t len) override;
    SeResult rmemErase(uint16_t slot) override;
    bool rmemSlotUsed(uint16_t slot) const override;
    SeResult rmemWriteWithHeader(uint16_t slot, uint8_t moduleId,
                                 const char* name, uint8_t flags,
                                 const uint8_t* payload, uint16_t payloadLen) override;
    SeResult rmemReadWithHeader(uint16_t slot, RMemHeader* headerOut,
                                uint8_t* payloadOut, uint16_t payloadMax,
                                uint16_t* payloadLenOut) override;

    // Random
    bool getRandom(uint8_t* buffer, uint16_t size) override;
    bool getRandomStrict(uint8_t* buffer, uint16_t size) override;

    // Diagnostics
    bool getChipId(uint8_t* serialNum, uint8_t size) override;

private:
    // Public-API helpers
    bool acquireBus();
    void releaseBus();

    // Bus-locked workers (caller must hold the bus)
    bool sessionStart_unlocked();
    bool ensureSession_unlocked(const char* op);
    SeResult rmemRead_unlocked(uint16_t slot, uint8_t* data, uint16_t maxLen, uint16_t* actualLen);
    SeResult rmemWrite_unlocked(uint16_t slot, const uint8_t* data, uint16_t len);
    SeResult rmemErase_unlocked(uint16_t slot);
    SeResult eccGetPublicKey_unlocked(uint8_t slot, uint8_t* pubKey, EccCurve* curve);
    void dumpChipStatus_unlocked(const char* context);
    bool ensureRConfigBits(lt_config_obj_addr_t addr, uint32_t mask, bool set);
    lt_ret_t randomChunked_unlocked(uint8_t* buffer, uint16_t size);

    SeResult mapResult(lt_ret_t ret) const;
    void handleSessionError(lt_ret_t ret);
    uint8_t computeHeaderChecksum(const RMemHeader& header) const;
    bool validateHeader(const RMemHeader& header) const;

    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;
    std::atomic<bool> sessionActive_{false};

    // libtropic handles
    lt_handle_t handle_ = {};
    lt_ctx_mbedtls_v4_t cryptoCtx_ = {};
    lt_dev_esp32_t device_ = {};

    // ECC slot-usage cache. Mutated under the SPI bus lock; read lock-free
    // from the const accessor, hence atomic.
    mutable std::atomic<uint32_t> eccSlotCache_{0};
    mutable std::atomic<bool> eccCacheValid_{false};

    // Populated once during init() from libtropic's runtime FW-attribute
    // query. Const after init() returns, so no synchronization needed.
    uint16_t rmemSlotSize_ = ISecureElement::RMEM_SLOT_SIZE;
};

/**
 * \brief Initializes PSA crypto and the libtropic device context.
 * \return `true` on success, otherwise `false`.
 */
bool Tropic01Element::init() {
    if (state_ != core::ServiceState::UNINITIALIZED) {
        return state_ == core::ServiceState::INITIALIZED ||
               state_ == core::ServiceState::STARTED;
    }

    LOG_I(TAG, "Initializing TROPIC01...");

    psa_status_t psaStatus = psa_crypto_init();
    if (psaStatus != PSA_SUCCESS && psaStatus != PSA_ERROR_BAD_STATE) {
        LOG_E(TAG, "PSA crypto init failed (status=%ld)", (long)psaStatus);
        state_ = core::ServiceState::ERROR;
        core::SystemLock::instance().triggerLockdown(
            core::LockdownReason::TR01_INIT_FAILED,
            "PSA crypto init failed");
        return false;
    }
    LOG_I(TAG, "PSA crypto initialized");

    memset(&handle_, 0, sizeof(handle_));
    device_.cs_pin = static_cast<gpio_num_t>(TR01_CS_PIN);
    device_.spi = nullptr;
    handle_.l2.device = &device_;
    handle_.l3.crypto_ctx = &cryptoCtx_;

    lt_ret_t ret = lt_init(&handle_);
    if (ret != LT_OK) {
        LOG_E(TAG, "lt_init failed (%s)", lt_ret_verbose(ret));
        state_ = core::ServiceState::ERROR;
        core::SystemLock::instance().triggerLockdown(
            ret == LT_L1_CHIP_ALARM_MODE
                ? core::LockdownReason::TR01_ALARM_MODE
                : core::LockdownReason::TR01_INIT_FAILED,
            lt_ret_verbose(ret));
        return false;
    }

    uint16_t slotSize = handle_.tr01_attrs.r_mem_udata_slot_size_max;
    if (slotSize >= ISecureElement::RMEM_SLOT_SIZE &&
        slotSize <= ISecureElement::RMEM_SLOT_SIZE_MAX) {
        rmemSlotSize_ = slotSize;
    }

    state_ = core::ServiceState::INITIALIZED;
    LOG_I(TAG, "TROPIC01 initialized (CS=GPIO%d, R-Mem slot=%u B)",
          TR01_CS_PIN, rmemSlotSize_);
    return true;
}

/**
 * \brief Starts secure-element service when initialized.
 */
bool Tropic01Element::start() {
    if (state_ == core::ServiceState::INITIALIZED ||
        state_ == core::ServiceState::STOPPED) {
        state_ = core::ServiceState::STARTED;
        return true;
    }
    return state_ == core::ServiceState::STARTED;
}

/**
 * \brief Stops secure-element service and closes any active session.
 */
void Tropic01Element::stop() {
    if (state_ == core::ServiceState::STARTED) {
        if (sessionActive_.load(std::memory_order_acquire)) {
            sessionEnd();
        }
        state_ = core::ServiceState::STOPPED;
    }
}

/**
 * \brief Acquires the shared SPI bus for the duration of one operation.
 * \return `true` when the bus was acquired within the timeout.
 */
bool Tropic01Element::acquireBus() {
    if (!device_.spi) {
        LOG_E(TAG, "acquireBus: SPI device not initialized");
        core::SystemLock::instance().triggerLockdown(
            core::LockdownReason::TR01_UNREACHABLE,
            "SPI device handle null");
        return false;
    }
    // Serialize the WHOLE secure-element operation against the e-paper display,
    // which shares this SPI bus. A TROPIC01 op is many CS-framed transfers; the
    // display (CalEPD) takes the same recursive lock around each of its
    // transfers, so a display transfer can never slip between two TR01 frames
    // (CS asserted) and corrupt the transaction (which would latch a chip
    // tamper alarm). Held for the whole op, released in releaseBus().
    SemaphoreHandle_t spiLock = cdc::hal::sharedSpiLock();
    if (spiLock) {
        xSemaphoreTakeRecursive(spiLock, portMAX_DELAY);
    }
    // ESP-IDF requires portMAX_DELAY for spi_device_acquire_bus; other timeouts
    // return ESP_ERR_INVALID_ARG. Operations run under a busy display+TR01 bus
    // are short, so blocking is acceptable here.
    esp_err_t err = spi_device_acquire_bus(device_.spi, portMAX_DELAY);
    if (err != ESP_OK) {
        LOG_E(TAG, "acquireBus failed: %d", err);
        if (spiLock) {
            xSemaphoreGiveRecursive(spiLock);
        }
        core::SystemLock::instance().triggerLockdown(
            core::LockdownReason::TR01_UNREACHABLE,
            "SPI bus acquire failed");
        return false;
    }
    return true;
}

/**
 * \brief Releases the shared SPI bus.
 */
void Tropic01Element::releaseBus() {
    if (device_.spi) {
        spi_device_release_bus(device_.spi);
    }
    SemaphoreHandle_t spiLock = cdc::hal::sharedSpiLock();
    if (spiLock) {
        xSemaphoreGiveRecursive(spiLock);
    }
}

/**
 * \brief Opens a secure session with the TROPIC01 chip.
 */
bool Tropic01Element::sessionStart() {
    if (core::SystemLock::instance().isLocked()) {
        return false;
    }
    if (sessionActive_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!acquireBus()) {
        return false;
    }
    bool ok = sessionStart_unlocked();
    releaseBus();
    return ok;
}

/**
 * \brief Brings the masked bits of an R-Config object to the desired state.
 *
 * Reads the object and writes the adjusted value back only when the masked bits
 * differ, so it is idempotent across boots. Bus must be held and a secure
 * session active.
 * \param addr R-Config object address.
 * \param mask Bit mask to constrain.
 * \param set  `true` to set the masked bits, `false` to clear them.
 * \return `true` if a write was performed and succeeded.
 */
bool Tropic01Element::ensureRConfigBits(lt_config_obj_addr_t addr, uint32_t mask, bool set) {
    uint32_t cfg = 0;
    if (lt_r_config_read(&handle_, addr, &cfg) != LT_OK) {
        return false;
    }
    const uint32_t desired = set ? (cfg | mask) : (cfg & ~mask);
    if (desired == cfg) {
        return false;
    }
    return lt_r_config_write(&handle_, addr, desired) == LT_OK;
}

/**
 * \brief Performs the actual session establishment. Bus must be held.
 */
bool Tropic01Element::sessionStart_unlocked() {
    if (sessionActive_.load(std::memory_order_acquire)) {
        return true;
    }
    LOG_I(TAG, "Starting secure session...");

    lt_ret_t ret = lt_verify_chip_and_start_secure_session(
        &handle_, PAIRING_KEY_PRIV, PAIRING_KEY_PUB, PAIRING_KEY_SLOT);

    if (ret != LT_OK) {
        LOG_E(TAG, "Secure session failed (%s)", lt_ret_verbose(ret));
        static int64_t lastDumpUs = 0;
        int64_t nowUs = esp_timer_get_time();
        if (nowUs - lastDumpUs > 1000000) {
            lastDumpUs = nowUs;
            dumpChipStatus_unlocked("sessionStart");
        }
        // Fail-closed: any session establishment failure latches lockdown.
        // The secure element is unusable without a verified session, so the
        // OS must not continue running modules that depend on it.
        core::SystemLock::instance().triggerLockdown(
            ret == LT_L1_CHIP_ALARM_MODE
                ? core::LockdownReason::TR01_ALARM_MODE
                : core::LockdownReason::TR01_INIT_FAILED,
            lt_ret_verbose(ret));
        sessionActive_.store(false, std::memory_order_release);
        return false;
    }

    sessionActive_.store(true, std::memory_order_release);
    eccCacheValid_.store(false, std::memory_order_release);

    if (ensureRConfigBits(TR01_CFG_SLEEP_MODE_ADDR, 0x01u, true)) {
        LOG_I(TAG, "Auto-sleep enabled");
    }
    // Disable maintenance mode (start-up R-Config bit) so the chip rejects
    // start-up firmware uploads. Reversible R-Config, re-applied automatically
    // after an R-Memory wipe. Ref: Tropic Square advisory ODR_TR01_SA_2026012900.
    if (ensureRConfigBits(TR01_CFG_START_UP_ADDR,
                          BOOTLOADER_CO_CFG_START_UP_MAINTENANCE_ENA_MASK, false)) {
        LOG_I(TAG, "TROPIC01 maintenance mode disabled (R-Config)");
    }

    LOG_I(TAG, "Secure session active");
    return true;
}

/**
 * \brief Aborts the active secure session.
 */
void Tropic01Element::sessionEnd() {
    if (!sessionActive_.load(std::memory_order_acquire)) {
        return;
    }
    if (!acquireBus()) {
        sessionActive_.store(false, std::memory_order_release);
        return;
    }
    lt_session_abort(&handle_);
    sessionActive_.store(false, std::memory_order_release);
    LOG_I(TAG, "Session ended");
    releaseBus();
}

/**
 * \brief Ensures an active secure session for an operation. Bus must be held.
 * \param op Operation name used for logs.
 * \return `true` when session is active, otherwise `false`.
 */
bool Tropic01Element::ensureSession_unlocked(const char* op) {
    if (sessionActive_.load(std::memory_order_acquire)) {
        return true;
    }
    LOG_W(TAG, "Session inactive for %s - restarting", op);
    return sessionStart_unlocked();
}

/**
 * \brief Requests secure-element sleep mode and marks session inactive.
 */
void Tropic01Element::sleep() {
    if (core::SystemLock::instance().isLocked()) {
        return;
    }
    if (!sessionActive_.load(std::memory_order_acquire)) {
        return;
    }
    if (!acquireBus()) {
        return;
    }
    lt_ret_t ret = lt_sleep(&handle_, TR01_L2_SLEEP_KIND_SLEEP);
    if (ret != LT_OK) {
        LOG_E(TAG, "Sleep failed (%s)", lt_ret_verbose(ret));
        handleSessionError(ret);
    } else {
        sessionActive_.store(false, std::memory_order_release);
        LOG_I(TAG, "Entered sleep mode");
    }
    releaseBus();
}

/**
 * \brief Invalidates session state and triggers lockdown on fatal errors.
 */
void Tropic01Element::handleSessionError(lt_ret_t ret) {
    switch (ret) {
        case LT_L1_CHIP_ALARM_MODE:
            sessionActive_.store(false, std::memory_order_release);
            core::SystemLock::instance().triggerLockdown(
                core::LockdownReason::TR01_ALARM_MODE,
                lt_ret_verbose(ret));
            break;
        case LT_L2_HSK_ERR:
        case LT_L2_NO_SESSION:
        case LT_L2_TAG_ERR:
            sessionActive_.store(false, std::memory_order_release);
            break;
        default:
            break;
    }
}

/**
 * \brief Maps libtropic return codes to generic secure-element results.
 */
SeResult Tropic01Element::mapResult(lt_ret_t ret) const {
    switch (ret) {
        case LT_OK:
            return SeResult::OK;
        case LT_PARAM_ERR:
            return SeResult::INVALID_PARAM;
        case LT_L3_R_MEM_DATA_READ_SLOT_EMPTY:
        case LT_L3_SLOT_EMPTY:
            return SeResult::SLOT_EMPTY;
        case LT_L3_SLOT_NOT_EMPTY:
            return SeResult::SLOT_OCCUPIED;
        case LT_HOST_NO_SESSION:
        case LT_L2_NO_SESSION:
            return SeResult::SESSION_REQUIRED;
        case LT_L1_CHIP_ALARM_MODE:
            return SeResult::ALARM_MODE;
        default:
            return SeResult::ERROR;
    }
}

/**
 * \brief Performs a raw CHIP_STATUS read via manual SPI transfer and logs the byte.
 *        Caller must hold the bus.
 */
void Tropic01Element::dumpChipStatus_unlocked(const char* context) {
    if (!device_.spi) {
        LOG_E(TAG, "[%s] CHIP_STATUS read skipped: SPI not initialized", context);
        return;
    }

    handle_.l2.buff[0] = 0xAA;  // TR01_L1_GET_RESPONSE_REQ_ID

    lt_ret_t ret = lt_port_spi_csn_low(&handle_.l2);
    if (ret != LT_OK) {
        LOG_E(TAG, "[%s] CHIP_STATUS read: CS-low failed (%d)", context, (int)ret);
        return;
    }

    ret = lt_port_spi_transfer(&handle_.l2, 0, 1, 100);
    lt_port_spi_csn_high(&handle_.l2);

    if (ret != LT_OK) {
        LOG_E(TAG, "[%s] CHIP_STATUS read: SPI transfer failed (%d)", context, (int)ret);
        return;
    }

    uint8_t status = handle_.l2.buff[0];
    LOG_E(TAG, "[%s] CHIP_STATUS=0x%02X (READY=%d ALARM=%d STARTUP=%d)",
          context, status,
          (status & 0x01) ? 1 : 0,
          (status & 0x02) ? 1 : 0,
          (status & 0x04) ? 1 : 0);

    if (status == 0xFF) {
        LOG_E(TAG, "[%s] All-ones MISO: chip not responding (wiring/power/CS)", context);
    } else if (status == 0x00) {
        LOG_E(TAG, "[%s] All-zeros MISO: chip not driving (no power or stuck)", context);
    } else if (status & 0x02) {
        LOG_E(TAG, "[%s] Real ALARM mode: chip set ALARM bit (tamper/violation)", context);
    }
}

/**
 * \brief Writes a host public pairing key into an empty pairing slot (one-shot).
 */
SeResult Tropic01Element::pairingKeyWrite(uint8_t slot, const uint8_t pub[32]) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (!pub || slot > TR01_PAIRING_KEY_SLOT_INDEX_3) return SeResult::INVALID_PARAM;
    // Refuse to touch the slot the running firmware authenticates with: a bad
    // write there could leave the chip unreachable on next boot.
    if (slot == PAIRING_KEY_SLOT) {
        LOG_E(TAG, "pairingKeyWrite refused: slot %u is the active pairing slot", slot);
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;

    SeResult result;
    if (!ensureSession_unlocked("pairingKeyWrite")) {
        result = SeResult::SESSION_REQUIRED;
    } else {
        LOG_W(TAG, "Writing pairing key into slot %u (one-shot, irreversible)", slot);
        lt_ret_t ret = lt_pairing_key_write(&handle_, pub,
                                            static_cast<lt_pkey_index_t>(slot));
        handleSessionError(ret);
        result = mapResult(ret);
    }
    releaseBus();
    return result;
}

/**
 * \brief Permanently invalidates a pairing slot (irreversible).
 */
SeResult Tropic01Element::pairingKeyInvalidate(uint8_t slot) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (slot > TR01_PAIRING_KEY_SLOT_INDEX_3) return SeResult::INVALID_PARAM;
    // Never invalidate the slot in use: that would kill the secure channel the
    // firmware needs and could brick the chip if it is the last valid slot.
    if (slot == PAIRING_KEY_SLOT) {
        LOG_E(TAG, "pairingKeyInvalidate refused: slot %u is the active pairing slot", slot);
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;

    SeResult result;
    if (!ensureSession_unlocked("pairingKeyInvalidate")) {
        result = SeResult::SESSION_REQUIRED;
    } else {
        LOG_W(TAG, "PERMANENTLY invalidating pairing slot %u", slot);
        lt_ret_t ret = lt_pairing_key_invalidate(&handle_,
                                                 static_cast<lt_pkey_index_t>(slot));
        handleSessionError(ret);
        result = mapResult(ret);
    }
    releaseBus();
    return result;
}

/**
 * \brief Generates an ECC key pair in the requested slot.
 */
SeResult Tropic01Element::eccGenerate(uint8_t slot, EccCurve curve) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (slot >= ECC_SLOT_COUNT) {
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;

    SeResult result;
    if (!ensureSession_unlocked("eccGenerate")) {
        result = SeResult::SESSION_REQUIRED;
    } else {
        lt_ecc_curve_type_t ltCurve = (curve == EccCurve::ED25519) ?
                                       TR01_CURVE_ED25519 : TR01_CURVE_P256;
        lt_ret_t ret = lt_ecc_key_generate(&handle_, static_cast<lt_ecc_slot_t>(slot), ltCurve);
        if (ret == LT_OK) {
            eccSlotCache_.fetch_or(1u << slot, std::memory_order_release);
        }
        handleSessionError(ret);
        result = mapResult(ret);
    }
    releaseBus();
    return result;
}

/**
 * \brief Imports an ECC private key into the requested slot.
 */
SeResult Tropic01Element::eccImport(uint8_t slot, const uint8_t* privKey, EccCurve curve) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (slot >= ECC_SLOT_COUNT || !privKey) {
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;

    SeResult result;
    if (!ensureSession_unlocked("eccImport")) {
        result = SeResult::SESSION_REQUIRED;
    } else {
        lt_ecc_curve_type_t ltCurve = (curve == EccCurve::ED25519) ?
                                       TR01_CURVE_ED25519 : TR01_CURVE_P256;
        lt_ret_t ret = lt_ecc_key_store(&handle_, static_cast<lt_ecc_slot_t>(slot), ltCurve, privKey);
        if (ret == LT_OK) {
            eccSlotCache_.fetch_or(1u << slot, std::memory_order_release);
        }
        handleSessionError(ret);
        result = mapResult(ret);
    }
    releaseBus();
    return result;
}

/**
 * \brief Reads public key from ECC slot. Bus must be held.
 */
SeResult Tropic01Element::eccGetPublicKey_unlocked(uint8_t slot, uint8_t* pubKey, EccCurve* curve) {
    if (!ensureSession_unlocked("eccGetPublicKey")) {
        return SeResult::SESSION_REQUIRED;
    }
    lt_ecc_curve_type_t ltCurve;
    lt_ecc_key_origin_t ltOrigin;
    lt_ret_t ret = lt_ecc_key_read(&handle_, static_cast<lt_ecc_slot_t>(slot),
                                    pubKey, 64, &ltCurve, &ltOrigin);
    if (ret == LT_OK && curve) {
        *curve = (ltCurve == TR01_CURVE_ED25519) ? EccCurve::ED25519 : EccCurve::P256;
    }
    handleSessionError(ret);
    if (ret == LT_L3_INVALID_KEY) {
        return SeResult::SLOT_EMPTY;
    }
    return mapResult(ret);
}

/**
 * \brief Reads public key from ECC slot.
 */
SeResult Tropic01Element::eccGetPublicKey(uint8_t slot, uint8_t* pubKey, EccCurve* curve) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (slot >= ECC_SLOT_COUNT || !pubKey) {
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;
    SeResult result = eccGetPublicKey_unlocked(slot, pubKey, curve);
    releaseBus();
    return result;
}

/**
 * \brief Erases ECC key material from slot.
 */
SeResult Tropic01Element::eccDelete(uint8_t slot) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (slot >= ECC_SLOT_COUNT) {
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;

    SeResult result;
    if (!ensureSession_unlocked("eccDelete")) {
        result = SeResult::SESSION_REQUIRED;
    } else {
        lt_ret_t ret = lt_ecc_key_erase(&handle_, static_cast<lt_ecc_slot_t>(slot));
        if (ret == LT_OK) {
            eccSlotCache_.fetch_and(~(1u << slot), std::memory_order_release);
        }
        handleSessionError(ret);
        result = mapResult(ret);
    }
    releaseBus();
    return result;
}

/**
 * \brief Checks whether ECC slot currently contains a key.
 */
bool Tropic01Element::eccSlotUsed(uint8_t slot) const {
    if (slot >= ECC_SLOT_COUNT) {
        return false;
    }
    if (eccCacheValid_.load(std::memory_order_acquire)) {
        return (eccSlotCache_.load(std::memory_order_acquire) & (1u << slot)) != 0;
    }
    auto* self = const_cast<Tropic01Element*>(this);
    uint8_t tempKey[65];
    return self->eccGetPublicKey(slot, tempKey, nullptr) == SeResult::OK;
}

/**
 * \brief Signs a message using ECDSA key in slot.
 *
 * Callers pass the raw message. ECDSA on the chip signs a 32-byte digest, so
 * the message is hashed with SHA-256 here before signing.
 */
SeResult Tropic01Element::ecdsaSign(uint8_t slot, const uint8_t* msg, size_t msgLen,
                                     uint8_t* sig, size_t* sigLen) {
    if (!msg || msgLen == 0) {
        return SeResult::INVALID_PARAM;
    }

    uint8_t digest[32];
    size_t digestLen = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, msg, msgLen, digest, sizeof(digest),
                         &digestLen) != PSA_SUCCESS ||
        digestLen != sizeof(digest)) {
        return SeResult::ERROR;
    }
    return ecdsaSignDigest(slot, digest, sig, sigLen);
}

/**
 * \brief Signs a caller-supplied 32-byte digest using the ECDSA key in slot.
 */
SeResult Tropic01Element::ecdsaSignDigest(uint8_t slot, const uint8_t digest[32],
                                           uint8_t* sig, size_t* sigLen) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (slot >= ECC_SLOT_COUNT || !digest || !sig || !sigLen) {
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;

    SeResult result;
    if (!ensureSession_unlocked("ecdsaSign")) {
        result = SeResult::SESSION_REQUIRED;
    } else {
        lt_ret_t ret = lt_ecc_ecdsa_sign(&handle_, static_cast<lt_ecc_slot_t>(slot),
                                          digest, 32u, sig);
        if (ret == LT_OK) {
            *sigLen = TR01_ECDSA_EDDSA_SIGNATURE_LENGTH;
        }
        handleSessionError(ret);
        result = mapResult(ret);
    }
    releaseBus();
    return result;
}

/**
 * \brief Signs message using EdDSA key in slot.
 */
SeResult Tropic01Element::eddsaSign(uint8_t slot, const uint8_t* msg, size_t msgLen,
                                     uint8_t* sig) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (slot >= ECC_SLOT_COUNT || !msg || msgLen == 0 || !sig) {
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;

    SeResult result;
    if (!ensureSession_unlocked("eddsaSign")) {
        result = SeResult::SESSION_REQUIRED;
    } else {
        lt_ret_t ret = lt_ecc_eddsa_sign(&handle_, static_cast<lt_ecc_slot_t>(slot),
                                          msg, static_cast<uint16_t>(msgLen), sig);
        handleSessionError(ret);
        result = mapResult(ret);
    }
    releaseBus();
    return result;
}

/**
 * \brief Reads raw R-memory slot data. Bus must be held.
 */
SeResult Tropic01Element::rmemRead_unlocked(uint16_t slot, uint8_t* data, uint16_t maxLen,
                                             uint16_t* actualLen) {
    if (!ensureSession_unlocked("rmemRead")) {
        return SeResult::SESSION_REQUIRED;
    }
    uint16_t bytesRead = 0;
    lt_ret_t ret = lt_r_mem_data_read(&handle_, slot, data, maxLen, &bytesRead);
    if (ret == LT_OK && actualLen) {
        *actualLen = bytesRead;
    }
    handleSessionError(ret);
    if (ret == LT_L3_R_MEM_DATA_READ_SLOT_EMPTY) {
        if (actualLen) *actualLen = 0;
        return SeResult::SLOT_EMPTY;
    }
    return mapResult(ret);
}

/**
 * \brief Reads raw R-memory slot data.
 */
SeResult Tropic01Element::rmemRead(uint16_t slot, uint8_t* data, uint16_t maxLen,
                                    uint16_t* actualLen) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (slot >= RMEM_SLOT_COUNT || !data || maxLen == 0) {
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;
    SeResult result = rmemRead_unlocked(slot, data, maxLen, actualLen);
    releaseBus();
    return result;
}

/**
 * \brief Writes raw data to an R-memory slot. Bus must be held.
 */
SeResult Tropic01Element::rmemWrite_unlocked(uint16_t slot, const uint8_t* data, uint16_t len) {
    if (!ensureSession_unlocked("rmemWrite")) {
        return SeResult::SESSION_REQUIRED;
    }
    lt_ret_t ret = lt_r_mem_data_write(&handle_, slot, data, len);
    handleSessionError(ret);
    return mapResult(ret);
}

/**
 * \brief Writes raw data to an R-memory slot.
 */
SeResult Tropic01Element::rmemWrite(uint16_t slot, const uint8_t* data, uint16_t len) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (slot >= RMEM_SLOT_COUNT || !data || len == 0 || len > rmemSlotSize_) {
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;
    SeResult result = rmemWrite_unlocked(slot, data, len);
    releaseBus();
    return result;
}

/**
 * \brief Erases one R-memory slot. Bus must be held.
 */
SeResult Tropic01Element::rmemErase_unlocked(uint16_t slot) {
    if (!ensureSession_unlocked("rmemErase")) {
        return SeResult::SESSION_REQUIRED;
    }
    lt_ret_t ret = lt_r_mem_data_erase(&handle_, slot);
    handleSessionError(ret);
    return mapResult(ret);
}

/**
 * \brief Erases one R-memory slot.
 */
SeResult Tropic01Element::rmemErase(uint16_t slot) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (slot >= RMEM_SLOT_COUNT) {
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;
    SeResult result = rmemErase_unlocked(slot);
    releaseBus();
    return result;
}

/**
 * \brief Checks whether R-memory slot contains data.
 */
bool Tropic01Element::rmemSlotUsed(uint16_t slot) const {
    if (slot >= RMEM_SLOT_COUNT) {
        return false;
    }
    auto* self = const_cast<Tropic01Element*>(this);
    uint8_t tempBuf[4];
    uint16_t actualLen = 0;
    SeResult res = self->rmemRead(slot, tempBuf, sizeof(tempBuf), &actualLen);
    return res == SeResult::OK && actualLen > 0;
}

/**
 * \brief Computes header checksum for structured R-memory payload.
 */
uint8_t Tropic01Element::computeHeaderChecksum(const RMemHeader& header) const {
    uint16_t sum = 0;
    sum += header.moduleId;
    sum += header.flags;
    for (size_t i = 0; i < sizeof(header.name); i++) {
        sum += static_cast<uint8_t>(header.name[i]);
    }
    sum += static_cast<uint8_t>(header.payloadLen & 0xFF);
    sum += static_cast<uint8_t>((header.payloadLen >> 8) & 0xFF);
    return static_cast<uint8_t>(sum & 0xFF);
}

/**
 * \brief Validates header magic and checksum.
 */
bool Tropic01Element::validateHeader(const RMemHeader& header) const {
    if (header.magic != RMEM_HEADER_MAGIC) {
        return false;
    }
    return header.checksum == computeHeaderChecksum(header);
}

/**
 * \brief Writes payload to R-memory slot with metadata header.
 */
SeResult Tropic01Element::rmemWriteWithHeader(uint16_t slot, uint8_t moduleId,
                                              const char* name, uint8_t flags,
                                              const uint8_t* payload, uint16_t payloadLen) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (slot >= RMEM_SLOT_COUNT) {
        return SeResult::INVALID_PARAM;
    }
    if (payloadLen > (rmemSlotSize_ - sizeof(RMemHeader))) {
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;

    SeResult result;
    SeResult eraseRes = rmemErase_unlocked(slot);
    if (eraseRes != SeResult::OK && eraseRes != SeResult::SLOT_EMPTY) {
        result = eraseRes;
    } else {
        RMemHeader header = {};
        header.magic = RMEM_HEADER_MAGIC;
        header.moduleId = moduleId;
        header.flags = flags;
        header.payloadLen = payloadLen;
        if (name) {
            strncpy(header.name, name, sizeof(header.name) - 1);
            header.name[sizeof(header.name) - 1] = '\0';
        }
        header.checksum = computeHeaderChecksum(header);

        uint8_t buffer[RMEM_SLOT_SIZE_MAX] = {};
        memcpy(buffer, &header, sizeof(header));
        if (payloadLen > 0 && payload) {
            memcpy(buffer + sizeof(header), payload, payloadLen);
        }

        result = rmemWrite_unlocked(slot, buffer, static_cast<uint16_t>(sizeof(header) + payloadLen));
    }
    releaseBus();
    return result;
}

/**
 * \brief Reads and validates headered R-memory record.
 */
SeResult Tropic01Element::rmemReadWithHeader(uint16_t slot, RMemHeader* headerOut,
                                             uint8_t* payloadOut, uint16_t payloadMax,
                                             uint16_t* payloadLenOut) {
    if (core::SystemLock::instance().isLocked()) return SeResult::ALARM_MODE;
    if (slot >= RMEM_SLOT_COUNT) {
        return SeResult::INVALID_PARAM;
    }
    if (!acquireBus()) return SeResult::ERROR;

    SeResult result;
    uint8_t buffer[RMEM_SLOT_SIZE_MAX] = {};
    uint16_t actualLen = 0;
    SeResult res = rmemRead_unlocked(slot, buffer, sizeof(buffer), &actualLen);
    if (res != SeResult::OK) {
        result = res;
    } else if (actualLen < sizeof(RMemHeader)) {
        result = SeResult::ERROR;
    } else {
        RMemHeader header = {};
        memcpy(&header, buffer, sizeof(header));
        if (!validateHeader(header)) {
            result = SeResult::ERROR;
        } else if (header.payloadLen > (rmemSlotSize_ - sizeof(RMemHeader))) {
            result = SeResult::ERROR;
        } else if (actualLen < static_cast<uint16_t>(sizeof(RMemHeader) + header.payloadLen)) {
            result = SeResult::ERROR;
        } else if (payloadOut && header.payloadLen > 0 && payloadMax < header.payloadLen) {
            result = SeResult::INVALID_PARAM;
        } else {
            if (headerOut) {
                *headerOut = header;
            }
            if (payloadLenOut) {
                *payloadLenOut = header.payloadLen;
            }
            if (payloadOut && header.payloadLen > 0) {
                memcpy(payloadOut, buffer + sizeof(RMemHeader), header.payloadLen);
            }
            result = SeResult::OK;
        }
    }
    releaseBus();
    return result;
}

/**
 * \brief Reads `size` random bytes from the TROPIC01 TRNG in <=255-byte chunks.
 *
 * lt_random_value_get accepts at most 255 bytes per call. Bus must be held and
 * a secure session active.
 * \return LT_OK when the whole buffer was filled, otherwise the failing result.
 */
lt_ret_t Tropic01Element::randomChunked_unlocked(uint8_t* buffer, uint16_t size) {
    uint16_t offset = 0;
    while (offset < size) {
        const uint16_t remaining = size - offset;
        const uint8_t chunk = static_cast<uint8_t>(remaining > 255 ? 255 : remaining);
        lt_ret_t ret = lt_random_value_get(&handle_, buffer + offset, chunk);
        if (ret != LT_OK) {
            return ret;
        }
        offset += chunk;
    }
    return LT_OK;
}

/**
 * \brief Fills buffer with random bytes from TROPIC TRNG with ESP fallback.
 *
 * Always returns true on a non-empty request; a WARN is logged whenever the
 * ESP32 TRNG fallback is taken so the origin is auditable in the log stream.
 * Callers that require hardware-only entropy must use getRandomStrict().
 */
bool Tropic01Element::getRandom(uint8_t* buffer, uint16_t size) {
    if (!buffer || size == 0) {
        return false;
    }
    if (core::SystemLock::instance().isLocked()) {
        LOG_W(TAG, "SystemLock active, ESP32 TRNG fallback (size=%u)", size);
        esp_fill_random(buffer, size);
        return true;
    }
    if (!acquireBus()) {
        LOG_W(TAG, "Bus unavailable, ESP32 TRNG fallback (size=%u)", size);
        esp_fill_random(buffer, size);
        return true;
    }

    if (!ensureSession_unlocked("getRandom")) {
        LOG_W(TAG, "No SE session, ESP32 TRNG fallback (size=%u)", size);
        esp_fill_random(buffer, size);
    } else {
        lt_ret_t ret = randomChunked_unlocked(buffer, size);
        handleSessionError(ret);
        if (ret != LT_OK) {
            LOG_W(TAG, "TROPIC01 TRNG failed (%s), ESP32 TRNG fallback (size=%u)",
                  lt_ret_verbose(ret), size);
            esp_fill_random(buffer, size);
        }
    }
    releaseBus();
    return true;
}

/**
 * \brief Fills buffer with random bytes from TROPIC TRNG only; no fallback.
 */
bool Tropic01Element::getRandomStrict(uint8_t* buffer, uint16_t size) {
    if (!buffer || size == 0) {
        return false;
    }
    if (core::SystemLock::instance().isLocked()) {
        return false;
    }
    if (!acquireBus()) {
        return false;
    }
    bool ok = false;
    if (ensureSession_unlocked("getRandomStrict")) {
        lt_ret_t ret = randomChunked_unlocked(buffer, size);
        handleSessionError(ret);
        ok = (ret == LT_OK);
    }
    releaseBus();
    return ok;
}

/**
 * \brief Reads chip serial identifier.
 */
bool Tropic01Element::getChipId(uint8_t* serialNum, uint8_t size) {
    if (core::SystemLock::instance().isLocked()) return false;
    if (!serialNum || size < 8) {
        return false;
    }
    if (!acquireBus()) return false;

    bool ok = false;
    if (ensureSession_unlocked("getChipId")) {
        struct lt_chip_id_t chipId;
        memset(&chipId, 0, sizeof(chipId));
        lt_ret_t ret = lt_get_info_chip_id(&handle_, &chipId);
        if (ret == LT_OK) {
            uint8_t copyLen = (size < sizeof(chipId.ser_num)) ? size : sizeof(chipId.ser_num);
            memcpy(serialNum, &chipId.ser_num, copyLen);
            ok = true;
        }
        handleSessionError(ret);
    }
    releaseBus();
    return ok;
}

/**
 * \brief Reads RISC-V and SPECT firmware major version bytes.
 */
bool Tropic01Element::getFwVersion(uint8_t riscvVer[4], uint8_t spectVer[4]) {
    if (core::SystemLock::instance().isLocked()) return false;
    if (!riscvVer || !spectVer) {
        return false;
    }
    if (!acquireBus()) return false;

    bool ok = false;
    if (ensureSession_unlocked("getFwVersion")) {
        uint8_t riscvFw[TR01_L2_GET_INFO_RISCV_FW_SIZE] = {0};
        lt_ret_t ret = lt_get_info_riscv_fw_ver(&handle_, riscvFw);
        if (ret == LT_OK) {
            for (int i = 0; i < 4; i++) riscvVer[i] = riscvFw[i];
        }
        uint8_t spectFw[TR01_L2_GET_INFO_SPECT_FW_SIZE] = {0};
        lt_ret_t ret2 = lt_get_info_spect_fw_ver(&handle_, spectFw);
        if (ret2 == LT_OK) {
            for (int i = 0; i < 4; i++) spectVer[i] = spectFw[i];
        }
        handleSessionError(ret == LT_OK ? ret2 : ret);
        ok = (ret == LT_OK) && (ret2 == LT_OK);
    }
    releaseBus();
    return ok;
}

/** \brief Global singleton instance of TROPIC secure-element implementation. */
static Tropic01Element g_secureElement;

/**
 * \brief Returns the singleton secure element service instance.
 */
ISecureElement* getSecureElementInstance() {
    return &g_secureElement;
}

} // namespace cdc::hal
