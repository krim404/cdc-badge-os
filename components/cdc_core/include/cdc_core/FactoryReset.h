#pragma once

#include <cstdint>
#include "esp_err.h"
#include "cdc_hal/ISecureElement.h"

namespace cdc::core {

/// NVS namespace and key of the build-profile marker. The boot path
/// (main.cpp) seeds this key after a completed factory wipe and treats its
/// absence on the next boot as a trigger to wipe NVS and TROPIC01. Shared so
/// the self-destruct trigger erases exactly the key the boot path reads.
inline constexpr const char* kBootProfileNs  = "boot_profile";
inline constexpr const char* kBootProfileKey = "profile";

struct TropicWipeResult {
    uint16_t eccDeleted   = 0;
    uint16_t rmemDeleted  = 0;
    bool     sessionReady = false;
};

/**
 * \brief Iterates every TROPIC01 ECC slot (0..ECC_SLOT_COUNT-1) and R-Memory
 *        slot (0..RMEM_SLOT_COUNT-1), deleting whatever is currently
 *        populated. Sets `sessionReady=false` and returns immediately if no
 *        active SE session is available.
 *
 * \param se Secure element instance.
 * \param progressEvery When non-zero, `onRmemProgress` is invoked every
 *        `progressEvery` R-Memory slots and once on completion. Ignored when
 *        `onRmemProgress` is null.
 * \param onRmemProgress Optional progress callback receiving `(current, total)`.
 * \return Wipe statistics.
 */
TropicWipeResult wipeTropic(hal::ISecureElement* se,
                             uint16_t progressEvery = 0,
                             void (*onRmemProgress)(uint16_t current, uint16_t total) = nullptr);

/**
 * \brief Erases the NVS partition and re-initializes it blank.
 * \return ESP_OK on success, propagated error otherwise.
 */
esp_err_t wipeNvs();

/**
 * \brief Triggers a full factory wipe on the next boot and restarts.
 *
 * Erases the build-profile marker (\ref kBootProfileNs / \ref kBootProfileKey)
 * from NVS, commits, then reboots. The boot path detects the absent marker and
 * wipes all NVS plus every TROPIC01 ECC/R-Memory slot before reseeding it.
 * The function does not return.
 */
[[noreturn]] void selfDestruct();

} // namespace cdc::core
