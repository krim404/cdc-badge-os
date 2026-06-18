/**
 * \file
 * \brief FIDO2/WebAuthn runtime entry points and processing task.
 */

#include "mod_fido2/fido2.h"
#include "mod_fido2/fido2_storage.h"
#include "mod_fido2/ctap2.h"
#include "mod_fido2/ctaphid.h"
#include "mod_fido2/u2f.h"
#include "cdc_core/pin_storage_c.h"
#include "cdc_log.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

// USB transport hooks implemented by Fido2Module.cpp.
namespace cdc::mod_fido2 {
    bool fido2_usb_available();
    bool fido2_usb_ready();
    uint16_t fido2_usb_read(uint8_t* buffer);
    bool fido2_usb_write(const uint8_t* buffer);
}

using namespace cdc::mod_fido2;

static const char* TAG = "FIDO2";

/** \brief Global FIDO2 runtime state. */

static struct {
    bool initialized;
    fido2_user_presence_cb_t user_presence_cb;
    TaskHandle_t task_handle;
    bool pin_verified;  // PIN was verified via ClientPIN protocol
} g_fido2 = {};

/**
 * \brief Background task that receives CTAPHID packets and sends responses.
 * \param arg Unused task argument.
 */
static void fido2_task(void* arg) {
    (void)arg;
    uint8_t packet[64];

    LOG_I(TAG, "Processing task started");

    while (1) {
        // Process incoming packets and drain responses immediately to avoid overwriting
        while (fido2_usb_available()) {
            // If we still owe a response and USB isn't ready, pause input to avoid overwrite
            if (ctaphid_has_response() && !fido2_usb_ready()) {
                break;
            }

            if (fido2_usb_read(packet) == 64) {
                ctaphid_process_packet(packet);
            }

            // Send pending responses before reading more packets (with brief wait)
            int inner_retry = 0;
            while (ctaphid_has_response()) {
                if (fido2_usb_ready()) {
                    uint8_t response[64];
                    if (ctaphid_get_response_packet(response)) {
                        if (!fido2_usb_write(response)) {
                            LOG_W(TAG, "USB FIDO write failed");
                            break;
                        }
                        LOG_D(TAG, "Sent response packet");
                        inner_retry = 0;
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                } else {
                    inner_retry++;
                    if (inner_retry > 10) {  // Brief wait, then continue in outer loop
                        LOG_D(TAG, "USB not ready, deferring to outer loop");
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }

            // If response still pending, stop reading more packets this cycle
            if (ctaphid_has_response()) {
                break;
            }
        }

        // Send any remaining response packets (with retry on USB not ready)
        int retry_count = 0;
        while (ctaphid_has_response()) {
            if (fido2_usb_ready()) {
                uint8_t response[64];
                if (ctaphid_get_response_packet(response)) {
                    if (!fido2_usb_write(response)) {
                        LOG_W(TAG, "USB FIDO write failed (outer)");
                        break;
                    }
                    LOG_D(TAG, "Sent response packet (outer)");
                    retry_count = 0;  // Reset retry counter on success
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            } else {
                // Wait for USB to be ready instead of giving up
                retry_count++;
                if (retry_count > 100) {  // ~1 second timeout
                    LOG_W(TAG, "USB not ready timeout, aborting response");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        // Check timeouts
        ctaphid_check_timeout();

        // Poll rate
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * \brief Initializes storage, CTAP layers, and starts the processing task.
 * \return `true` on success, otherwise `false`.
 */
bool fido2_init(void) {
    LOG_I(TAG, "Initializing...");

    memset(&g_fido2, 0, sizeof(g_fido2));

    // Initialize storage layer
    uint8_t cred_count = fido2_storage_init();
    LOG_I(TAG, "Storage initialized, %d credentials", cred_count);

    // Initialize CTAP2 protocol handler
    if (!ctap2_init()) {
        LOG_E(TAG, "CTAP2 init failed");
        return false;
    }

    // Initialize CTAPHID transport
    if (!ctaphid_init()) {
        LOG_E(TAG, "CTAPHID init failed");
        return false;
    }

    // Initialize U2F attestation certificate
    if (!u2f_init_attestation()) {
        LOG_W(TAG, "U2F attestation init failed (non-fatal)");
        // Continue anyway - FIDO2 will still work, U2F might not
    }

    // Start FIDO2 processing task
    xTaskCreate(fido2_task, "fido2", 6144, nullptr,
                configMAX_PRIORITIES - 2, &g_fido2.task_handle);

    g_fido2.initialized = true;
    LOG_I(TAG, "Initialized");
    return true;
}

/**
 * \brief Sets callback used to request user presence for CTAP operations.
 * \param cb User-presence callback.
 */
void fido2_set_user_presence_callback(fido2_user_presence_cb_t cb) {
    g_fido2.user_presence_cb = cb;
}

/**
 * \brief Requests user presence from host/application callback.
 * \param rp_id Relying-party identifier.
 * \param action Requested user action.
 * \param user_name Optional user name.
 * \return User presence decision.
 */
fido2_user_presence_result_t fido2_request_user_presence(
    const char *rp_id,
    fido2_action_t action,
    const char *user_name
) {
    if (g_fido2.user_presence_cb) {
        return g_fido2.user_presence_cb(rp_id, action, user_name);
    }
    // No callback set - auto-approve (unsafe, but allows testing)
    LOG_W(TAG, "No user presence callback - auto-approving");
    return FIDO2_UP_APPROVED;
}

/**
 * \brief Stores whether PIN verification was completed via ClientPIN.
 * \param verified PIN verification state.
 */
void fido2_set_pin_verified(bool verified) {
    g_fido2.pin_verified = verified;
    if (verified) {
        LOG_I(TAG, "PIN verified via ClientPIN - device PIN will be skipped");
    }
}

/**
 * \brief Returns current PIN-verified state.
 * \return `true` if PIN was verified, otherwise `false`.
 */
bool fido2_is_pin_verified(void) {
    return g_fido2.pin_verified;
}

/**
 * \brief Returns number of stored credentials.
 * \return Credential count.
 */
uint8_t fido2_get_credential_count(void) {
    return fido2_storage_count();
}

/**
 * \brief Retrieves credential metadata by visible index.
 * \param index Zero-based visible credential index.
 * \param info Destination structure.
 * \return `true` on success, otherwise `false`.
 */
bool fido2_get_credential_info(uint8_t index, fido2_credential_info_t *info) {
    if (!info) return false;

    // Map index to slot
    uint8_t found = 0;
    for (uint8_t slot = 0; slot < FIDO2_MAX_CREDENTIALS; slot++) {
        if (fido2_storage_slot_used(slot)) {
            if (found == index) {
                return fido2_storage_get_credential(slot, info);
            }
            found++;
        }
    }

    return false;
}

/**
 * \brief Finds credential slots matching RP ID hash.
 * \param rp_id_hash 32-byte RP hash.
 * \param out_indices Destination slot list.
 * \param max_indices Capacity of `out_indices`.
 * \return Number of matching credentials.
 */
uint8_t fido2_find_credentials_by_rp(const uint8_t *rp_id_hash,
                                      uint8_t *out_indices, uint8_t max_indices) {
    return fido2_storage_find_by_rp(rp_id_hash, out_indices, max_indices);
}

/**
 * \brief Deletes credential in given slot.
 * \param slot Credential slot index.
 * \return `true` on success, otherwise `false`.
 */
bool fido2_delete_credential(uint8_t slot) {
    return fido2_storage_delete_credential(slot);
}

/**
 * \brief Removes all credentials and resets FIDO2 data.
 * \return `true` on success.
 */
bool fido2_factory_reset(void) {
    LOG_W(TAG, "Factory reset requested");

    // Delete all credentials
    for (uint8_t slot = 0; slot < FIDO2_MAX_CREDENTIALS; slot++) {
        if (fido2_storage_slot_used(slot)) {
            fido2_storage_delete_credential(slot);
        }
    }

    // Drop CTAP2.1 large-blob storage and config flags, and revert the PIN floor.
    fido2_storage_config_reset();
    pin_storage_set_min_pin_floor(0);

    LOG_I(TAG, "Factory reset complete");
    return true;
}

/**
 * \brief Returns global authentication counter.
 * \return Counter value.
 */
uint32_t fido2_get_auth_counter(void) {
    return fido2_storage_counter_get();
}

/**
 * \brief Increments global authentication counter.
 */
void fido2_increment_auth_counter(void) {
    fido2_storage_counter_increment();
}

/**
 * \brief Indicates whether FIDO2 subsystem is initialized.
 * \return `true` when initialized, otherwise `false`.
 */
bool fido2_is_initialized(void) {
    return g_fido2.initialized;
}

/**
 * \brief Returns number of free credential slots.
 * \return Available slot count.
 */
uint8_t fido2_get_available_slots(void) {
    uint16_t ecc_start = fido2_storage_ecc_start();
    uint16_t ecc_end = fido2_storage_ecc_end();
    if (ecc_end < ecc_start) return 0;
    uint16_t total = static_cast<uint16_t>(ecc_end - ecc_start + 1);
    uint8_t used = fido2_storage_count();
    return (total > used) ? static_cast<uint8_t>(total - used) : 0;
}
