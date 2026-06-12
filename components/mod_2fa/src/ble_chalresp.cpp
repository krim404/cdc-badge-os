#include "mod_2fa/ble_chalresp.h"
#include "mod_2fa/OathStore.h"
#include "cdc_core/IChallengeResponder.h"
#include "cdc_core/Raii.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_ui/I18n.h"
#include "cdc_views/ConfirmView.h"
#include "cdc_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdio>

using namespace cdc::hal;

static const char* TAG = "2FA_CR_BLE";

namespace cdc::mod_2fa {

/**
 * \brief CR service UUID: `8E2F1F30-8B5D-4D7A-9A6E-4C9D6A8B1A01` (little-endian).
 */
static const uint8_t CR_SVC_UUID[16] = {
    0x01, 0x1A, 0x8B, 0x6A, 0x9D, 0x4C, 0x6E, 0x9A,
    0x7A, 0x4D, 0x5D, 0x8B, 0x30, 0x1F, 0x2F, 0x8E
};

/**
 * \brief Challenge characteristic UUID (`...1F31...`), write.
 */
static const uint8_t CR_CHALLENGE_UUID[16] = {
    0x01, 0x1A, 0x8B, 0x6A, 0x9D, 0x4C, 0x6E, 0x9A,
    0x7A, 0x4D, 0x5D, 0x8B, 0x31, 0x1F, 0x2F, 0x8E
};

/**
 * \brief Response characteristic UUID (`...1F32...`), read + notify.
 */
static const uint8_t CR_RESPONSE_UUID[16] = {
    0x01, 0x1A, 0x8B, 0x6A, 0x9D, 0x4C, 0x6E, 0x9A,
    0x7A, 0x4D, 0x5D, 0x8B, 0x32, 0x1F, 0x2F, 0x8E
};

static constexpr uint16_t INVALID_HANDLE = 0xFFFF;

/// Max challenge payload (entry name + NUL + challenge bytes) accepted per write.
static constexpr size_t MAX_NAME_LEN = 16;
static constexpr size_t MAX_CHALLENGE_LEN = 128;
static constexpr size_t MAX_FRAME_LEN = MAX_NAME_LEN + 1 + MAX_CHALLENGE_LEN;

static bool s_initialized = false;

static uint16_t s_challenge_handle = 0;
static uint16_t s_response_handle = 0;

static GattCharacteristic s_gattChars[2] = {};
static GattServiceDef s_gattSvcDef = {};

static IBluetoothController::ListenerToken s_tokDisconn = IBluetoothController::INVALID_LISTENER;

static SemaphoreHandle_t s_mutex = nullptr;

/**
 * \brief Pending challenge handed from the BLE host task to the main task.
 *
 * The challenge bytes live in a separate PSRAM buffer (\ref s_challengeBuf)
 * shared with the in-progress request; only one request is processed at a time.
 */
struct PendingChallenge {
    bool valid = false;
    uint16_t connHandle = INVALID_HANDLE;
    char name[MAX_NAME_LEN + 1] = {};
    size_t challengeLen = 0;
};
EXT_RAM_BSS_ATTR static uint8_t s_challengeBuf[MAX_CHALLENGE_LEN] = {};
static PendingChallenge s_pending = {};

/**
 * \brief Response awaiting an on-device touch confirmation.
 */
struct PendingConfirm {
    bool awaiting = false;
    uint16_t connHandle = INVALID_HANDLE;
    uint8_t response[core::IChallengeResponder::MAX_RESPONSE_LEN] = {};
    size_t responseLen = 0;
};
static PendingConfirm s_confirm = {};

/**
 * \brief Returns shared Bluetooth controller instance.
 * \return Controller pointer or `nullptr`.
 */
static IBluetoothController* getBle() {
    return getBluetoothControllerInstance();
}

/**
 * \brief Sends a response notification to the requesting connection.
 * \param connHandle Connection handle.
 * \param data Response bytes.
 * \param len Response length.
 */
static void notifyResponse(uint16_t connHandle, const uint8_t* data, size_t len) {
    auto* ble = getBle();
    if (!ble || s_response_handle == 0 || connHandle == INVALID_HANDLE) return;
    ble->sendNotification(connHandle, s_response_handle, data, static_cast<uint16_t>(len));
}

/**
 * \brief Computes and notifies the response stashed in \ref s_confirm.
 *
 * Runs on the main task (touch-confirm callback or direct tick path).
 */
static void deliverConfirmedResponse() {
    core::MutexGuard guard(s_mutex);
    if (!s_confirm.awaiting) return;
    notifyResponse(s_confirm.connHandle, s_confirm.response, s_confirm.responseLen);
    LOG_I(TAG, "CR response notified (%u bytes)", static_cast<unsigned>(s_confirm.responseLen));
    s_confirm = {};
}

/**
 * \brief Touch-confirm accepted: notify the prepared response.
 * \param userData Unused.
 */
static void onTouchConfirm(void* userData) {
    (void)userData;
    deliverConfirmedResponse();
}

/**
 * \brief Touch-confirm declined: discard the prepared response.
 * \param userData Unused.
 */
static void onTouchCancel(void* userData) {
    (void)userData;
    core::MutexGuard guard(s_mutex);
    LOG_W(TAG, "CR request declined by user");
    s_confirm = {};
}

/**
 * \brief GATT challenge write callback (BLE host task).
 *
 * Records the request only; the response is computed on the main task in
 * \ref ble_chalresp_tick so the touch-confirm UI is never driven from here.
 *
 * \param connHandle Connection handle.
 * \param data Frame bytes: NUL-terminated entry name then challenge.
 * \param len Frame length.
 * \return GATT status (0 = success).
 */
static int onChallengeWrite(uint16_t connHandle, uint16_t, const uint8_t* data, uint16_t len) {
    if (!data || len == 0 || len > MAX_FRAME_LEN) return 0;

    // Split the frame into a NUL-terminated name prefix and the challenge tail.
    size_t nameLen = 0;
    while (nameLen < len && data[nameLen] != '\0') nameLen++;
    if (nameLen == 0 || nameLen > MAX_NAME_LEN || nameLen >= len) {
        LOG_W(TAG, "Malformed CR frame");
        return 0;
    }
    const uint8_t* challenge = data + nameLen + 1;
    size_t challengeLen = len - nameLen - 1;
    if (challengeLen > MAX_CHALLENGE_LEN) return 0;

    core::MutexGuard guard(s_mutex);
    if (s_pending.valid || s_confirm.awaiting) {
        LOG_W(TAG, "CR request already in progress");
        return 0;
    }
    memcpy(s_pending.name, data, nameLen);
    s_pending.name[nameLen] = '\0';
    memcpy(s_challengeBuf, challenge, challengeLen);
    s_pending.challengeLen = challengeLen;
    s_pending.connHandle = connHandle;
    s_pending.valid = true;
    return 0;
}

/**
 * \brief Registers the CR GATT service and characteristics.
 * \return `true` on success.
 */
static bool registerGattService() {
    auto* ble = getBle();
    if (!ble) return false;

    s_gattChars[0].uuid = BleUuid::from128(CR_CHALLENGE_UUID);
    s_gattChars[0].properties = GattProp::WRITE;
    s_gattChars[0].permissions = GattPerm::WRITE;
    s_gattChars[0].valueHandle = &s_challenge_handle;
    s_gattChars[0].onWrite = onChallengeWrite;

    s_gattChars[1].uuid = BleUuid::from128(CR_RESPONSE_UUID);
    s_gattChars[1].properties = GattProp::READ | GattProp::NOTIFY;
    s_gattChars[1].permissions = GattPerm::READ;
    s_gattChars[1].valueHandle = &s_response_handle;
    s_gattChars[1].onRead = [](uint16_t, uint16_t, uint8_t* buf, uint16_t* len) -> int {
        // The response is delivered by notification; a plain read returns empty.
        if (buf && len) *len = 0;
        return 0;
    };

    s_gattSvcDef.uuid = BleUuid::from128(CR_SVC_UUID);
    s_gattSvcDef.characteristics = s_gattChars;
    s_gattSvcDef.numCharacteristics = 2;

    if (!ble->registerGattService(s_gattSvcDef)) {
        LOG_E(TAG, "Failed to register CR GATT service");
        return false;
    }
    LOG_I(TAG, "CR GATT service registered");
    return true;
}

/**
 * \brief Clears pending request state if the requesting peer disconnects.
 * \param connHandle Disconnected handle.
 * \param reason BLE disconnect reason (unused).
 */
static void onDisconnect(uint16_t connHandle, int reason) {
    (void)reason;
    core::MutexGuard guard(s_mutex);
    if (s_pending.valid && s_pending.connHandle == connHandle) {
        s_pending = {};
    }
    if (s_confirm.awaiting && s_confirm.connHandle == connHandle) {
        s_confirm = {};
    }
}

/**
 * \brief Initializes the BLE CR subsystem and registers the GATT service.
 * \return `true` on success.
 */
bool ble_chalresp_init() {
    if (s_initialized) return true;

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            LOG_E(TAG, "Failed to create mutex");
            return false;
        }
    }

    auto* ble = getBle();
    if (!ble) {
        LOG_E(TAG, "Bluetooth controller not available");
        return false;
    }

    if (!ble->isEnabled()) {
        if (!ble->enable()) {
            LOG_E(TAG, "Bluetooth enable failed");
            return false;
        }
    }

    for (int i = 0; i < 50 && !ble->isEnabled(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!registerGattService()) {
        return false;
    }

    s_tokDisconn = ble->addDisconnectionCallback(onDisconnect);

    s_initialized = true;
    LOG_I(TAG, "BLE CR service initialized");
    return true;
}

/**
 * \brief Tears down the BLE CR subsystem and removes GATT callbacks.
 */
void ble_chalresp_deinit() {
    auto* ble = getBle();
    if (ble) {
        ble->unregisterGattService(BleUuid::from128(CR_SVC_UUID));
        ble->removeDisconnectionCallback(s_tokDisconn);
    }
    s_tokDisconn = IBluetoothController::INVALID_LISTENER;

    core::MutexGuard guard(s_mutex);
    s_pending = {};
    s_confirm = {};
    s_initialized = false;
}

/**
 * \brief Main-task tick: processes a pending challenge (confirm + notify).
 * \param nowMs Current uptime in milliseconds.
 */
void ble_chalresp_tick(uint32_t nowMs) {
    (void)nowMs;
    if (!s_initialized) return;

    char name[MAX_NAME_LEN + 1] = {};
    uint8_t challenge[MAX_CHALLENGE_LEN] = {};
    size_t challengeLen = 0;
    uint16_t connHandle = INVALID_HANDLE;
    {
        core::MutexGuard guard(s_mutex);
        if (!s_pending.valid || s_confirm.awaiting) return;
        strncpy(name, s_pending.name, sizeof(name) - 1);
        challengeLen = s_pending.challengeLen;
        memcpy(challenge, s_challengeBuf, challengeLen);
        connHandle = s_pending.connHandle;
        s_pending.valid = false;
    }

    // Compute via the store directly: it lives in this module and is the only
    // path that also reports the per-entry touch flag in a single lookup. The
    // IChallengeResponder service is the contract for external transports.
    uint8_t response[core::IChallengeResponder::MAX_RESPONSE_LEN] = {};
    bool touchRequired = true;
    int rc = OathStore::instance().challengeResponse(name, challenge, challengeLen,
                                                     response, &touchRequired);
    if (rc <= 0) {
        LOG_W(TAG, "CR compute failed for '%s'", name);
        return;
    }

    {
        core::MutexGuard guard(s_mutex);
        s_confirm.awaiting = true;
        s_confirm.connHandle = connHandle;
        memcpy(s_confirm.response, response, static_cast<size_t>(rc));
        s_confirm.responseLen = static_cast<size_t>(rc);
    }

    if (touchRequired) {
        ui::showConfirm(ui::tr("mod_2fa.cr_confirm"), onTouchConfirm, onTouchCancel,
                        ui::ConfirmView::Icon::QUESTION, nullptr);
    } else {
        deliverConfirmedResponse();
    }
}

} // namespace cdc::mod_2fa
