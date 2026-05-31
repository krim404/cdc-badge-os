/**
 * BLE vCard Exchange Implementation
 *
 * Uses IBluetoothController API exclusively - no direct NimBLE dependency.
 *
 * Protocol:
 * - Advertising contains device name and optional minicard (slogan)
 * - GATT service allows reading/writing full vCards for exchange
 */

#include "mod_vcard/ble_vcard.h"
#include "mod_vcard/vcard_store.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <cstring>
#include <cstdio>

using namespace cdc::hal;

static const char* TAG = "VCARD_BLE";

/**
 * \brief Service and characteristic UUID definitions (little-endian byte arrays).
 */

/**
 * \brief vCard service UUID: `8E2F1F20-8B5D-4D7A-9A6E-4C9D6A8B1A01`.
 */
static const uint8_t VCARD_SVC_UUID[16] = {
    0x01, 0x1A, 0x8B, 0x6A, 0x9D, 0x4C, 0x6E, 0x9A,
    0x7A, 0x4D, 0x5D, 0x8B, 0x20, 0x1F, 0x2F, 0x8E
};

/**
 * \brief vCard Data characteristic UUID (`...1F21...`) for reading own vCard.
 */
static const uint8_t VCARD_DATA_UUID[16] = {
    0x01, 0x1A, 0x8B, 0x6A, 0x9D, 0x4C, 0x6E, 0x9A,
    0x7A, 0x4D, 0x5D, 0x8B, 0x21, 0x1F, 0x2F, 0x8E
};

/**
 * \brief vCard RX characteristic UUID (`...1F22...`) for receiving peer vCard data.
 */
static const uint8_t VCARD_RX_UUID[16] = {
    0x01, 0x1A, 0x8B, 0x6A, 0x9D, 0x4C, 0x6E, 0x9A,
    0x7A, 0x4D, 0x5D, 0x8B, 0x22, 0x1F, 0x2F, 0x8E
};

/**
 * \brief Control characteristic UUID (`...1F23...`) for exchange commands.
 */
static const uint8_t VCARD_CTRL_UUID[16] = {
    0x01, 0x1A, 0x8B, 0x6A, 0x9D, 0x4C, 0x6E, 0x9A,
    0x7A, 0x4D, 0x5D, 0x8B, 0x23, 0x1F, 0x2F, 0x8E
};

/**
 * \brief Status characteristic UUID (`...1F24...`) for consent notifications.
 */
static const uint8_t VCARD_STAT_UUID[16] = {
    0x01, 0x1A, 0x8B, 0x6A, 0x9D, 0x4C, 0x6E, 0x9A,
    0x7A, 0x4D, 0x5D, 0x8B, 0x24, 0x1F, 0x2F, 0x8E
};

/**
 * \brief Protocol constants and command/status enums.
 */

enum VcardControlCmd : uint8_t {
    CMD_REQUEST_EXCHANGE = 0x01,
    CMD_CANCEL           = 0x02,
};

enum VcardStatusValue : uint8_t {
    STATUS_ACCEPTED = 0x01,
    STATUS_DECLINED = 0x02,
    STATUS_BUSY     = 0x03,
    STATUS_TIMEOUT  = 0x04,
};

// 0xFFFF is the reserved test/placeholder Bluetooth SIG company identifier.
// A real Company ID must be acquired from the SIG before shipping.
static constexpr uint16_t VCARD_COMPANY_ID = 0xFFFF;
static constexpr uint8_t VCARD_ADV_TYPE = 0x01;
static constexpr uint16_t INVALID_HANDLE = 0xFFFF;

/**
 * \brief Runtime state flags.
 */

static bool s_initialized = false;
static bool s_adv_enabled = false;
static bool s_scan_enabled = false;
static bool s_receive_enabled = false;
static bool s_exchange_enabled = false;
static bool s_exchange_in_progress = false;
static bool s_exchange_result_ready = false;
static bool s_exchange_result = false;
static bool s_scan_pending_process = false;

/**
 * \brief Runtime configuration parameters.
 */
static uint32_t s_adv_interval_ms = 1000;
static uint32_t s_scan_interval_ms = 5000;
static uint32_t s_scan_pause_ms = 10000;
static int8_t s_rssi_threshold = -80;

/**
 * \brief Peer-discovery storage and synchronization primitives.
 */
static constexpr uint16_t MAX_PEERS = 32;
EXT_RAM_BSS_ATTR static vcard_peer_t s_peers[MAX_PEERS] = {};
static uint16_t s_peer_count = 0;
static SemaphoreHandle_t s_peer_mutex = nullptr;

/**
 * \brief Most recent nearby peer snapshot for notification use.
 */
static vcard_peer_t s_nearby_peer = {};
static bool s_nearby_available = false;

/**
 * \brief TX/RX buffers for vCard exchange payloads.
 */
EXT_RAM_BSS_ATTR static char s_tx_vcard[VCARD_MAX_LEN] = {};
EXT_RAM_BSS_ATTR static char s_rx_vcard[VCARD_MAX_LEN] = {};
static size_t s_rx_offset = 0;

/**
 * \brief Local GATT server attribute handles populated during registration.
 */
static uint16_t s_data_handle = 0;
static uint16_t s_rx_handle = 0;
static uint16_t s_control_handle = 0;
static uint16_t s_status_handle = 0;

/**
 * \brief Exchange state machine tracking fields.
 */
static vcard_exchange_state_t s_exchange_state = VCARD_EXCHANGE_IDLE;
static char s_exchange_error[64] = {};
static uint32_t s_state_start_ms = 0;

/**
 * \brief Client-role connection state for outbound exchange attempts.
 */
static uint16_t s_client_conn_handle = INVALID_HANDLE;
static uint8_t s_target_addr[6] = {};
static uint8_t s_target_addr_type = 0;

/**
 * \brief Remote characteristic handles discovered in client role.
 */
static uint16_t s_remote_data_handle = 0;
static uint16_t s_remote_rx_handle = 0;
static uint16_t s_remote_control_handle = 0;
static uint16_t s_remote_status_handle = 0;
static uint16_t s_remote_status_cccd_handle = 0;

/**
 * \brief Server-role consent state for inbound exchange requests.
 */
static bool s_consent_pending = false;
static char s_consent_peer_name[VCARD_BLE_NAME_MAX] = {};
static uint16_t s_consent_conn_handle = INVALID_HANDLE;

/**
 * \brief Application callback hooks.
 */
static vcard_consent_callback_t s_consent_callback = nullptr;
static vcard_exchange_complete_callback_t s_exchange_complete_callback = nullptr;

/**
 * \brief Guards exchange state machine fields against concurrent GAP events
 *        and user-facing API calls.
 */
static SemaphoreHandle_t s_exchange_mutex = nullptr;

/**
 * \brief RAII helper that takes/releases s_exchange_mutex.
 */
struct ExchangeLock {
    bool held = false;
    ExchangeLock() {
        if (s_exchange_mutex && xSemaphoreTake(s_exchange_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            held = true;
        }
    }
    ~ExchangeLock() {
        if (held && s_exchange_mutex) xSemaphoreGive(s_exchange_mutex);
    }
};

/**
 * \brief Timeout constants for connection, discovery, consent, and operations.
 */
static constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
static constexpr uint32_t DISCOVERY_TIMEOUT_MS = 5000;
static constexpr uint32_t CONSENT_TIMEOUT_MS = 30000;
static constexpr uint32_t OPERATION_TIMEOUT_MS = 10000;

/**
 * \brief GATT service/characteristic descriptors kept alive for service lifetime.
 */
static GattCharacteristic s_gattChars[4] = {};
static GattServiceDef s_gattSvcDef = {};

/**
 * \brief Listener tokens for cleanup in ble_vcard_deinit.
 */
static IBluetoothController::ListenerToken s_tokConn = IBluetoothController::INVALID_LISTENER;
static IBluetoothController::ListenerToken s_tokDisconn = IBluetoothController::INVALID_LISTENER;
static IBluetoothController::ListenerToken s_tokSvcDisc = IBluetoothController::INVALID_LISTENER;
static IBluetoothController::ListenerToken s_tokCharRead = IBluetoothController::INVALID_LISTENER;
static IBluetoothController::ListenerToken s_tokNotify = IBluetoothController::INVALID_LISTENER;
static IBluetoothController::ListenerToken s_tokWriteComplete = IBluetoothController::INVALID_LISTENER;

/**
 * \brief Internal forward declarations.
 */

static void setExchangeError(const char* error);
static void completeExchange(bool success);
static void sendExchangeRequest();
static void readPeerVcard();
static void writePeerVcard();
static void processScanResults();

/**
 * \brief Internal helper functions.
 */

/**
 * \brief Returns shared Bluetooth controller instance.
 * \return Controller pointer or `nullptr`.
 */
static IBluetoothController* getBle() {
    return getBluetoothControllerInstance();
}

/**
 * \brief Sends status notification to peer through status characteristic.
 * \param connHandle Connection handle.
 * \param status Protocol status byte.
 */
static void sendStatusNotification(uint16_t connHandle, uint8_t status) {
    auto* ble = getBle();
    if (!ble || s_status_handle == 0 || connHandle == INVALID_HANDLE) return;
    ble->sendNotification(connHandle, s_status_handle, &status, 1);
}

/**
 * \brief GATT client callback handlers.
 */

/**
 * \brief Handles service-discovery completion for client-side exchange.
 * \param connHandle Connection handle.
 * \param service Discovered service data.
 * \param complete `true` when discovery is complete.
 */
static void onServiceDiscovered(uint16_t connHandle,
                                 const IBluetoothController::DiscoveredService* service,
                                 bool complete) {
    // Service-discovery callbacks are shared across all GATT-client users, so
    // ignore any discovery that is not part of our own active client exchange.
    if (s_exchange_state != VCARD_EXCHANGE_DISCOVERING ||
        connHandle != s_client_conn_handle) {
        return;
    }
    if (!complete) return;

    if (!service) {
        setExchangeError("Service not found");
        return;
    }

    BleUuid dataUuid = BleUuid::from128(VCARD_DATA_UUID);
    BleUuid rxUuid   = BleUuid::from128(VCARD_RX_UUID);
    BleUuid ctrlUuid = BleUuid::from128(VCARD_CTRL_UUID);
    BleUuid statUuid = BleUuid::from128(VCARD_STAT_UUID);

    s_remote_data_handle = 0;
    s_remote_rx_handle = 0;
    s_remote_control_handle = 0;
    s_remote_status_handle = 0;
    s_remote_status_cccd_handle = 0;

    for (uint8_t i = 0; i < service->numCharacteristics; i++) {
        const auto& chr = service->characteristics[i];
        if (chr.uuid == dataUuid)      s_remote_data_handle = chr.valueHandle;
        else if (chr.uuid == rxUuid)   s_remote_rx_handle = chr.valueHandle;
        else if (chr.uuid == ctrlUuid) s_remote_control_handle = chr.valueHandle;
        else if (chr.uuid == statUuid) {
            s_remote_status_handle = chr.valueHandle;
            // The CCCD is typically at valueHandle + 1 but the spec does not
            // guarantee this layout. A real implementation must enumerate
            // descriptors via ble_gattc_disc_all_dscs and locate UUID 0x2902.
            s_remote_status_cccd_handle = chr.valueHandle + 1;
        }
    }

    LOG_I(TAG, "Discovered chars: data=%d rx=%d ctrl=%d status=%d",
          s_remote_data_handle, s_remote_rx_handle,
          s_remote_control_handle, s_remote_status_handle);

    if (s_remote_data_handle == 0 || s_remote_control_handle == 0) {
        setExchangeError("Required chars not found");
        return;
    }

    auto* ble = getBle();
    if (ble && s_remote_status_cccd_handle != 0) {
        ble->enableNotifications(connHandle, s_remote_status_cccd_handle);
    }

    sendExchangeRequest();
}

/**
 * \brief Handles peer vCard read completion.
 * \param connHandle Connection handle.
 * \param attrHandle Attribute handle.
 * \param data Received payload.
 * \param len Payload length.
 */
static void onCharacteristicRead(uint16_t connHandle, uint16_t attrHandle,
                                   const uint8_t* data, uint16_t len) {
    (void)connHandle;
    (void)attrHandle;

    if (s_exchange_state != VCARD_EXCHANGE_READING_PEER_VCARD) return;

    if (data && len > 0 && len < sizeof(s_rx_vcard)) {
        memcpy(s_rx_vcard, data, len);
        s_rx_vcard[len] = '\0';

        LOG_I(TAG, "Received peer vCard (%d bytes)", len);

        char err[64];
        if (vcard_store_add(s_rx_vcard, len, err, sizeof(err))) {
            LOG_I(TAG, "Peer vCard stored");
        } else {
            LOG_W(TAG, "Failed to store peer vCard: %s", err);
        }

        s_exchange_state = VCARD_EXCHANGE_WRITING_OWN_VCARD;
        s_state_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        writePeerVcard();
    } else {
        setExchangeError("Invalid vCard data");
    }
}

/**
 * \brief Handles status notifications from remote peer.
 * \param connHandle Connection handle.
 * \param attrHandle Attribute handle.
 * \param data Notification payload.
 * \param len Payload length.
 */
static void onNotification(uint16_t connHandle, uint16_t attrHandle,
                             const uint8_t* data, uint16_t len) {
    (void)attrHandle;

    if (connHandle != s_client_conn_handle ||
        s_exchange_state != VCARD_EXCHANGE_AWAITING_REMOTE_CONSENT) return;

    if (!data || len < 1) return;

    switch (data[0]) {
        case STATUS_ACCEPTED:
            LOG_I(TAG, "Exchange accepted by peer");
            s_exchange_state = VCARD_EXCHANGE_READING_PEER_VCARD;
            s_state_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            readPeerVcard();
            break;
        case STATUS_DECLINED:
            LOG_I(TAG, "Exchange declined by peer");
            s_exchange_state = VCARD_EXCHANGE_DECLINED;
            completeExchange(false);
            break;
        case STATUS_BUSY:
            setExchangeError("Peer is busy");
            break;
        case STATUS_TIMEOUT:
            setExchangeError("Peer timed out");
            break;
        default:
            break;
    }
}

/**
 * \brief Handles write completion callbacks for exchange operations.
 * \param connHandle Connection handle.
 * \param attrHandle Attribute handle.
 * \param status Operation status code.
 */
static void onWriteComplete(uint16_t connHandle, uint16_t attrHandle, int status) {
    (void)connHandle;
    (void)attrHandle;

    if (s_exchange_state == VCARD_EXCHANGE_WRITING_OWN_VCARD) {
        if (status == 0) {
            completeExchange(true);
        } else {
            setExchangeError("Failed to write vCard");
        }
    }
}

/**
 * \brief Connection lifecycle callback handlers.
 */

/**
 * \brief Handles connection event during client-side exchange setup.
 * \param connHandle New connection handle.
 */
static void onConnect(uint16_t connHandle) {
    if (s_exchange_state == VCARD_EXCHANGE_CONNECTING) {
        s_client_conn_handle = connHandle;
        s_exchange_state = VCARD_EXCHANGE_DISCOVERING;
        s_state_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        LOG_I(TAG, "Client connected (handle=%d), starting discovery", connHandle);

        auto* ble = getBle();
        if (ble) {
            ble->discoverServiceByUuid(connHandle, BleUuid::from128(VCARD_SVC_UUID));
        }
    }
}

/**
 * \brief Handles disconnect events for both client and consent flows.
 * \param connHandle Disconnected handle.
 * \param reason BLE disconnect reason.
 */
static void onDisconnect(uint16_t connHandle, int reason) {
    if (connHandle == s_client_conn_handle) {
        s_client_conn_handle = INVALID_HANDLE;
        if (s_exchange_state != VCARD_EXCHANGE_IDLE &&
            s_exchange_state != VCARD_EXCHANGE_COMPLETE &&
            s_exchange_state != VCARD_EXCHANGE_ERROR) {
            setExchangeError("Disconnected");
        }
        LOG_I(TAG, "Client disconnected (reason=%d)", reason);
    } else {
        if (connHandle == s_consent_conn_handle) {
            s_consent_pending = false;
            s_consent_conn_handle = INVALID_HANDLE;
        }
        LOG_I(TAG, "Peer disconnected (reason=%d)", reason);
    }
}

/**
 * \brief Exchange flow helper routines.
 */

/**
 * \brief Transitions exchange flow to error state and triggers cleanup.
 * \param error Error message text.
 */
static void setExchangeError(const char* error) {
    s_exchange_state = VCARD_EXCHANGE_ERROR;
    strncpy(s_exchange_error, error ? error : "Unknown error", sizeof(s_exchange_error) - 1);
    s_exchange_error[sizeof(s_exchange_error) - 1] = '\0';
    LOG_E(TAG, "Exchange error: %s", s_exchange_error);

    auto* ble = getBle();
    if (ble && s_client_conn_handle != INVALID_HANDLE) {
        ble->disconnectHandle(s_client_conn_handle);
    }

    completeExchange(false);
}

/**
 * \brief Finalizes exchange flow and reports result through callback.
 * \param success Exchange success flag.
 */
static void completeExchange(bool success) {
    s_exchange_in_progress = false;
    s_exchange_result = success;
    s_exchange_result_ready = true;

    if (success) {
        s_exchange_state = VCARD_EXCHANGE_COMPLETE;
        LOG_I(TAG, "Exchange completed successfully");
    }

    if (s_exchange_complete_callback) {
        s_exchange_complete_callback(success, success ? nullptr : s_exchange_error);
    }

    auto* ble = getBle();
    if (ble && s_client_conn_handle != INVALID_HANDLE) {
        ble->disconnectHandle(s_client_conn_handle);
        s_client_conn_handle = INVALID_HANDLE;
    }
}

/**
 * \brief Sends exchange request command to connected peer.
 */
static void sendExchangeRequest() {
    auto* ble = getBle();
    if (!ble || s_client_conn_handle == INVALID_HANDLE || s_remote_control_handle == 0) {
        setExchangeError("Not connected");
        return;
    }

    uint8_t cmd = CMD_REQUEST_EXCHANGE;
    if (!ble->writeCharacteristic(s_client_conn_handle, s_remote_control_handle,
                                    &cmd, sizeof(cmd), true)) {
        setExchangeError("Failed to send request");
        return;
    }

    s_exchange_state = VCARD_EXCHANGE_AWAITING_REMOTE_CONSENT;
    s_state_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    LOG_I(TAG, "Exchange request sent, waiting for consent");
}

/**
 * \brief Starts read of peer vCard characteristic.
 */
static void readPeerVcard() {
    auto* ble = getBle();
    if (!ble || s_client_conn_handle == INVALID_HANDLE || s_remote_data_handle == 0) {
        setExchangeError("Not connected");
        return;
    }

    if (!ble->readCharacteristic(s_client_conn_handle, s_remote_data_handle)) {
        setExchangeError("Failed to start read");
    }
}

/**
 * \brief Writes local vCard to peer RX characteristic.
 */
static void writePeerVcard() {
    auto* ble = getBle();
    if (!ble || s_client_conn_handle == INVALID_HANDLE || s_remote_rx_handle == 0) {
        setExchangeError("Not connected");
        return;
    }

    size_t len = vcard_store_get_own(s_tx_vcard, sizeof(s_tx_vcard));
    if (len == 0) {
        setExchangeError("No own vCard");
        return;
    }

    if (!ble->writeCharacteristic(s_client_conn_handle, s_remote_rx_handle,
                                    reinterpret_cast<const uint8_t*>(s_tx_vcard),
                                    static_cast<uint16_t>(len), true)) {
        setExchangeError("Failed to write vCard");
        return;
    }

    LOG_I(TAG, "Sent own vCard (%zu bytes)", len);
}

/**
 * \brief Scan result processing helpers.
 */

/**
 * \brief Parses BLE scan cache and updates nearby vCard peer list.
 */
static void processScanResults() {
    auto* ble = getBle();
    if (!ble) return;

    BleScanResult results[16];
    uint8_t count = ble->getScanResults(results, 16);

    for (uint8_t i = 0; i < count; i++) {
        const auto& r = results[i];
        vcard_peer_t peer = {};
        memcpy(peer.addr, r.mac, 6);
        peer.addr_type = r.addrType;
        peer.rssi = r.rssi;
        strncpy(peer.name, r.name, sizeof(peer.name) - 1);

        // Check for vCard service UUID in raw advertising data
        if (BleAdvParser::findServiceUuid128(r.advData, r.advDataLen, VCARD_SVC_UUID)) {
            peer.exchange_ready = true;
        }

        // Check for manufacturer data with vCard minicard
        uint16_t companyId = 0;
        const uint8_t* mfgPayload = nullptr;
        uint8_t mfgLen = 0;
        if (BleAdvParser::findManufacturerData(r.advData, r.advDataLen,
                                                 &companyId, &mfgPayload, &mfgLen)) {
            if (companyId == VCARD_COMPANY_ID && mfgLen >= 1 &&
                mfgPayload[0] == VCARD_ADV_TYPE) {
                size_t sloganLen = mfgLen - 1;
                if (sloganLen > sizeof(peer.slogan) - 1) {
                    sloganLen = sizeof(peer.slogan) - 1;
                }
                memcpy(peer.slogan, mfgPayload + 1, sloganLen);
                peer.slogan[sloganLen] = '\0';
                peer.exchange_ready = true;
            }
        }

        if (!peer.exchange_ready) continue;
        if (peer.rssi < s_rssi_threshold) continue;

        if (xSemaphoreTake(s_peer_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            bool found = false;
            for (uint16_t j = 0; j < s_peer_count; j++) {
                if (memcmp(s_peers[j].addr, peer.addr, 6) == 0) {
                    s_peers[j].rssi = peer.rssi;
                    found = true;
                    break;
                }
            }

            if (!found && s_peer_count < MAX_PEERS) {
                s_peers[s_peer_count++] = peer;
                LOG_I(TAG, "Found vCard peer: %s (RSSI %d)", peer.name, peer.rssi);
                s_nearby_peer = peer;
                s_nearby_available = true;
            }

            xSemaphoreGive(s_peer_mutex);
        }
    }
}

/**
 * \brief GATT service registration via controller API.
 */

/**
 * \brief Registers vCard GATT service and characteristics.
 * \return `true` if registration succeeded.
 */
static bool registerGattService() {
    auto* ble = getBle();
    if (!ble) return false;

    // DATA - Read own vCard
    s_gattChars[0].uuid = BleUuid::from128(VCARD_DATA_UUID);
    s_gattChars[0].properties = GattProp::READ;
    s_gattChars[0].permissions = GattPerm::READ;
    s_gattChars[0].valueHandle = &s_data_handle;
    s_gattChars[0].onRead = [](uint16_t, uint16_t, uint8_t* buf, uint16_t* len) -> int {
        size_t vcLen = vcard_store_get_own(s_tx_vcard, sizeof(s_tx_vcard));
        if (vcLen > 0 && vcLen <= *len) {
            memcpy(buf, s_tx_vcard, vcLen);
            *len = static_cast<uint16_t>(vcLen);
        } else {
            *len = 0;
        }
        return 0;
    };

    // RX - Receive vCard from peer
    s_gattChars[1].uuid = BleUuid::from128(VCARD_RX_UUID);
    s_gattChars[1].properties = GattProp::WRITE;
    s_gattChars[1].permissions = GattPerm::WRITE;
    s_gattChars[1].valueHandle = &s_rx_handle;
    s_gattChars[1].onWrite = [](uint16_t, uint16_t, const uint8_t* data, uint16_t len) -> int {
        if (!s_receive_enabled) return 0x03; // Write not permitted

        if (len > 0 && s_rx_offset + len < sizeof(s_rx_vcard)) {
            memcpy(s_rx_vcard + s_rx_offset, data, len);
            s_rx_offset += len;
            s_rx_vcard[s_rx_offset] = '\0';

            if (strstr(s_rx_vcard, "END:VCARD") != nullptr) {
                LOG_I(TAG, "Received vCard (%zu bytes)", s_rx_offset);
                char err[64];
                if (vcard_store_add(s_rx_vcard, s_rx_offset, err, sizeof(err))) {
                    s_exchange_result = true;
                } else {
                    LOG_W(TAG, "Failed to store vCard: %s", err);
                    s_exchange_result = false;
                }
                s_exchange_result_ready = true;
                s_rx_offset = 0;
            }
        }
        return 0;
    };

    // CONTROL - Exchange commands
    s_gattChars[2].uuid = BleUuid::from128(VCARD_CTRL_UUID);
    s_gattChars[2].properties = GattProp::WRITE;
    s_gattChars[2].permissions = GattPerm::WRITE;
    s_gattChars[2].valueHandle = &s_control_handle;
    s_gattChars[2].onWrite = [](uint16_t connHandle, uint16_t, const uint8_t* data, uint16_t len) -> int {
        if (len < 1) return 0;

        if (data[0] == CMD_REQUEST_EXCHANGE) {
            if (s_consent_pending || s_exchange_in_progress) {
                sendStatusNotification(connHandle, STATUS_BUSY);
                return 0;
            }

            // Peer name from scan list is not easily available here;
            // connHandle-to-address mapping would need API extension
            snprintf(s_consent_peer_name, sizeof(s_consent_peer_name), "BLE Device");

            s_consent_pending = true;
            s_consent_conn_handle = connHandle;
            s_state_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            LOG_I(TAG, "Exchange request from connected device");

            if (s_consent_callback) {
                s_consent_callback(s_consent_peer_name);
            }
        } else if (data[0] == CMD_CANCEL) {
            if (s_consent_pending && s_consent_conn_handle == connHandle) {
                s_consent_pending = false;
                s_consent_conn_handle = INVALID_HANDLE;
                LOG_I(TAG, "Exchange cancelled by peer");
            }
        }
        return 0;
    };

    // STATUS - Consent notifications
    s_gattChars[3].uuid = BleUuid::from128(VCARD_STAT_UUID);
    s_gattChars[3].properties = GattProp::READ | GattProp::NOTIFY;
    s_gattChars[3].permissions = GattPerm::READ;
    s_gattChars[3].valueHandle = &s_status_handle;
    s_gattChars[3].onRead = [](uint16_t, uint16_t, uint8_t* buf, uint16_t* len) -> int {
        buf[0] = 0;
        *len = 1;
        return 0;
    };

    s_gattSvcDef.uuid = BleUuid::from128(VCARD_SVC_UUID);
    s_gattSvcDef.characteristics = s_gattChars;
    s_gattSvcDef.numCharacteristics = 4;

    if (!ble->registerGattService(s_gattSvcDef)) {
        LOG_E(TAG, "Failed to register vCard GATT service");
        return false;
    }

    LOG_I(TAG, "vCard GATT service registered");
    return true;
}

/**
 * \brief Public BLE vCard API implementation.
 */

/**
 * \brief Initializes BLE vCard subsystem and registers GATT service/callbacks.
 * \return `true` on successful initialization.
 */
bool ble_vcard_init(void) {
    if (s_initialized) return true;

    if (!s_peer_mutex) {
        s_peer_mutex = xSemaphoreCreateMutex();
        if (!s_peer_mutex) {
            LOG_E(TAG, "Failed to create mutex");
            return false;
        }
    }
    if (!s_exchange_mutex) {
        s_exchange_mutex = xSemaphoreCreateMutex();
        if (!s_exchange_mutex) {
            LOG_E(TAG, "Failed to create exchange mutex");
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

    // Wait for BLE stack to be ready
    for (int i = 0; i < 50 && !ble->isEnabled(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!registerGattService()) {
        return false;
    }

    // Register GATT client callbacks for exchange using multi-listener API
    s_tokSvcDisc = ble->addServiceDiscoveryCallback(onServiceDiscovered);
    s_tokCharRead = ble->addCharacteristicReadCallback(onCharacteristicRead);
    s_tokNotify = ble->addNotificationCallback(onNotification);
    s_tokWriteComplete = ble->addWriteCompleteCallback(onWriteComplete);

    // Register connection callbacks
    s_tokConn = ble->addConnectionCallback(onConnect);
    s_tokDisconn = ble->addDisconnectionCallback(onDisconnect);

    s_initialized = true;
    LOG_I(TAG, "BLE vCard service initialized");
    return true;
}

/**
 * \brief Deinitializes BLE vCard runtime state.
 */
void ble_vcard_deinit(void) {
    auto* ble = getBle();
    if (ble) {
        ble->removeAdvertisingUuid(BleUuid::from128(VCARD_SVC_UUID));
        ble->clearAdvertisingManufacturerData();
        // Remove all callback registrations so a stale this-state pointer
        // is never dereferenced after the module is torn down.
        ble->removeConnectionCallback(s_tokConn);
        ble->removeDisconnectionCallback(s_tokDisconn);
        ble->removeServiceDiscoveryCallback(s_tokSvcDisc);
        ble->removeCharacteristicReadCallback(s_tokCharRead);
        ble->removeNotificationCallback(s_tokNotify);
        ble->removeWriteCompleteCallback(s_tokWriteComplete);
    }

    s_initialized = false;
    s_adv_enabled = false;
    s_scan_enabled = false;
    s_receive_enabled = false;
    s_exchange_enabled = false;
    s_exchange_in_progress = false;
    s_exchange_result_ready = false;
}

/**
 * \brief Enables or disables advertising of vCard service presence.
 * \param enabled Desired advertising state.
 */
void ble_vcard_set_adv_enabled(bool enabled) {
    s_adv_enabled = enabled;
    auto* ble = getBle();
    if (!ble || !s_initialized) return;

    if (enabled) {
        ble->addAdvertisingUuid(BleUuid::from128(VCARD_SVC_UUID));
    } else {
        ble->removeAdvertisingUuid(BleUuid::from128(VCARD_SVC_UUID));
        ble->clearAdvertisingManufacturerData();
    }
}

/**
 * \brief Returns configured advertising enable state.
 * \return `true` if advertising is configured as enabled.
 */
bool ble_vcard_is_adv_enabled(void) { return s_adv_enabled; }

/**
 * \brief Returns whether vCard advertising is currently active.
 * \return `true` if advertising is running.
 */
bool ble_vcard_is_adv_active(void) {
    auto* ble = getBle();
    return ble && s_adv_enabled && ble->isAdvertising();
}

/**
 * \brief Enables or disables scanning for nearby vCard peers.
 * \param enabled Desired scan state.
 */
void ble_vcard_set_scan_enabled(bool enabled) {
    s_scan_enabled = enabled;
    auto* ble = getBle();
    if (!ble || !s_initialized) return;

    if (enabled) {
        if (xSemaphoreTake(s_peer_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_peer_count = 0;
            memset(s_peers, 0, sizeof(s_peers));
            xSemaphoreGive(s_peer_mutex);
        }
        ble->startScan(s_scan_interval_ms);
        s_scan_pending_process = true;
        LOG_I(TAG, "vCard scan started");
    } else {
        ble->stopScan();
        s_scan_pending_process = false;
    }
}

/**
 * \brief Returns configured scan-enable state.
 * \return `true` if scanning is configured as enabled.
 */
bool ble_vcard_is_scan_enabled(void) { return s_scan_enabled; }

/**
 * \brief Returns whether scan is currently active.
 * \return `true` when scan is running.
 */
bool ble_vcard_is_scan_active(void) {
    auto* ble = getBle();
    return ble && s_scan_enabled && !ble->isScanComplete();
}

/**
 * \brief Enables or disables receiving peer vCards over GATT RX.
 * \param enabled Desired receive state.
 */
void ble_vcard_set_receive_enabled(bool enabled) { s_receive_enabled = enabled; }

/**
 * \brief Returns receive-enable state.
 * \return `true` if receiving is enabled.
 */
bool ble_vcard_is_receive_enabled(void) { return s_receive_enabled; }

/**
 * \brief Enables or disables initiating exchange workflows.
 * \param enabled Desired exchange-enable state.
 */
void ble_vcard_set_exchange_enabled(bool enabled) { s_exchange_enabled = enabled; }

/**
 * \brief Returns exchange-enable state.
 * \return `true` if exchange initiation is enabled.
 */
bool ble_vcard_is_exchange_enabled(void) { return s_exchange_enabled; }

/**
 * \brief Configures scan interval and pause parameters.
 * \param scan_ms Scan duration in milliseconds.
 * \param pause_ms Pause duration in milliseconds.
 */
void ble_vcard_set_scan_interval(uint32_t scan_ms, uint32_t pause_ms) {
    s_scan_interval_ms = scan_ms;
    s_scan_pause_ms = pause_ms;
    (void)s_scan_pause_ms;
}

/**
 * \brief Configures advertising interval parameter.
 * \param interval_ms Advertising interval in milliseconds.
 */
void ble_vcard_set_adv_interval(uint32_t interval_ms) {
    s_adv_interval_ms = interval_ms;
    (void)s_adv_interval_ms;
}

/**
 * \brief Sets minimum RSSI threshold for peer acceptance.
 * \param rssi Threshold in dBm.
 */
void ble_vcard_set_rssi_threshold(int8_t rssi) { s_rssi_threshold = rssi; }

/**
 * \brief Returns configured advertising interval.
 * \return Interval in milliseconds.
 */
uint32_t ble_vcard_get_adv_interval(void) { return s_adv_interval_ms; }

/**
 * \brief Returns configured scan interval.
 * \return Interval in milliseconds.
 */
uint32_t ble_vcard_get_scan_interval(void) { return s_scan_interval_ms; }

/**
 * \brief Copies cached peer list into caller buffer.
 * \param out Output array for peers.
 * \param max_peers Maximum entries writable to `out`.
 * \return Number of copied peers.
 */
uint16_t ble_vcard_get_peers(vcard_peer_t* out, uint16_t max_peers) {
    if (!out || max_peers == 0) return 0;

    uint16_t count = 0;
    if (xSemaphoreTake(s_peer_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = (s_peer_count < max_peers) ? s_peer_count : max_peers;
        memcpy(out, s_peers, count * sizeof(vcard_peer_t));
        xSemaphoreGive(s_peer_mutex);
    }
    return count;
}

/**
 * \brief Starts exchange workflow with target peer address.
 * \param addr Target peer address.
 * \param addr_type Target address type.
 * \return `true` if connection attempt started.
 */
bool ble_vcard_exchange_with(const uint8_t addr[6], uint8_t addr_type) {
    auto* ble = getBle();
    if (!ble || !s_initialized) {
        LOG_E(TAG, "BLE not initialized");
        return false;
    }

    ExchangeLock lock;
    if (s_exchange_state != VCARD_EXCHANGE_IDLE) {
        LOG_W(TAG, "Exchange already in progress");
        return false;
    }

    if (s_scan_enabled && !ble->isScanComplete()) {
        ble->stopScan();
    }

    memcpy(s_target_addr, addr, 6);
    s_target_addr_type = addr_type;

    s_exchange_state = VCARD_EXCHANGE_CONNECTING;
    s_exchange_in_progress = true;
    s_exchange_result_ready = false;
    s_exchange_result = false;
    s_exchange_error[0] = '\0';
    s_state_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (!ble->connect(addr, addr_type)) {
        LOG_E(TAG, "Failed to initiate connection");
        s_exchange_state = VCARD_EXCHANGE_IDLE;
        s_exchange_in_progress = false;
        return false;
    }

    LOG_I(TAG, "Connecting to peer for vCard exchange");
    return true;
}

/**
 * \brief Cancels active exchange workflow.
 * \return `true` after cancellation logic completes.
 */
bool ble_vcard_exchange_cancel(void) {
    ExchangeLock lock;
    if (s_exchange_state == VCARD_EXCHANGE_IDLE) {
        return true;
    }

    s_exchange_state = VCARD_EXCHANGE_CANCELLED;

    auto* ble = getBle();
    if (ble && s_client_conn_handle != INVALID_HANDLE) {
        ble->disconnectHandle(s_client_conn_handle);
        s_client_conn_handle = INVALID_HANDLE;
    }

    s_exchange_in_progress = false;
    s_exchange_state = VCARD_EXCHANGE_IDLE;
    LOG_I(TAG, "Exchange cancelled");
    return true;
}

/**
 * \brief Polls and consumes pending exchange result.
 * \param success Optional output success flag.
 * \return `true` when a result was available.
 */
bool ble_vcard_poll_exchange_result(bool* success) {
    if (!s_exchange_result_ready) return false;
    if (success) *success = s_exchange_result;
    s_exchange_result_ready = false;
    s_exchange_state = VCARD_EXCHANGE_IDLE;
    return true;
}

/**
 * \brief Returns whether exchange is currently in progress.
 * \return `true` if exchange state machine is active.
 */
bool ble_vcard_exchange_in_progress(void) { return s_exchange_in_progress; }

/**
 * \brief Returns current exchange state machine value.
 * \return Exchange state enum.
 */
vcard_exchange_state_t ble_vcard_get_exchange_state(void) { return s_exchange_state; }

/**
 * \brief Returns last exchange error text.
 * \return Error string pointer.
 */
const char* ble_vcard_get_exchange_error(void) { return s_exchange_error; }

/**
 * \brief Returns whether an incoming consent prompt is pending.
 * \return `true` if consent is waiting for user decision.
 */
bool ble_vcard_has_pending_consent(void) { return s_consent_pending; }

/**
 * \brief Copies pending consent peer name to caller buffer.
 * \param out Output text buffer.
 * \param max_len Output buffer size.
 * \return `true` if name was copied.
 */
bool ble_vcard_get_consent_peer_name(char* out, size_t max_len) {
    if (!out || max_len == 0 || !s_consent_pending) return false;
    strncpy(out, s_consent_peer_name, max_len - 1);
    out[max_len - 1] = '\0';
    return true;
}

/**
 * \brief Responds to pending consent request with accept/decline.
 * \param accepted `true` to accept exchange.
 */
void ble_vcard_respond_consent(bool accepted) {
    if (!s_consent_pending || s_consent_conn_handle == INVALID_HANDLE) return;

    if (accepted) {
        LOG_I(TAG, "Consent accepted, enabling receive");
        s_receive_enabled = true;
        sendStatusNotification(s_consent_conn_handle, STATUS_ACCEPTED);
    } else {
        LOG_I(TAG, "Consent declined");
        sendStatusNotification(s_consent_conn_handle, STATUS_DECLINED);
    }

    s_consent_pending = false;
    if (!accepted) {
        s_consent_conn_handle = INVALID_HANDLE;
    }
}

/**
 * \brief Sets callback for incoming consent requests.
 * \param cb Consent callback.
 */
void ble_vcard_set_consent_callback(vcard_consent_callback_t cb) { s_consent_callback = cb; }

/**
 * \brief Sets callback for exchange completion notifications.
 * \param cb Completion callback.
 */
void ble_vcard_set_exchange_complete_callback(vcard_exchange_complete_callback_t cb) { s_exchange_complete_callback = cb; }

/**
 * \brief Periodic state-machine tick for scanning and exchange timeouts.
 * \param now_ms Current uptime in milliseconds.
 */
void ble_vcard_tick(uint32_t now_ms) {
    // Process scan results when scan completes
    auto* ble = getBle();
    if (ble && s_scan_pending_process && ble->isScanComplete()) {
        processScanResults();
        s_scan_pending_process = false;
    }

    // Exchange state timeouts
    if (s_exchange_state != VCARD_EXCHANGE_IDLE &&
        s_exchange_state != VCARD_EXCHANGE_COMPLETE &&
        s_exchange_state != VCARD_EXCHANGE_ERROR &&
        s_exchange_state != VCARD_EXCHANGE_CANCELLED &&
        s_exchange_state != VCARD_EXCHANGE_DECLINED) {

        uint32_t elapsed = now_ms - s_state_start_ms;

        switch (s_exchange_state) {
            case VCARD_EXCHANGE_CONNECTING:
                if (elapsed > CONNECT_TIMEOUT_MS) setExchangeError("Connection timeout");
                break;
            case VCARD_EXCHANGE_DISCOVERING:
                if (elapsed > DISCOVERY_TIMEOUT_MS) setExchangeError("Discovery timeout");
                break;
            case VCARD_EXCHANGE_AWAITING_REMOTE_CONSENT:
                if (elapsed > CONSENT_TIMEOUT_MS) setExchangeError("No response from peer");
                break;
            case VCARD_EXCHANGE_READING_PEER_VCARD:
            case VCARD_EXCHANGE_WRITING_OWN_VCARD:
                if (elapsed > OPERATION_TIMEOUT_MS) setExchangeError("Operation timeout");
                break;
            default:
                break;
        }
    }

    // Server-side consent timeout
    if (s_consent_pending) {
        uint32_t consent_elapsed = now_ms - s_state_start_ms;
        if (consent_elapsed > CONSENT_TIMEOUT_MS) {
            LOG_W(TAG, "Consent timeout, auto-declining");
            ble_vcard_respond_consent(false);
        }
    }
}

/**
 * \brief Polls one newly discovered nearby peer (edge-triggered).
 * \param out Output peer structure.
 * \return `true` when a nearby peer was available.
 */
bool ble_vcard_poll_nearby(vcard_peer_t* out) {
    if (!out || !s_nearby_available) return false;

    if (xSemaphoreTake(s_peer_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = s_nearby_peer;
        s_nearby_available = false;
        xSemaphoreGive(s_peer_mutex);
        return true;
    }
    return false;
}
