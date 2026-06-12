#include "cdc_core/FactoryReset.h"
#include "cdc_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"

static const char* TAG = "FactoryReset";

namespace cdc::core {

TropicWipeResult wipeTropic(hal::ISecureElement* se,
                             uint16_t progressEvery,
                             void (*onRmemProgress)(uint16_t, uint16_t)) {
    TropicWipeResult result;

    if (!se || !se->isSessionActive()) {
        LOG_E(TAG, "Cannot wipe TROPIC01: SE session unavailable");
        return result;
    }
    result.sessionReady = true;

    for (uint8_t slot = 0; slot < hal::ISecureElement::ECC_SLOT_COUNT; ++slot) {
        if (!se->eccSlotUsed(slot)) continue;
        if (se->eccDelete(slot) == hal::SeResult::OK) {
            result.eccDeleted++;
        }
    }

    const uint16_t total = hal::ISecureElement::RMEM_SLOT_COUNT;
    for (uint16_t slot = 0; slot < total; ++slot) {
        if (progressEvery && onRmemProgress && (slot % progressEvery) == 0) {
            onRmemProgress(slot, total);
        }
        if (!se->rmemSlotUsed(slot)) continue;
        if (se->rmemErase(slot) == hal::SeResult::OK) {
            result.rmemDeleted++;
        }
    }
    if (progressEvery && onRmemProgress) {
        onRmemProgress(total, total);
    }

    return result;
}

esp_err_t wipeNvs() {
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        LOG_E(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_flash_init();
    if (err != ESP_OK) {
        LOG_E(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

void selfDestruct() {
    nvs_handle_t handle = 0;
    if (nvs_open(kBootProfileNs, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, kBootProfileKey);
        nvs_commit(handle);
        nvs_close(handle);
    }
    esp_restart();
}

} // namespace cdc::core
