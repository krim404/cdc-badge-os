/**
 * \file
 * \brief Shared HID keyboard provider base: Unicode-method NVS persistence.
 */

#include "cdc_keyboard/KeyboardProviderBase.h"
#include "nvs_flash.h"
#include "nvs.h"

namespace cdc::keyboard {

/** \brief NVS key holding the persisted UnicodeMethod byte. */
static constexpr const char* NVS_KEY_UNICODE = "unicode";

void KeyboardProviderBase::loadSettings() {
    nvs_handle_t handle;
    if (nvs_open(nvsNamespace_, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t method = 0;
        if (nvs_get_u8(handle, NVS_KEY_UNICODE, &method) == ESP_OK) {
            engine_.setUnicodeMethod(static_cast<UnicodeMethod>(method));
        }
        nvs_close(handle);
    }
}

void KeyboardProviderBase::saveSettings() {
    nvs_handle_t handle;
    if (nvs_open(nvsNamespace_, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, NVS_KEY_UNICODE,
                   static_cast<uint8_t>(engine_.getUnicodeMethod()));
        nvs_commit(handle);
        nvs_close(handle);
    }
}

} // namespace cdc::keyboard
