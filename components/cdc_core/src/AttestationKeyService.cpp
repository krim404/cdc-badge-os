#include "cdc_core/AttestationKeyService.h"
#include "cdc_core/Raii.h"
#include "cdc_log.h"
#include <mbedtls/sha256.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cstring>

static const char* TAG = "AttestKey";
static constexpr const char* NVS_NAMESPACE = "attest";
static constexpr const char* NVS_KEY_PUBHASH = "pubhash";
static constexpr uint32_t RETRY_INTERVAL_MS = 3000;

/** \brief Size of a SHA-256 digest in bytes (FIPS 180-4). */
static constexpr size_t SHA256_DIGEST_SIZE = 32;
/** \brief Uncompressed P-256 public key, raw X||Y coordinates (no SEC1 0x04 prefix). */
static constexpr size_t P256_PUBKEY_RAW_SIZE = 64;

namespace cdc::core {

/**
 * \brief Initializes service state.
 * \return `true` if service is initialized.
 */
bool AttestationKeyService::init() {
    if (state_ != ServiceState::UNINITIALIZED) {
        return state_ == ServiceState::INITIALIZED || state_ == ServiceState::STARTED;
    }
    state_ = ServiceState::INITIALIZED;
    return true;
}

/**
 * \brief Starts service, ensures initialized state, and attempts to provision
 *        the attestation key synchronously so dependent module inits (FIDO2
 *        attestation certificate) find it present. Falls back to onTick
 *        retries when the secure element is not ready yet.
 * \return `true` if service is started.
 */
bool AttestationKeyService::start() {
    if (state_ == ServiceState::UNINITIALIZED) {
        if (!init()) return false;
    }
    state_ = ServiceState::STARTED;
    if (ensureKey()) {
        ready_ = true;
        LOG_I(TAG, "Attestation key ready");
    }
    return true;
}

/**
 * \brief Stops attestation-key background processing.
 */
void AttestationKeyService::stop() {
    state_ = ServiceState::STOPPED;
}

/**
 * \brief Periodically attempts to ensure attestation key exists and is valid.
 * \param nowMs Current uptime in milliseconds.
 */
void AttestationKeyService::onTick(uint32_t nowMs) {
    if (state_ != ServiceState::STARTED || ready_) return;
    if (nowMs - lastAttemptMs_ < RETRY_INTERVAL_MS) return;
    lastAttemptMs_ = nowMs;
    if (ensureKey()) {
        ready_ = true;
        LOG_I(TAG, "Attestation key ready");
    }
}

/**
 * \brief Loads stored public-key hash from NVS.
 * \param out Output hash buffer.
 * \param outLen Expected hash length.
 * \return `true` on successful load.
 */
bool AttestationKeyService::loadStoredHash(uint8_t* out, size_t outLen) {
    if (!out || outLen == 0) return false;
    NvsScope nvs(NVS_NAMESPACE, NVS_READONLY);
    if (!nvs) return false;
    size_t len = outLen;
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY_PUBHASH, out, &len);
    return err == ESP_OK && len == outLen;
}

/**
 * \brief Stores public-key hash to NVS.
 * \param data Hash bytes.
 * \param len Hash length.
 * \return `true` on successful save.
 */
bool AttestationKeyService::saveStoredHash(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    NvsScope nvs(NVS_NAMESPACE, NVS_READWRITE);
    if (!nvs) return false;
    esp_err_t err = nvs_set_blob(nvs, NVS_KEY_PUBHASH, data, len);
    if (err == ESP_OK) err = nvs.commit();
    return err == ESP_OK;
}

/**
 * \brief Ensures valid P-256 attestation key exists and matches persisted hash.
 * \return `true` when key is ready and consistent.
 */
bool AttestationKeyService::ensureKey() {
    if (!secureElement_) {
        LOG_W(TAG, "Secure element not set");
        return false;
    }
    if (!secureElement_->isSessionActive()) {
        if (!secureElement_->sessionStart()) {
            LOG_W(TAG, "Secure element session not active");
            return false;
        }
    }

    uint8_t pubkey[P256_PUBKEY_RAW_SIZE] = {};
    hal::EccCurve curve = hal::EccCurve::P256;
    hal::SeResult res = secureElement_->eccGetPublicKey(ATTESTATION_ECC_SLOT, pubkey, &curve);

    if (res == hal::SeResult::SLOT_EMPTY) {
        LOG_I(TAG, "Attestation slot empty, generating key");
        if (secureElement_->eccGenerate(ATTESTATION_ECC_SLOT, hal::EccCurve::P256) !=
            hal::SeResult::OK) {
            LOG_E(TAG, "Failed to generate attestation key");
            return false;
        }
        res = secureElement_->eccGetPublicKey(ATTESTATION_ECC_SLOT, pubkey, &curve);
    }

    if (res != hal::SeResult::OK) {
        LOG_W(TAG, "Attestation key read failed: %d", static_cast<int>(res));
        return false;
    }

    if (curve != hal::EccCurve::P256) {
        LOG_W(TAG, "Attestation key wrong curve, regenerating");
        secureElement_->eccDelete(ATTESTATION_ECC_SLOT);
        if (secureElement_->eccGenerate(ATTESTATION_ECC_SLOT, hal::EccCurve::P256) !=
            hal::SeResult::OK) {
            LOG_E(TAG, "Failed to regenerate attestation key");
            return false;
        }
        res = secureElement_->eccGetPublicKey(ATTESTATION_ECC_SLOT, pubkey, &curve);
        if (res != hal::SeResult::OK) return false;
    }

    uint8_t hash[SHA256_DIGEST_SIZE] = {};
    mbedtls_sha256(pubkey, sizeof(pubkey), hash, 0);

    uint8_t stored[SHA256_DIGEST_SIZE] = {};
    if (loadStoredHash(stored, sizeof(stored))) {
        if (memcmp(stored, hash, sizeof(hash)) == 0) {
            return true;
        }
        LOG_W(TAG, "Attestation key mismatch, regenerating");
        secureElement_->eccDelete(ATTESTATION_ECC_SLOT);
        if (secureElement_->eccGenerate(ATTESTATION_ECC_SLOT, hal::EccCurve::P256) !=
            hal::SeResult::OK) {
            LOG_E(TAG, "Failed to regenerate attestation key");
            return false;
        }
        res = secureElement_->eccGetPublicKey(ATTESTATION_ECC_SLOT, pubkey, &curve);
        if (res != hal::SeResult::OK) return false;
        mbedtls_sha256(pubkey, sizeof(pubkey), hash, 0);
    }

    if (!saveStoredHash(hash, sizeof(hash))) {
        LOG_W(TAG, "Failed to store attestation key hash");
    }

    return true;
}

} // namespace cdc::core
