/**
 * ESP32 Bluetooth (BLE) Controller Implementation
 * Full BLE stack management with NimBLE (when available)
 *
 * Requires CONFIG_BT_ENABLED and CONFIG_BT_NIMBLE_ENABLED in sdkconfig
 */

#include "cdc_hal/IBluetoothController.h"
#include "cdc_hal/hw_config.h"
#include "cdc_log.h"
#include "sdkconfig.h"

static const char* TAG = "BT-Ctrl";

// Full implementation is compiled only when NimBLE support is enabled.
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_NIMBLE_ENABLED)

#include "esp_bt.h"
#include "esp_mac.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_uuid.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "host/ble_store.h"
#include "store/config/ble_store_config.h"

/** \brief Forward declaration for NimBLE NVS-backed bond store initializer. */
extern "C" void ble_store_config_init(void);
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>
#include <algorithm>

namespace cdc::hal {

/**
 * \brief Forward declaration of the NimBLE host task entry point.
 */
static void bleHostTask(void* param);

/**
 * \brief Forward declarations for central-role GATT client callbacks.
 */
static int gattcSvcDiscCb(uint16_t connHandle, const struct ble_gatt_error* error,
                           const struct ble_gatt_svc* service, void* arg);
static int gattcChrDiscCb(uint16_t connHandle, const struct ble_gatt_error* error,
                           const struct ble_gatt_chr* chr, void* arg);
static int gattcReadCb(uint16_t connHandle, const struct ble_gatt_error* error,
                        struct ble_gatt_attr* attr, void* arg);
static int gattcWriteCb(uint16_t connHandle, const struct ble_gatt_error* error,
                          struct ble_gatt_attr* attr, void* arg);

/**
 * \brief Internal limits for dynamic GATT service registration.
 */

static constexpr uint8_t MAX_REGISTERED_SERVICES = IBluetoothController::MAX_REGISTERED_SERVICES;
/// \brief Last service slot, reserved exclusively for a plugin-registered service.
static constexpr uint8_t PLUGIN_SERVICE_SLOT = MAX_REGISTERED_SERVICES - 1;
static constexpr uint8_t MAX_CHARS_PER_SERVICE = IBluetoothController::MAX_CHARS_PER_SERVICE;
static constexpr uint8_t MAX_DESCRIPTORS_PER_CHAR = 2;
static constexpr uint8_t MAX_ADV_UUIDS = 4;
static constexpr uint8_t MAX_CONN_CALLBACKS = 4;
static constexpr uint8_t MAX_CONNECTIONS = 2;
static constexpr uint8_t MAX_SUBSCRIBE_ENTRIES = 16;
static constexpr uint8_t MAX_BONDS = 5;

/**
 * \brief Persistent NimBLE-side storage for one registered service definition.
 */
struct InternalService {
    bool active = false;

    // NimBLE-native UUID storage
    ble_uuid_any_t svcUuid;
    ble_uuid_any_t charUuids[MAX_CHARS_PER_SERVICE];

    // NimBLE characteristic + service definitions (must persist)
    ble_gatt_chr_def nimbleChars[MAX_CHARS_PER_SERVICE + 1]; // +1 terminator
    ble_gatt_svc_def nimbleSvcs[2];                          // +1 terminator

    // Per-characteristic descriptor backing storage (e.g. HID Report Reference).
    // Packed layout per descriptor slot: [0] = data length, [1..4] = data bytes.
    ble_uuid_any_t dscUuids[MAX_CHARS_PER_SERVICE][MAX_DESCRIPTORS_PER_CHAR];
    ble_gatt_dsc_def dscDefs[MAX_CHARS_PER_SERVICE][MAX_DESCRIPTORS_PER_CHAR + 1];
    uint8_t dscPacked[MAX_CHARS_PER_SERVICE][MAX_DESCRIPTORS_PER_CHAR][5];

    // Callbacks registered by the module
    GattWriteCallback writeCallbacks[MAX_CHARS_PER_SERVICE];
    GattReadCallback readCallbacks[MAX_CHARS_PER_SERVICE];
    uint8_t numChars = 0;
};

static InternalService s_services[MAX_REGISTERED_SERVICES];

/**
 * \brief Converts a generic BLE UUID into NimBLE's `ble_uuid_any_t` format.
 * \param src Source UUID representation.
 * \param dst Destination NimBLE UUID structure.
 * \return void
 */
static void convertUuid(const BleUuid& src, ble_uuid_any_t& dst) {
    if (src.type == BleUuid::UUID_16) {
        dst.u.type = BLE_UUID_TYPE_16;
        dst.u16.u.type = BLE_UUID_TYPE_16;
        dst.u16.value = src.u16;
    } else {
        dst.u.type = BLE_UUID_TYPE_128;
        dst.u128.u.type = BLE_UUID_TYPE_128;
        memcpy(dst.u128.value, src.u128, 16);
    }
}

/**
 * \brief Maps CDC GATT property/permission bits to NimBLE characteristic flags.
 * \param props Characteristic property bitmask (`GattProp`).
 * \param perms Characteristic permission bitmask (`GattPerm`).
 * \return NimBLE `ble_gatt_chr_flags` value.
 */
static ble_gatt_chr_flags mapProperties(uint8_t props, uint8_t perms) {
    ble_gatt_chr_flags flags = 0;
    if (props & GattProp::READ)         flags |= BLE_GATT_CHR_F_READ;
    if (props & GattProp::WRITE)        flags |= BLE_GATT_CHR_F_WRITE;
    if (props & GattProp::WRITE_NO_RSP) flags |= BLE_GATT_CHR_F_WRITE_NO_RSP;
    if (props & GattProp::NOTIFY)       flags |= BLE_GATT_CHR_F_NOTIFY;
    if (props & GattProp::INDICATE)     flags |= BLE_GATT_CHR_F_INDICATE;

    // Encryption requirements
    if (perms & GattPerm::READ_ENC)     flags |= BLE_GATT_CHR_F_READ_ENC;
    if (perms & GattPerm::WRITE_ENC)    flags |= BLE_GATT_CHR_F_WRITE_ENC;

    return flags;
}

/**
 * \brief Dispatches GATT read/write requests to registered characteristic callbacks.
 * \param connHandle Active BLE connection handle.
 * \param attrHandle Accessed attribute handle.
 * \param ctxt NimBLE access context.
 * \param arg Pointer to the owning internal service descriptor.
 * \return NimBLE ATT status code.
 */
/**
 * \brief Shared 512-byte access buffer used in the NimBLE host task only.
 *
 * NimBLE runs all GATT access callbacks on a single host task, so this buffer
 * cannot race with itself. Hoisting it out of `gattServiceAccessCb` reclaims
 * roughly 1 KB of host-task stack per call.
 */
static uint8_t s_gattAccessBuf[512];

static int gattServiceAccessCb(uint16_t connHandle, uint16_t attrHandle,
                                struct ble_gatt_access_ctxt* ctxt, void* arg) {
    auto* svc = static_cast<InternalService*>(arg);
    if (!svc) return BLE_ATT_ERR_UNLIKELY;

    // Find which characteristic was accessed by matching UUID
    for (uint8_t i = 0; i < svc->numChars; i++) {
        if (ble_uuid_cmp(ctxt->chr->uuid, &svc->charUuids[i].u) == 0) {
            if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && svc->writeCallbacks[i]) {
                uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
                if (len > sizeof(s_gattAccessBuf)) len = sizeof(s_gattAccessBuf);
                ble_hs_mbuf_to_flat(ctxt->om, s_gattAccessBuf, len, nullptr);
                return svc->writeCallbacks[i](connHandle, attrHandle, s_gattAccessBuf, len);
            }
            if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && svc->readCallbacks[i]) {
                uint16_t len = sizeof(s_gattAccessBuf);
                int rc = svc->readCallbacks[i](connHandle, attrHandle, s_gattAccessBuf, &len);
                if (rc == 0 && len > 0) {
                    os_mbuf_append(ctxt->om, s_gattAccessBuf, len);
                }
                return rc;
            }
            return 0; // No callback = allow silently
        }
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/**
 * \brief Read callback for static descriptors (e.g. HID Report Reference).
 *
 * The `arg` is interpreted as packed (data length << 8) | (data byte 0..3 word).
 * We rely on the descriptor data being copied into `InternalService::dscData`
 * and referenced by a `void*` pointer stored in `ble_gatt_dsc_def::arg`.
 */
static int gattStaticDescriptorAccessCb(uint16_t connHandle, uint16_t attrHandle,
                                          struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)connHandle;
    (void)attrHandle;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) return BLE_ATT_ERR_UNLIKELY;
    if (!arg) return BLE_ATT_ERR_UNLIKELY;

    // Layout: [0] = length, [1..4] = bytes
    const uint8_t* packed = static_cast<const uint8_t*>(arg);
    uint8_t len = packed[0];
    return os_mbuf_append(ctxt->om, packed + 1, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/**
 * ESP32 Bluetooth Controller Implementation using NimBLE
 */
class BluetoothController : public IBluetoothController {
public:
    BluetoothController() { instance_ = this; }

    /**
     * \name IService implementation
     * \{
     */
    bool init() override;
    bool start() override;
    void stop() override;
    core::ServiceState getState() const override { return state_; }
    const char* getName() const override { return "bluetooth"; }
    /** \} */

    /**
     * \name IBluetoothController implementation
     * \{
     */
    bool enable() override;
    void disable() override;
    bool isEnabled() const override { return enabled_; }
    bool getMacAddress(uint8_t* mac) const override;
    void setDeviceName(const char* name) override;
    const char* getDeviceName() const override { return deviceName_; }
    bool isConnected() const override;
    void disconnect() override;
    int8_t getRssi() const override;

    /**
     * \name Advertising
     * \{
     */
    void startAdvertising() override;
    void stopAdvertising() override;
    bool isAdvertising() const override { return advertising_; }
    bool addAdvertisingUuid(const BleUuid& uuid) override;
    void removeAdvertisingUuid(const BleUuid& uuid) override;
    bool setAdvertisingManufacturerData(uint16_t companyId,
                                         const uint8_t* data, uint16_t len) override;
    void clearAdvertisingManufacturerData() override;
    /** \} */

    /**
     * \name Scanning
     * \{
     */
    bool startScan(uint32_t durationMs) override;
    void stopScan() override;
    bool isScanComplete() const override { return !scanning_; }
    uint8_t getScanResults(BleScanResult* results, uint8_t maxResults) override;
    /** \} */

    /**
     * \name GATT Server
     * \{
     */
    bool registerGattService(const GattServiceDef& service,
                             bool pluginReserved = false) override;
    bool unregisterGattService(const BleUuid& serviceUuid) override;
    bool sendNotification(uint16_t connHandle, uint16_t attrHandle,
                          const uint8_t* data, uint16_t len) override;
    uint16_t getMtu() const override;
    uint16_t getConnectionHandle() const override { return primaryConnHandle(); }
    void clearAllBonds() override;
    /** \} */

    /**
     * \name GATT Client (Central Role)
     * \{
     */
    bool connect(const uint8_t* addr, uint8_t addrType) override;
    void cancelConnect() override;
    bool discoverServiceByUuid(uint16_t connHandle, const BleUuid& uuid) override;
    bool writeCharacteristic(uint16_t connHandle, uint16_t attrHandle,
                             const uint8_t* data, uint16_t len,
                             bool withResponse) override;
    bool readCharacteristic(uint16_t connHandle, uint16_t attrHandle) override;
    bool enableNotifications(uint16_t connHandle, uint16_t cccdHandle) override;
    void disconnectHandle(uint16_t connHandle) override;
    ListenerToken addServiceDiscoveryCallback(ServiceDiscoveryCallback cb) override;
    ListenerToken addCharacteristicReadCallback(CharacteristicReadCallback cb) override;
    ListenerToken addNotificationCallback(NotificationCallback cb) override;
    ListenerToken addWriteCompleteCallback(WriteCompleteCallback cb) override;
    void removeServiceDiscoveryCallback(ListenerToken token) override;
    void removeCharacteristicReadCallback(ListenerToken token) override;
    void removeNotificationCallback(ListenerToken token) override;
    void removeWriteCompleteCallback(ListenerToken token) override;
    void setServiceDiscoveryCallback(ServiceDiscoveryCallback cb) override;
    void setCharacteristicReadCallback(CharacteristicReadCallback cb) override;
    void setNotificationCallback(NotificationCallback cb) override;
    void setWriteCompleteCallback(WriteCompleteCallback cb) override;
    /** \} */

    /**
     * \name Connection callbacks
     * \{
     */
    ListenerToken addConnectionCallback(ConnectionCallback cb) override;
    ListenerToken addDisconnectionCallback(DisconnectionCallback cb) override;
    void removeConnectionCallback(ListenerToken token) override;
    void removeDisconnectionCallback(ListenerToken token) override;
    /** \} */

    /**
     * \name Pairing
     * \{
     */
    ListenerToken addNumericComparisonCallback(NumericComparisonCallback cb) override;
    void removeNumericComparisonCallback(ListenerToken token) override;
    void setNumericComparisonCallback(NumericComparisonCallback cb) override;
    void respondToNumericComparison(uint16_t connHandle, bool accept) override;
    void setPasskeyCallback(PasskeyCallback cb) override;
    void setAuthCompleteCallback(AuthCompleteCallback cb) override;
    /** \} */

    /**
     * \name Connection event handlers (called from NimBLE callbacks)
     * \{
     */
    void onConnect(uint16_t connHandle, bool isPeripheral);
    void onDisconnect(uint16_t connHandle, int reason);
    void onSync();
    void onScanResult(const ble_gap_disc_desc* disc);
    void onScanComplete();
    void onAdvComplete();
    void onPasskeyAction(uint16_t connHandle, const ble_gap_passkey_params* params);
    void onEncChange(uint16_t connHandle, int status);
    void onSubscribe(const struct ble_gap_event* event);
    void onMtuExchange(uint16_t connHandle, uint16_t mtu);
    /** \} */

    /**
     * \brief Generic listener slot (public so template helpers can access).
     */
    template <typename CB>
    struct ListenerSlot {
        bool active = false;
        CB callback;
    };

    /**
     * \brief Tracks per-connection metadata.
     */
    struct ConnectionState {
        bool active = false;
        uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
        bool isPeripheral = false;   // true = we are peripheral, false = central
        uint16_t mtu = 23;            // ATT MTU including 3-byte header
    };

    /**
     * \brief Tracks per-connection CCCD subscription state for notifications.
     */
    struct SubscribeEntry {
        bool active = false;
        uint16_t connHandle;
        uint16_t attrHandle;
        bool notify;
        bool indicate;
    };

    uint16_t primaryConnHandle() const;
    int8_t findConnectionSlot(uint16_t handle) const;

private:
    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;
    bool enabled_ = false;
    bool synced_ = false;
    bool advertising_ = false;
    bool scanning_ = false;
    bool scanWasAdvertising_ = false;
    char deviceName_[32] = "CDC Badge";
    uint8_t ownAddrType_ = BLE_OWN_ADDR_PUBLIC;

    /** \brief Active connection slots. */
    ConnectionState connections_[MAX_CONNECTIONS] = {};

    /** \brief Per-connection CCCD subscription table. */
    SubscribeEntry subscribes_[MAX_SUBSCRIBE_ENTRIES] = {};

    /** \brief Scan result cache. */
    BleScanResult scanResults_[MAX_SCAN_RESULTS] = {};
    // Per-result flag: name came from a Complete Local Name (0x09). Prevents
    // downgrading it to a Shortened name (0x08) on later advertising events.
    bool scanNameComplete_[MAX_SCAN_RESULTS] = {};
    uint8_t scanResultCount_ = 0;

    /** \brief Advertised service UUID list for scan responses. */
    BleUuid advUuids_[MAX_ADV_UUIDS] = {};
    uint8_t advUuidCount_ = 0;

    ListenerSlot<ConnectionCallback> connCallbacks_[MAX_CONN_CALLBACKS] = {};
    ListenerSlot<DisconnectionCallback> disconnCallbacks_[MAX_CONN_CALLBACKS] = {};
    ListenerSlot<NumericComparisonCallback> numCmpCallbacks_[MAX_CONN_CALLBACKS] = {};
    ListenerSlot<ServiceDiscoveryCallback> svcDiscoveryCallbacks_[MAX_CONN_CALLBACKS] = {};
    ListenerSlot<CharacteristicReadCallback> charReadCallbacks_[MAX_CONN_CALLBACKS] = {};
    ListenerSlot<NotificationCallback> notifyCallbacks_[MAX_CONN_CALLBACKS] = {};
    ListenerSlot<WriteCompleteCallback> writeCompleteCallbacks_[MAX_CONN_CALLBACKS] = {};

    /** \brief Pairing callback handlers. */
    PasskeyCallback passkeyCb_;
    AuthCompleteCallback authCompleteCb_;

    /** \brief Last discovered service state. */
    DiscoveredService discoveredSvc_ = {};
    BleUuid discoverTargetUuid_ = {};
    uint16_t discoverSvcStart_ = 0;
    uint16_t discoverSvcEnd_ = 0;

    // Manufacturer data for advertising
    uint8_t mfgData_[31] = {};
    uint16_t mfgDataLen_ = 0;
    uint16_t mfgCompanyId_ = 0;
    bool mfgDataSet_ = false;

    // Singleton access for callbacks
public:
    static BluetoothController* instance_;
    friend void bleHostTask(void* param);
    friend int bleGapEventCallback(struct ble_gap_event* event, void* arg);
    friend int gattcSvcDiscCb(uint16_t, const struct ble_gatt_error*,
                               const struct ble_gatt_svc*, void*);
    friend int gattcChrDiscCb(uint16_t, const struct ble_gatt_error*,
                               const struct ble_gatt_chr*, void*);
    friend int gattcReadCb(uint16_t, const struct ble_gatt_error*,
                            struct ble_gatt_attr*, void*);
    friend int gattcWriteCb(uint16_t, const struct ble_gatt_error*,
                              struct ble_gatt_attr*, void*);
};

/**
 * \brief Static controller instance pointer used by C-style NimBLE callbacks.
 */
BluetoothController* BluetoothController::instance_ = nullptr;

/**
 * \brief Global NimBLE GAP event callback dispatcher.
 * \param event Pointer to the GAP event payload from NimBLE.
 * \param arg Optional user argument (unused).
 * \return `0` to indicate the event was handled.
 */
int bleGapEventCallback(struct ble_gap_event* event, void* arg) {
    (void)arg;
    auto* ctrl = BluetoothController::instance_;
    if (!ctrl) return 0;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                struct ble_gap_conn_desc desc;
                bool isPeripheral = true;
                if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                    isPeripheral = (desc.role == BLE_GAP_ROLE_SLAVE);
                }
                ctrl->onConnect(event->connect.conn_handle, isPeripheral);
            } else {
                LOG_W(TAG, "Connection failed, status=%d", event->connect.status);
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ctrl->onDisconnect(event->disconnect.conn.conn_handle,
                              event->disconnect.reason);
            break;

        case BLE_GAP_EVENT_CONN_UPDATE:
            LOG_I(TAG, "Connection updated");
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            LOG_D(TAG, "Advertising complete");
            ctrl->onAdvComplete();
            break;

        case BLE_GAP_EVENT_MTU:
            LOG_I(TAG, "MTU updated: handle=%d value=%d",
                  event->mtu.conn_handle, event->mtu.value);
            ctrl->onMtuExchange(event->mtu.conn_handle, event->mtu.value);
            break;

        case BLE_GAP_EVENT_DISC:
            ctrl->onScanResult(&event->disc);
            break;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            LOG_D(TAG, "Scan complete");
            ctrl->onScanComplete();
            break;

        case BLE_GAP_EVENT_PASSKEY_ACTION:
            ctrl->onPasskeyAction(event->passkey.conn_handle,
                                  &event->passkey.params);
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            ctrl->onEncChange(event->enc_change.conn_handle,
                              event->enc_change.status);
            break;

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // Peer is re-pairing: delete the old bond so the new one can replace it.
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        case BLE_GAP_EVENT_SUBSCRIBE:
            ctrl->onSubscribe(event);
            break;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if (event->notify_rx.om) {
                uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
                if (len > sizeof(s_gattAccessBuf)) len = sizeof(s_gattAccessBuf);
                ble_hs_mbuf_to_flat(event->notify_rx.om, s_gattAccessBuf, len, nullptr);
                for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) {
                    if (ctrl->notifyCallbacks_[i].active &&
                        ctrl->notifyCallbacks_[i].callback) {
                        ctrl->notifyCallbacks_[i].callback(
                            event->notify_rx.conn_handle,
                            event->notify_rx.attr_handle,
                            s_gattAccessBuf, len);
                    }
                }
            }
            break;

        default:
            break;
    }

    return 0;
}

/**
 * \brief Called by NimBLE when the host and controller are synchronized.
 * \return void
 */
static void bleSyncCallback() {
    if (BluetoothController::instance_) {
        BluetoothController::instance_->onSync();
    }
}

/**
 * \brief Called when the NimBLE host resets.
 * \param reason Reset reason code provided by NimBLE.
 * \return void
 */
static void bleResetCallback(int reason) {
    LOG_E(TAG, "BLE host reset, reason=%d", reason);
}

/**
 * \brief FreeRTOS task entry that runs the NimBLE host loop.
 * \param param Task parameter (unused).
 * \return void
 */
static void bleHostTask(void* param) {
    (void)param;
    LOG_I(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/**
 * \brief Initializes controller state and releases unused classic BT memory.
 * \return `true` when initialization completed or was already done.
 */
bool BluetoothController::init() {
    if (state_ != core::ServiceState::UNINITIALIZED) {
        return state_ == core::ServiceState::INITIALIZED ||
               state_ == core::ServiceState::STARTED;
    }

    // instance_ is set in the constructor, but reaffirm here in case multiple
    // instances are ever created (only the most recently initialized wins).
    instance_ = this;

    // Release classic BT memory (we only use BLE)
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    state_ = core::ServiceState::INITIALIZED;
    LOG_I(TAG, "Bluetooth controller initialized");
    return true;
}

/**
 * \brief Transitions the service to started state.
 * \return `true` when the controller is started or already running.
 */
bool BluetoothController::start() {
    if (state_ == core::ServiceState::INITIALIZED ||
        state_ == core::ServiceState::STOPPED) {
        state_ = core::ServiceState::STARTED;
        return true;
    }
    return state_ == core::ServiceState::STARTED;
}

/**
 * \brief Stops the service and disables BLE if currently active.
 */
void BluetoothController::stop() {
    if (state_ == core::ServiceState::STARTED) {
        if (enabled_) {
            disable();
        }
        state_ = core::ServiceState::STOPPED;
    }
}

/**
 * \brief Enables NimBLE host stack and starts host task execution.
 * \return `true` if BLE was enabled successfully.
 */
bool BluetoothController::enable() {
    if (enabled_) {
        return true;
    }

    if (state_ != core::ServiceState::STARTED) {
        LOG_E(TAG, "Cannot enable - service not started");
        return false;
    }

    // Initialize NimBLE
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        LOG_E(TAG, "nimble_port_init failed: %d", ret);
        return false;
    }

    // Configure NimBLE host
    ble_hs_cfg.reset_cb = bleResetCallback;
    ble_hs_cfg.sync_cb = bleSyncCallback;
    ble_hs_cfg.gatts_register_cb = nullptr;
    ble_hs_cfg.store_status_cb = nullptr;

    // Security: Display+YesNo for numeric comparison pairing
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_YES_NO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    // Initialize mandatory GAP and GATT services
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Commit module GATT services registered before BLE was enabled (and restore
    // those from a previous session, since disable() tears down the GATT DB).
    for (int i = 0; i < MAX_REGISTERED_SERVICES; i++) {
        if (!s_services[i].active) continue;
        int grc = ble_gatts_count_cfg(s_services[i].nimbleSvcs);
        if (grc == 0) grc = ble_gatts_add_svcs(s_services[i].nimbleSvcs);
        if (grc != 0) {
            LOG_E(TAG, "Deferred GATT service slot %d commit failed: %d", i, grc);
        } else {
            LOG_I(TAG, "Committed GATT service slot %d", i);
        }
    }

    // Wire up the NVS-backed bond store
    ble_store_config_init();

    // Set device name
    ble_svc_gap_device_name_set(deviceName_);

    // Start NimBLE host task
    nimble_port_freertos_init(bleHostTask);

    enabled_ = true;
    LOG_I(TAG, "Bluetooth enabled");
    return true;
}

/**
 * \brief Disables BLE stack and clears runtime connection state.
 */
void BluetoothController::disable() {
    if (!enabled_) {
        return;
    }

    // Disconnect any active connection
    for (uint8_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections_[i].active) {
            ble_gap_terminate(connections_[i].handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    // Stop advertising if active
    ble_gap_adv_stop();

    // Shutdown NimBLE and wait for the host task to actually exit before deinit
    int rc = nimble_port_stop();
    if (rc == 0) {
        // nimble_port_stop signals the host task; give it time to drain
        vTaskDelay(pdMS_TO_TICKS(50));
        nimble_port_deinit();
    }

    enabled_ = false;
    synced_ = false;
    for (uint8_t i = 0; i < MAX_CONNECTIONS; i++) connections_[i].active = false;
    for (uint8_t i = 0; i < MAX_SUBSCRIBE_ENTRIES; i++) subscribes_[i].active = false;

    LOG_I(TAG, "Bluetooth disabled");
}

/**
 * \brief Retrieves the local Bluetooth MAC address.
 * \param mac Output buffer for 6-byte MAC address.
 * \return `true` if the address was written.
 */
bool BluetoothController::getMacAddress(uint8_t* mac) const {
    if (!mac) return false;

    if (enabled_ && synced_) {
        // Get address from NimBLE
        int rc = ble_hs_id_copy_addr(ownAddrType_, mac, nullptr);
        return rc == 0;
    }

    // Fallback: read from efuse
    esp_read_mac(mac, ESP_MAC_BT);
    return true;
}

/**
 * \brief Updates the GAP device name used for advertising.
 * \param name New device name string.
 */
void BluetoothController::setDeviceName(const char* name) {
    if (!name) return;

    strncpy(deviceName_, name, sizeof(deviceName_) - 1);
    deviceName_[sizeof(deviceName_) - 1] = '\0';

    if (enabled_ && synced_) {
        ble_svc_gap_device_name_set(deviceName_);
    }
}

/**
 * \brief Terminates the current connection if one exists.
 */
void BluetoothController::disconnect() {
    for (uint8_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections_[i].active) {
            ble_gap_terminate(connections_[i].handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}

bool BluetoothController::isConnected() const {
    for (uint8_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections_[i].active) return true;
    }
    return false;
}

uint16_t BluetoothController::primaryConnHandle() const {
    for (uint8_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections_[i].active) return connections_[i].handle;
    }
    return BLE_HS_CONN_HANDLE_NONE;
}

int8_t BluetoothController::findConnectionSlot(uint16_t handle) const {
    for (int8_t i = 0; i < (int8_t)MAX_CONNECTIONS; i++) {
        if (connections_[i].active && connections_[i].handle == handle) return i;
    }
    return -1;
}

void BluetoothController::clearAllBonds() {
    int rc = ble_store_clear();
    if (rc != 0) {
        LOG_E(TAG, "ble_store_clear failed: %d", rc);
    } else {
        LOG_I(TAG, "All bonds cleared");
    }
}

void BluetoothController::onEncChange(uint16_t connHandle, int status) {
    LOG_I(TAG, "Encryption %s on handle %d (status=%d)",
          status == 0 ? "established" : "failed", connHandle, status);
    if (authCompleteCb_) {
        authCompleteCb_(status == 0);
    }
}

void BluetoothController::onMtuExchange(uint16_t connHandle, uint16_t mtu) {
    int8_t slot = findConnectionSlot(connHandle);
    if (slot >= 0) {
        connections_[slot].mtu = mtu;
    }
}

void BluetoothController::onSubscribe(const struct ble_gap_event* event) {
    if (!event) return;
    LOG_I(TAG, "Subscribe: handle=%d attr=%d notify=%d indicate=%d",
          event->subscribe.conn_handle, event->subscribe.attr_handle,
          event->subscribe.cur_notify, event->subscribe.cur_indicate);

    // Find or allocate a subscribe entry
    int slot = -1;
    for (int i = 0; i < MAX_SUBSCRIBE_ENTRIES; i++) {
        if (subscribes_[i].active &&
            subscribes_[i].connHandle == event->subscribe.conn_handle &&
            subscribes_[i].attrHandle == event->subscribe.attr_handle) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_SUBSCRIBE_ENTRIES; i++) {
            if (!subscribes_[i].active) { slot = i; break; }
        }
    }
    if (slot < 0) {
        LOG_W(TAG, "Subscribe table full");
        return;
    }
    subscribes_[slot].active = true;
    subscribes_[slot].connHandle = event->subscribe.conn_handle;
    subscribes_[slot].attrHandle = event->subscribe.attr_handle;
    subscribes_[slot].notify = event->subscribe.cur_notify != 0;
    subscribes_[slot].indicate = event->subscribe.cur_indicate != 0;
    if (!subscribes_[slot].notify && !subscribes_[slot].indicate) {
        subscribes_[slot].active = false;
    }
}

/**
 * \brief Reads RSSI for the active connection.
 * \return Current RSSI in dBm, or `0` if unavailable.
 */
int8_t BluetoothController::getRssi() const {
    uint16_t handle = primaryConnHandle();
    if (handle == BLE_HS_CONN_HANDLE_NONE) {
        return 0;
    }

    int8_t rssi = 0;
    int rc = ble_gap_conn_rssi(handle, &rssi);
    return (rc == 0) ? rssi : 0;
}

/**
 * \brief Handles successful connection establishment.
 * \param connHandle Established connection handle.
 */
void BluetoothController::onConnect(uint16_t connHandle, bool isPeripheral) {
    advertising_ = false;
    LOG_I(TAG, "Device connected (handle=%d, role=%s)",
          connHandle, isPeripheral ? "peripheral" : "central");

    // Track in connection table
    int slot = -1;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connections_[i].active) { slot = i; break; }
    }
    if (slot >= 0) {
        connections_[slot].active = true;
        connections_[slot].handle = connHandle;
        connections_[slot].isPeripheral = isPeripheral;
        connections_[slot].mtu = 23;
    } else {
        LOG_W(TAG, "Connection table full, dropping handle %d", connHandle);
    }

    // Dispatch to registered listeners
    for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) {
        if (connCallbacks_[i].active && connCallbacks_[i].callback) {
            connCallbacks_[i].callback(connHandle);
        }
    }
}

/**
 * \brief Handles disconnect events and restarts advertising.
 * \param connHandle Disconnected handle.
 * \param reason NimBLE disconnect reason code.
 */
void BluetoothController::onDisconnect(uint16_t connHandle, int reason) {
    LOG_I(TAG, "Device disconnected (handle=%d reason=%d)", connHandle, reason);

    bool wasPeripheral = true;
    int8_t slot = findConnectionSlot(connHandle);
    if (slot >= 0) {
        wasPeripheral = connections_[slot].isPeripheral;
        connections_[slot].active = false;
    }

    // Drop subscriptions associated with this connection
    for (uint8_t i = 0; i < MAX_SUBSCRIBE_ENTRIES; i++) {
        if (subscribes_[i].active && subscribes_[i].connHandle == connHandle) {
            subscribes_[i].active = false;
        }
    }

    // Dispatch to registered listeners
    for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) {
        if (disconnCallbacks_[i].active && disconnCallbacks_[i].callback) {
            disconnCallbacks_[i].callback(connHandle, reason);
        }
    }

    // Role-aware advertising restart: only if we were the peripheral.
    advertising_ = false;
    if (wasPeripheral) {
        startAdvertising();
    }
}

/**
 * \brief Finalizes BLE sync state and starts advertising.
 */
void BluetoothController::onSync() {
    synced_ = true;

    // Determine best address type
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        LOG_E(TAG, "Failed to ensure address: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &ownAddrType_);
    if (rc != 0) {
        LOG_E(TAG, "Failed to infer address type: %d", rc);
        ownAddrType_ = BLE_OWN_ADDR_PUBLIC;
    }

    uint8_t addr[6];
    ble_hs_id_copy_addr(ownAddrType_, addr, nullptr);
    LOG_I(TAG, "BLE synced, addr=%02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    // Auto-start advertising after sync
    startAdvertising();
}

/**
 * \brief Advertising lifecycle and payload configuration.
 */

/**
 * \brief Starts BLE advertising with configured UUIDs and manufacturer payload.
 */
void BluetoothController::startAdvertising() {
    if (!enabled_ || !synced_) return;

    // Stop current advertising if running
    if (advertising_) {
        ble_gap_adv_stop();
        advertising_ = false;
    }

    struct ble_gap_adv_params advParams = {};
    advParams.conn_mode = BLE_GAP_CONN_MODE_UND;
    advParams.disc_mode = BLE_GAP_DISC_MODE_GEN;
    advParams.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    advParams.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;

    // Primary advertising data: flags + name
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t*)deviceName_;
    fields.name_len = strlen(deviceName_);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        LOG_E(TAG, "Failed to set adv fields: %d", rc);
        return;
    }

    // Scan response: registered service UUIDs + manufacturer data
    if (advUuidCount_ > 0 || mfgDataSet_) {
        struct ble_hs_adv_fields rsp = {};

        // Separate 16-bit and 128-bit UUIDs
        ble_uuid16_t uuid16s[MAX_ADV_UUIDS];
        ble_uuid128_t uuid128s[MAX_ADV_UUIDS];
        uint8_t num16 = 0, num128 = 0;

        for (uint8_t i = 0; i < advUuidCount_; i++) {
            if (advUuids_[i].type == BleUuid::UUID_16 && num16 < MAX_ADV_UUIDS) {
                uuid16s[num16].u.type = BLE_UUID_TYPE_16;
                uuid16s[num16].value = advUuids_[i].u16;
                num16++;
            } else if (advUuids_[i].type == BleUuid::UUID_128 && num128 < MAX_ADV_UUIDS) {
                uuid128s[num128].u.type = BLE_UUID_TYPE_128;
                memcpy(uuid128s[num128].value, advUuids_[i].u128, 16);
                num128++;
            }
        }

        if (num16 > 0) {
            rsp.uuids16 = uuid16s;
            rsp.num_uuids16 = num16;
            rsp.uuids16_is_complete = 1;
        }
        if (num128 > 0) {
            rsp.uuids128 = uuid128s;
            rsp.num_uuids128 = num128;
            rsp.uuids128_is_complete = 1;
        }

        // Manufacturer-specific data (company ID + payload)
        uint8_t mfgAdvBuf[33];
        if (mfgDataSet_) {
            mfgAdvBuf[0] = mfgCompanyId_ & 0xFF;
            mfgAdvBuf[1] = (mfgCompanyId_ >> 8) & 0xFF;
            memcpy(mfgAdvBuf + 2, mfgData_, mfgDataLen_);
            rsp.mfg_data = mfgAdvBuf;
            rsp.mfg_data_len = mfgDataLen_ + 2;
        }

        rc = ble_gap_adv_rsp_set_fields(&rsp);
        if (rc != 0) {
            LOG_W(TAG, "Failed to set scan response: %d (continuing without)", rc);
        }
    }

    rc = ble_gap_adv_start(ownAddrType_, nullptr, BLE_HS_FOREVER,
                           &advParams, bleGapEventCallback, nullptr);
    if (rc != 0) {
        LOG_E(TAG, "Failed to start advertising: %d", rc);
        return;
    }

    advertising_ = true;
    LOG_I(TAG, "Advertising started (%d service UUIDs)", advUuidCount_);
}

/**
 * \brief Stops active BLE advertising.
 */
void BluetoothController::stopAdvertising() {
    if (!advertising_) return;

    ble_gap_adv_stop();
    advertising_ = false;
    LOG_I(TAG, "Advertising stopped");
}

/**
 * \brief Updates internal state when advertising naturally completes.
 */
void BluetoothController::onAdvComplete() {
    advertising_ = false;
}

/**
 * \brief Scanning lifecycle and discovery result caching.
 */

/**
 * \brief Starts an active BLE scan for the requested duration.
 * \param durationMs Scan duration in milliseconds.
 * \return `true` if scan start succeeded.
 */
bool BluetoothController::startScan(uint32_t durationMs) {
    if (!enabled_ || !synced_ || scanning_) return false;

    // Legacy advertising and scanning can't coexist - stop advertising first
    bool wasAdvertising = advertising_;
    if (advertising_) {
        ble_gap_adv_stop();
        advertising_ = false;
        LOG_D(TAG, "Stopped advertising for scan");
    }

    // Clear previous results
    scanResultCount_ = 0;
    memset(scanResults_, 0, sizeof(scanResults_));
    memset(scanNameComplete_, 0, sizeof(scanNameComplete_));

    struct ble_gap_disc_params discParams = {};
    discParams.filter_duplicates = 0;  // Allow duplicates to get scan responses with names
    discParams.passive = 0;  // Active scan to get names
    discParams.itvl = 0;     // Use defaults
    discParams.window = 0;
    discParams.filter_policy = 0;
    discParams.limited = 0;

    int rc = ble_gap_disc(ownAddrType_, durationMs, &discParams,
                          bleGapEventCallback, nullptr);
    if (rc != 0) {
        LOG_E(TAG, "Failed to start scan: %d", rc);
        // Restore advertising if it was active
        if (wasAdvertising) {
            startAdvertising();
        }
        return false;
    }

    scanWasAdvertising_ = wasAdvertising;
    scanning_ = true;
    LOG_I(TAG, "Scan started (%lu ms)", (unsigned long)durationMs);
    return true;
}

/**
 * \brief Cancels an ongoing BLE scan.
 */
void BluetoothController::stopScan() {
    if (!scanning_) return;

    ble_gap_disc_cancel();
    scanning_ = false;
    LOG_I(TAG, "Scan stopped");
}

/**
 * \brief Extract device name from raw BLE advertising data.
 *
 * Searches for AD types 0x09 (complete local name) or 0x08 (shortened local name)
 * without using the heavy ble_hs_adv_parse_fields struct (saves ~200 bytes stack).
 *
 * \param data Raw advertising data bytes.
 * \param dataLen Length of advertising data.
 * \param name Output buffer for the extracted name.
 * \param nameMaxLen Size of output buffer.
 * \return true if a name was found and copied.
 */
static bool parseAdvName(const uint8_t* data, uint8_t dataLen,
                         char* name, size_t nameMaxLen, bool* isComplete = nullptr) {
    uint8_t pos = 0;
    while (pos < dataLen) {
        uint8_t len = data[pos];
        if (len == 0 || pos + len > dataLen) break;
        uint8_t type = data[pos + 1];
        // 0x09 = Complete Local Name, 0x08 = Shortened Local Name
        if (type == 0x09 || type == 0x08) {
            uint8_t nameLen = len - 1;
            size_t copyLen = (nameLen < nameMaxLen - 1) ? nameLen : nameMaxLen - 1;
            memcpy(name, &data[pos + 2], copyLen);
            name[copyLen] = '\0';
            if (isComplete) *isComplete = (type == 0x09);
            return true;
        }
        pos += len + 1;
    }
    return false;
}

/**
 * \brief Fills a scan-result slot from a discovery descriptor.
 *
 * Stores MAC/address-type/RSSI and the raw advertising data, then sets the
 * name from the advertising payload, falling back to the formatted MAC when
 * no name is advertised. \p outNameComplete reports whether the stored name
 * came from a Complete Local Name (0x09) so callers can avoid downgrading it.
 */
static void fillScanResult(BleScanResult& result, const ble_gap_disc_desc* disc,
                           bool* outNameComplete) {
    memcpy(result.mac, disc->addr.val, 6);
    result.addrType = disc->addr.type;
    result.rssi = disc->rssi;
    result.name[0] = '\0';

    result.advDataLen = (disc->length_data <= sizeof(result.advData))
                         ? disc->length_data : sizeof(result.advData);
    memcpy(result.advData, disc->data, result.advDataLen);

    bool isComplete = false;
    parseAdvName(disc->data, disc->length_data, result.name, sizeof(result.name), &isComplete);
    const bool haveName = (result.name[0] != '\0');
    if (outNameComplete) *outNameComplete = haveName && isComplete;
    if (!haveName) {
        snprintf(result.name, sizeof(result.name), "%02X:%02X:%02X:%02X:%02X:%02X",
                 result.mac[5], result.mac[4], result.mac[3],
                 result.mac[2], result.mac[1], result.mac[0]);
    }
}

void BluetoothController::onScanResult(const ble_gap_disc_desc* disc) {
    if (!disc) return;

    // Update an existing entry (same MAC). Scan responses often carry the name.
    for (uint8_t i = 0; i < scanResultCount_; i++) {
        if (memcmp(scanResults_[i].mac, disc->addr.val, 6) == 0) {
            if (disc->rssi > scanResults_[i].rssi) {
                scanResults_[i].rssi = disc->rssi;
            }
            // Replace the name only with a non-empty one, and never downgrade a
            // Complete Local Name (0x09) to a Shortened one (0x08): devices that
            // advertise a short name in the primary PDU and the full name in the
            // scan response would otherwise flip-flop every interval.
            char parsedName[32];
            bool isComplete = false;
            if (parseAdvName(disc->data, disc->length_data, parsedName, sizeof(parsedName), &isComplete) &&
                parsedName[0] != '\0' &&
                (isComplete || !scanNameComplete_[i]) &&
                strcmp(parsedName, scanResults_[i].name) != 0) {
                strncpy(scanResults_[i].name, parsedName, sizeof(scanResults_[i].name) - 1);
                scanResults_[i].name[sizeof(scanResults_[i].name) - 1] = '\0';
                scanNameComplete_[i] = isComplete;
                LOG_D(TAG, "Name updated: %s (evt=0x%02X)", parsedName, disc->event_type);
            }
            return;
        }
    }

    // New device. Append while there is room; once full, keep the strongest by
    // replacing the weakest entry only when the newcomer has a higher RSSI.
    uint8_t slot;
    if (scanResultCount_ < MAX_SCAN_RESULTS) {
        slot = scanResultCount_++;
    } else {
        uint8_t weakest = 0;
        for (uint8_t i = 1; i < scanResultCount_; i++) {
            if (scanResults_[i].rssi < scanResults_[weakest].rssi) weakest = i;
        }
        if (disc->rssi <= scanResults_[weakest].rssi) return;
        slot = weakest;
    }

    bool nameComplete = false;
    fillScanResult(scanResults_[slot], disc, &nameComplete);
    scanNameComplete_[slot] = nameComplete;
    LOG_D(TAG, "Found: %s (RSSI %d) evt=0x%02X dlen=%d",
          scanResults_[slot].name, scanResults_[slot].rssi,
          disc->event_type, disc->length_data);
}

/**
 * \brief Finalizes scanning state and restores prior advertising mode.
 */
void BluetoothController::onScanComplete() {
    scanning_ = false;
    LOG_I(TAG, "Scan complete, found %d devices", scanResultCount_);

    // Restore advertising if it was active before scan
    if (scanWasAdvertising_) {
        scanWasAdvertising_ = false;
        startAdvertising();
    }
}

/**
 * \brief Copies cached scan results to caller-provided storage.
 * \param results Output array for scan results.
 * \param maxResults Maximum number of entries to copy.
 * \return Number of copied scan results.
 */
uint8_t BluetoothController::getScanResults(BleScanResult* results, uint8_t maxResults) {
    if (!results || maxResults == 0) return 0;

    uint8_t count = (scanResultCount_ < maxResults) ? scanResultCount_ : maxResults;
    memcpy(results, scanResults_, count * sizeof(BleScanResult));
    return count;
}

/**
 * \brief Dynamic GATT server registration and notification helpers.
 */

/**
 * \brief Registers a module-provided GATT service with NimBLE.
 * \param service High-level service definition.
 * \return `true` if registration succeeded.
 */
bool BluetoothController::registerGattService(const GattServiceDef& service,
                                              bool pluginReserved) {
    // Reuse the slot already holding this service UUID (idempotent re-register),
    // otherwise take a free slot. Registration is allowed while BLE is disabled:
    // the slot is stored and committed to NimBLE later from enable().
    // System modules draw from slots [0, PLUGIN_SERVICE_SLOT); a plugin draws
    // only from the reserved PLUGIN_SERVICE_SLOT, so neither can starve the other.
    ble_uuid_any_t wantUuid;
    convertUuid(service.uuid, wantUuid);
    const int firstSlot = pluginReserved ? PLUGIN_SERVICE_SLOT : 0;
    const int lastSlot  = pluginReserved ? MAX_REGISTERED_SERVICES : PLUGIN_SERVICE_SLOT;
    int slot = -1;
    for (int i = firstSlot; i < lastSlot; i++) {
        if (s_services[i].active && ble_uuid_cmp(&s_services[i].svcUuid.u, &wantUuid.u) == 0) {
            slot = i; break;
        }
    }
    if (slot < 0) {
        for (int i = firstSlot; i < lastSlot; i++) {
            if (!s_services[i].active) { slot = i; break; }
        }
    }
    if (slot < 0) {
        LOG_E(TAG, "No free GATT service slots (max %d)", MAX_REGISTERED_SERVICES);
        return false;
    }

    auto& s = s_services[slot];
    memset(&s, 0, sizeof(InternalService));

    // Convert service UUID
    convertUuid(service.uuid, s.svcUuid);

    // Convert characteristics
    uint8_t numChars = std::min(service.numCharacteristics, (uint8_t)MAX_CHARS_PER_SERVICE);
    s.numChars = numChars;

    for (uint8_t i = 0; i < numChars; i++) {
        const auto& src = service.characteristics[i];
        auto& dst = s.nimbleChars[i];

        convertUuid(src.uuid, s.charUuids[i]);

        dst.uuid = &s.charUuids[i].u;
        dst.access_cb = gattServiceAccessCb;
        dst.arg = &s;
        dst.descriptors = nullptr;
        dst.flags = mapProperties(src.properties, src.permissions);
        dst.min_key_size = 0;
        dst.val_handle = src.valueHandle;

        // Build per-characteristic descriptor list (e.g. HID Report Reference).
        uint8_t numDsc = src.numDescriptors;
        if (numDsc > MAX_DESCRIPTORS_PER_CHAR) numDsc = MAX_DESCRIPTORS_PER_CHAR;
        for (uint8_t d = 0; d < numDsc; d++) {
            const GattDescriptor& gd = src.descriptors[d];
            uint16_t uuid16 = 0;
            switch (gd.kind) {
                case GattDescriptorKind::REPORT_REFERENCE: uuid16 = 0x2908; break;
                default: uuid16 = 0; break;
            }
            if (uuid16 == 0) continue;

            s.dscUuids[i][d].u.type = BLE_UUID_TYPE_16;
            s.dscUuids[i][d].u16.u.type = BLE_UUID_TYPE_16;
            s.dscUuids[i][d].u16.value = uuid16;

            // Pack as [len][bytes]
            uint8_t copyLen = (gd.dataLen <= 4) ? gd.dataLen : 4;
            s.dscPacked[i][d][0] = copyLen;
            memcpy(&s.dscPacked[i][d][1], gd.data, copyLen);

            s.dscDefs[i][d].uuid = &s.dscUuids[i][d].u;
            s.dscDefs[i][d].att_flags = BLE_ATT_F_READ;
            s.dscDefs[i][d].min_key_size = 0;
            s.dscDefs[i][d].access_cb = gattStaticDescriptorAccessCb;
            s.dscDefs[i][d].arg = s.dscPacked[i][d];
        }
        if (numDsc > 0) {
            // Terminator descriptor
            memset(&s.dscDefs[i][numDsc], 0, sizeof(ble_gatt_dsc_def));
            dst.descriptors = s.dscDefs[i];
        }

        s.writeCallbacks[i] = src.onWrite;
        s.readCallbacks[i] = src.onRead;
    }

    // Terminator
    memset(&s.nimbleChars[numChars], 0, sizeof(ble_gatt_chr_def));

    // Build NimBLE service definition
    s.nimbleSvcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s.nimbleSvcs[0].uuid = &s.svcUuid.u;
    s.nimbleSvcs[0].includes = nullptr;
    s.nimbleSvcs[0].characteristics = s.nimbleChars;
    memset(&s.nimbleSvcs[1], 0, sizeof(ble_gatt_svc_def));

    // Commit the slot; enable() (re)adds every active service through the one
    // correct init path.
    s.active = true;

    // BLE not enabled yet: enable() commits this slot later.
    if (!enabled_) {
        LOG_I(TAG, "GATT service stored, deferred until BLE enable (slot %d)", slot);
        return true;
    }

    // BLE already running: NimBLE cannot add a service to a started GATT server
    // in place (ble_svc_gap_init is not re-entrant and asserts on a second
    // call), so rebuild the whole stack through the proven disable()/enable()
    // path. This drops any active BLE connection.
    disable();
    if (!enable()) {
        s.active = false;
        LOG_E(TAG, "GATT service register: BLE restart failed (slot %d)", slot);
        return false;
    }
    LOG_I(TAG, "GATT service registered via BLE restart (slot %d, %d chars)", slot, numChars);
    return true;
}

bool BluetoothController::unregisterGattService(const BleUuid& serviceUuid) {
    ble_uuid_any_t wantUuid;
    convertUuid(serviceUuid, wantUuid);
    int slot = -1;
    for (int i = 0; i < MAX_REGISTERED_SERVICES; i++) {
        if (s_services[i].active && ble_uuid_cmp(&s_services[i].svcUuid.u, &wantUuid.u) == 0) {
            slot = i; break;
        }
    }
    if (slot < 0) return false;

    memset(&s_services[slot], 0, sizeof(InternalService));

    // While disabled there is no live GATT DB to rebuild; the slot is simply
    // dropped before the next enable() commits the remaining services.
    if (!enabled_) {
        LOG_I(TAG, "GATT service unregistered (slot %d, deferred)", slot);
        return true;
    }

    // Rebuild the live GATT DB through the proven disable()/enable() path (an
    // in-place ble_gatts_reset + ble_svc_gap_init re-init asserts in NimBLE).
    // This drops any active BLE connection.
    disable();
    if (!enable()) {
        LOG_E(TAG, "GATT service unregister: BLE restart failed (slot %d)", slot);
        return false;
    }
    LOG_I(TAG, "GATT service unregistered via BLE restart (slot %d)", slot);
    return true;
}

/**
 * \brief Sends a GATT notification to a connected peer.
 * \param connHandle Connection handle.
 * \param attrHandle Characteristic value handle.
 * \param data Notification payload.
 * \param len Payload length in bytes.
 * \return `true` if notification was queued successfully.
 */
bool BluetoothController::sendNotification(uint16_t connHandle, uint16_t attrHandle,
                                            const uint8_t* data, uint16_t len) {
    if (!enabled_ || !data || len == 0) {
        return false;
    }

    auto notifyOne = [&](uint16_t handle) -> bool {
        // Check the per-connection CCCD table - skip peers that are not subscribed.
        bool subscribed = false;
        for (uint8_t i = 0; i < MAX_SUBSCRIBE_ENTRIES; i++) {
            if (subscribes_[i].active &&
                subscribes_[i].connHandle == handle &&
                subscribes_[i].attrHandle == attrHandle &&
                subscribes_[i].notify) {
                subscribed = true;
                break;
            }
        }
        if (!subscribed) return false;

        struct os_mbuf* om = ble_hs_mbuf_from_flat(data, len);
        if (!om) {
            LOG_E(TAG, "Failed to allocate mbuf for notification");
            return false;
        }
        int rc = ble_gatts_notify_custom(handle, attrHandle, om);
        if (rc != 0) {
            // NimBLE may or may not free the mbuf chain on failure; free
            // unconditionally to avoid a leak when rc != 0.
            os_mbuf_free_chain(om);
            return false;
        }
        return true;
    };

    if (connHandle == 0xFFFF || connHandle == BLE_HS_CONN_HANDLE_NONE) {
        bool any = false;
        for (uint8_t i = 0; i < MAX_CONNECTIONS; i++) {
            if (connections_[i].active) {
                if (notifyOne(connections_[i].handle)) any = true;
            }
        }
        return any;
    }
    return notifyOne(connHandle);
}

/**
 * \brief Returns the ATT payload MTU for the active connection.
 * \return Effective application payload size in bytes.
 */
uint16_t BluetoothController::getMtu() const {
    uint16_t handle = primaryConnHandle();
    if (handle == BLE_HS_CONN_HANDLE_NONE) {
        return 20; // Default minimum BLE payload (ATT MTU 23 - 3 header bytes)
    }
    uint16_t mtu = ble_att_mtu(handle);
    return (mtu > 3) ? (mtu - 3) : 20;
}

/**
 * \brief Management of advertised service UUIDs.
 */

/**
 * \brief Adds a service UUID to advertising scan response data.
 * \param uuid UUID to advertise.
 * \return `true` if UUID is present after the call.
 */
bool BluetoothController::addAdvertisingUuid(const BleUuid& uuid) {
    // Check if already registered
    for (uint8_t i = 0; i < advUuidCount_; i++) {
        if (advUuids_[i] == uuid) return true;
    }

    if (advUuidCount_ >= MAX_ADV_UUIDS) {
        LOG_E(TAG, "Max advertising UUIDs reached (%d)", MAX_ADV_UUIDS);
        return false;
    }

    advUuids_[advUuidCount_++] = uuid;
    LOG_I(TAG, "Advertising UUID registered (%d total)", advUuidCount_);

    // Start or restart advertising to include new UUID
    if (enabled_ && synced_) {
        if (advertising_) {
            stopAdvertising();
        }
        startAdvertising();
    }

    return true;
}

/**
 * \brief Removes a service UUID from advertising data.
 * \param uuid UUID to remove.
 */
void BluetoothController::removeAdvertisingUuid(const BleUuid& uuid) {
    for (uint8_t i = 0; i < advUuidCount_; i++) {
        if (advUuids_[i] == uuid) {
            // Shift remaining UUIDs
            for (uint8_t j = i; j < advUuidCount_ - 1; j++) {
                advUuids_[j] = advUuids_[j + 1];
            }
            advUuidCount_--;
            LOG_I(TAG, "Advertising UUID removed (%d remaining)", advUuidCount_);

            // Restart advertising without removed UUID
            if (advertising_) {
                stopAdvertising();
                startAdvertising();
            }
            return;
        }
    }
}

/**
 * \brief Registration of connection/disconnection event callbacks.
 */

/**
 * \brief Allocates a free listener slot and stores the callback.
 * \return Slot index encoded as a `ListenerToken`, or `INVALID_LISTENER` if full.
 */
template <typename CB, size_t N>
static IBluetoothController::ListenerToken addListener(
    BluetoothController::ListenerSlot<CB> (&slots)[N], CB cb) {
    if (!cb) return IBluetoothController::INVALID_LISTENER;
    for (size_t i = 0; i < N; i++) {
        if (!slots[i].active) {
            slots[i].active = true;
            slots[i].callback = cb;
            return static_cast<IBluetoothController::ListenerToken>(i);
        }
    }
    return IBluetoothController::INVALID_LISTENER;
}

/**
 * \brief Releases a listener slot by token.
 */
template <typename CB, size_t N>
static void removeListener(
    BluetoothController::ListenerSlot<CB> (&slots)[N],
    IBluetoothController::ListenerToken token) {
    if (token >= N) return;
    slots[token].active = false;
    slots[token].callback = nullptr;
}

IBluetoothController::ListenerToken
BluetoothController::addConnectionCallback(ConnectionCallback cb) {
    return addListener(connCallbacks_, cb);
}

IBluetoothController::ListenerToken
BluetoothController::addDisconnectionCallback(DisconnectionCallback cb) {
    return addListener(disconnCallbacks_, cb);
}

void BluetoothController::removeConnectionCallback(ListenerToken token) {
    removeListener(connCallbacks_, token);
}

void BluetoothController::removeDisconnectionCallback(ListenerToken token) {
    removeListener(disconnCallbacks_, token);
}

/**
 * \brief Pairing callback setup and interactive confirmation handling.
 */

/**
 * \brief Sets callback for displayed passkeys.
 * \param cb Passkey callback.
 */
void BluetoothController::setPasskeyCallback(PasskeyCallback cb) {
    passkeyCb_ = cb;
}

/**
 * \brief Sets callback for authentication completion.
 * \param cb Authentication completion callback.
 */
void BluetoothController::setAuthCompleteCallback(AuthCompleteCallback cb) {
    authCompleteCb_ = cb;
}

IBluetoothController::ListenerToken
BluetoothController::addNumericComparisonCallback(NumericComparisonCallback cb) {
    return addListener(numCmpCallbacks_, cb);
}

void BluetoothController::removeNumericComparisonCallback(ListenerToken token) {
    removeListener(numCmpCallbacks_, token);
}

void BluetoothController::setNumericComparisonCallback(NumericComparisonCallback cb) {
    for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) numCmpCallbacks_[i].active = false;
    if (cb) addListener(numCmpCallbacks_, cb);
}

/**
 * \brief Sends numeric-comparison acceptance or rejection to NimBLE.
 * \param connHandle Connection handle.
 * \param accept `true` to accept, `false` to reject.
 */
void BluetoothController::respondToNumericComparison(uint16_t connHandle, bool accept) {
    struct ble_sm_io pkey = {};
    pkey.action = BLE_SM_IOACT_NUMCMP;
    pkey.numcmp_accept = accept ? 1 : 0;
    int rc = ble_sm_inject_io(connHandle, &pkey);
    if (rc != 0) {
        LOG_E(TAG, "ble_sm_inject_io failed: %d", rc);
    }
    LOG_I(TAG, "Pairing %s", accept ? "accepted" : "rejected");
}

/**
 * \brief Handles passkey and numeric-comparison pairing events.
 * \param connHandle Connection handle.
 * \param params Pairing action parameters.
 */
void BluetoothController::onPasskeyAction(uint16_t connHandle,
                                            const ble_gap_passkey_params* params) {
    if (!params) return;

    switch (params->action) {
        case BLE_SM_IOACT_NUMCMP: {
            LOG_I(TAG, "Numeric comparison: %06lu", (unsigned long)params->numcmp);
            LOG_I(TAG, "%s stack free: %lu words", pcTaskGetName(nullptr),
                  (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
            bool dispatched = false;
            for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) {
                if (numCmpCallbacks_[i].active && numCmpCallbacks_[i].callback) {
                    numCmpCallbacks_[i].callback(connHandle, params->numcmp);
                    dispatched = true;
                }
            }
            if (!dispatched) {
                // No callback registered: reject by default for safety.
                LOG_W(TAG, "No numeric comparison callback registered, rejecting");
                respondToNumericComparison(connHandle, false);
            }
            break;
        }

        case BLE_SM_IOACT_DISP:
            LOG_I(TAG, "Display passkey: %06lu", (unsigned long)params->numcmp);
            if (passkeyCb_) {
                passkeyCb_(params->numcmp);
            }
            break;

        default:
            LOG_W(TAG, "Unhandled passkey action: %d", params->action);
            break;
    }
}

/**
 * \brief Manufacturer-specific advertising data management.
 */

/**
 * \brief Configures manufacturer-specific advertising payload.
 * \param companyId Bluetooth SIG company identifier.
 * \param data Manufacturer payload bytes.
 * \param len Payload length.
 * \return `true` if payload was stored successfully.
 */
bool BluetoothController::setAdvertisingManufacturerData(uint16_t companyId,
                                                          const uint8_t* data, uint16_t len) {
    if (!data || len > sizeof(mfgData_)) return false;

    memcpy(mfgData_, data, len);
    mfgDataLen_ = len;
    mfgCompanyId_ = companyId;
    mfgDataSet_ = true;

    if (advertising_) {
        stopAdvertising();
        startAdvertising();
    }
    return true;
}

/**
 * \brief Clears manufacturer-specific advertising payload.
 */
void BluetoothController::clearAdvertisingManufacturerData() {
    mfgDataSet_ = false;
    mfgDataLen_ = 0;

    if (advertising_) {
        stopAdvertising();
        startAdvertising();
    }
}

/**
 * \brief Central-role callback handlers used by NimBLE GATT client operations.
 */

/**
 * \brief Characteristic discovery callback for central-role service discovery.
 * \param connHandle Connection handle.
 * \param error NimBLE discovery status.
 * \param chr Characteristic descriptor for current callback item.
 * \param arg Optional user argument (unused).
 * \return `0` to continue callback processing.
 */
int gattcChrDiscCb(uint16_t connHandle, const struct ble_gatt_error* error,
                    const struct ble_gatt_chr* chr, void* arg) {
    (void)arg;
    auto* ctrl = BluetoothController::instance_;
    if (!ctrl) return 0;

    if (error->status == 0 && chr) {
        auto& svc = ctrl->discoveredSvc_;
        if (svc.numCharacteristics < IBluetoothController::DiscoveredService::MAX_DISCOVERED_CHARS) {
            auto& dc = svc.characteristics[svc.numCharacteristics];
            // Use NimBLE's typed helper to safely extract 16-bit UUIDs from
            // either BLE_UUID_TYPE_16 or BLE_UUID_TYPE_32 variants. Anything
            // larger falls back to 128-bit.
            if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
                dc.uuid = BleUuid::from16(ble_uuid_u16(&chr->uuid.u));
            } else if (chr->uuid.u.type == BLE_UUID_TYPE_32) {
                // No native 32-bit support in our generic UUID; convert to 128-bit per BT spec.
                uint8_t u128[16] = { 0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00,
                                      0x00, 0x80, 0x00, 0x10, 0x00, 0x00,
                                      0, 0, 0, 0 };
                uint32_t v = chr->uuid.u32.value;
                u128[12] = v & 0xFF;
                u128[13] = (v >> 8) & 0xFF;
                u128[14] = (v >> 16) & 0xFF;
                u128[15] = (v >> 24) & 0xFF;
                dc.uuid = BleUuid::from128(u128);
            } else {
                dc.uuid = BleUuid::from128(chr->uuid.u128.value);
            }
            dc.valueHandle = chr->val_handle;
            dc.properties = chr->properties;
            svc.numCharacteristics++;
        }
    } else if (error->status == BLE_HS_EDONE) {
        LOG_I(TAG, "Char discovery done (%d chars)", ctrl->discoveredSvc_.numCharacteristics);
        for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) {
            if (ctrl->svcDiscoveryCallbacks_[i].active &&
                ctrl->svcDiscoveryCallbacks_[i].callback) {
                ctrl->svcDiscoveryCallbacks_[i].callback(
                    connHandle, &ctrl->discoveredSvc_, true);
            }
        }
    } else {
        LOG_E(TAG, "Char discovery error: %d", error->status);
        for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) {
            if (ctrl->svcDiscoveryCallbacks_[i].active &&
                ctrl->svcDiscoveryCallbacks_[i].callback) {
                ctrl->svcDiscoveryCallbacks_[i].callback(connHandle, nullptr, true);
            }
        }
    }

    return 0;
}

/**
 * \brief Service discovery callback that chains into characteristic discovery.
 * \param connHandle Connection handle.
 * \param error NimBLE discovery status.
 * \param service Service descriptor for current callback item.
 * \param arg Optional user argument (unused).
 * \return `0` to continue callback processing.
 */
int gattcSvcDiscCb(uint16_t connHandle, const struct ble_gatt_error* error,
                    const struct ble_gatt_svc* service, void* arg) {
    (void)arg;
    auto* ctrl = BluetoothController::instance_;
    if (!ctrl) return 0;

    auto dispatchFailure = [&]() {
        for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) {
            if (ctrl->svcDiscoveryCallbacks_[i].active &&
                ctrl->svcDiscoveryCallbacks_[i].callback) {
                ctrl->svcDiscoveryCallbacks_[i].callback(connHandle, nullptr, true);
            }
        }
    };

    if (error->status == 0 && service) {
        ctrl->discoverSvcStart_ = service->start_handle;
        ctrl->discoverSvcEnd_ = service->end_handle;
        LOG_I(TAG, "Service found: handles %d-%d",
              service->start_handle, service->end_handle);
    } else if (error->status == BLE_HS_EDONE) {
        if (ctrl->discoverSvcStart_ != 0) {
            int rc = ble_gattc_disc_all_chrs(connHandle,
                                              ctrl->discoverSvcStart_,
                                              ctrl->discoverSvcEnd_,
                                              gattcChrDiscCb, nullptr);
            if (rc != 0) {
                LOG_E(TAG, "ble_gattc_disc_all_chrs failed: %d", rc);
                dispatchFailure();
            }
        } else {
            LOG_W(TAG, "Service not found on remote device");
            dispatchFailure();
        }
    } else {
        LOG_E(TAG, "Service discovery error: %d", error->status);
        dispatchFailure();
    }

    return 0;
}

/**
 * \brief Read completion callback for GATT client operations.
 * \param connHandle Connection handle.
 * \param error Read status information.
 * \param attr Read attribute payload.
 * \param arg Optional user argument (unused).
 * \return `0` to finish callback handling.
 */
int gattcReadCb(uint16_t connHandle, const struct ble_gatt_error* error,
                 struct ble_gatt_attr* attr, void* arg) {
    (void)arg;
    auto* ctrl = BluetoothController::instance_;
    if (!ctrl) return 0;

    if (error->status == 0 && attr && attr->om) {
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len > sizeof(s_gattAccessBuf)) len = sizeof(s_gattAccessBuf);
        ble_hs_mbuf_to_flat(attr->om, s_gattAccessBuf, len, nullptr);
        for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) {
            if (ctrl->charReadCallbacks_[i].active &&
                ctrl->charReadCallbacks_[i].callback) {
                ctrl->charReadCallbacks_[i].callback(
                    connHandle, attr->handle, s_gattAccessBuf, len);
            }
        }
    } else {
        LOG_E(TAG, "GATT read failed: %d", error->status);
        for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) {
            if (ctrl->charReadCallbacks_[i].active &&
                ctrl->charReadCallbacks_[i].callback) {
                ctrl->charReadCallbacks_[i].callback(connHandle, 0, nullptr, 0);
            }
        }
    }

    return 0;
}

/**
 * \brief Write completion callback for GATT client operations.
 * \param connHandle Connection handle.
 * \param error Write status information.
 * \param attr Written attribute metadata.
 * \param arg Optional user argument (unused).
 * \return `0` to finish callback handling.
 */
int gattcWriteCb(uint16_t connHandle, const struct ble_gatt_error* error,
                   struct ble_gatt_attr* attr, void* arg) {
    (void)arg;
    auto* ctrl = BluetoothController::instance_;
    if (!ctrl) return 0;

    uint16_t handle = attr ? attr->handle : 0;
    for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) {
        if (ctrl->writeCompleteCallbacks_[i].active &&
            ctrl->writeCompleteCallbacks_[i].callback) {
            ctrl->writeCompleteCallbacks_[i].callback(connHandle, handle, error->status);
        }
    }

    return 0;
}

/**
 * \brief Central-role public API method implementations.
 */

/**
 * \brief Initiates a central-role connection to a remote BLE address.
 * \param addr Remote MAC address.
 * \param addrType Remote BLE address type.
 * \return `true` if connection initiation started successfully.
 */
bool BluetoothController::connect(const uint8_t* addr, uint8_t addrType) {
    if (!enabled_ || !synced_ || !addr) return false;

    // Must stop advertising to connect as central
    if (advertising_) {
        ble_gap_adv_stop();
        advertising_ = false;
    }

    ble_addr_t bleAddr;
    bleAddr.type = addrType;
    memcpy(bleAddr.val, addr, 6);

    int rc = ble_gap_connect(ownAddrType_, &bleAddr, 10000, nullptr,
                             bleGapEventCallback, nullptr);
    if (rc != 0) {
        LOG_E(TAG, "ble_gap_connect failed: %d", rc);
        startAdvertising();
        return false;
    }

    LOG_I(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X",
          addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return true;
}

/**
 * \brief Cancels an in-progress GAP connection attempt.
 */
void BluetoothController::cancelConnect() {
    ble_gap_conn_cancel();
}

/**
 * \brief Discovers a remote service by UUID on an active connection.
 * \param connHandle Connection handle.
 * \param uuid Target service UUID.
 * \return `true` if discovery request was accepted.
 */
bool BluetoothController::discoverServiceByUuid(uint16_t connHandle, const BleUuid& uuid) {
    if (!enabled_) return false;

    // Reset discovery state
    memset(&discoveredSvc_, 0, sizeof(discoveredSvc_));
    discoveredSvc_.uuid = uuid;
    discoverTargetUuid_ = uuid;
    discoverSvcStart_ = 0;
    discoverSvcEnd_ = 0;

    ble_uuid_any_t nimbleUuid;
    convertUuid(uuid, nimbleUuid);

    int rc = ble_gattc_disc_svc_by_uuid(connHandle, &nimbleUuid.u,
                                          gattcSvcDiscCb, nullptr);
    if (rc != 0) {
        LOG_E(TAG, "ble_gattc_disc_svc_by_uuid failed: %d", rc);
        return false;
    }

    LOG_I(TAG, "Service discovery started (connHandle=%d)", connHandle);
    return true;
}

/**
 * \brief Writes a remote characteristic value.
 * \param connHandle Connection handle.
 * \param attrHandle Characteristic value handle.
 * \param data Payload bytes to write.
 * \param len Payload length.
 * \param withResponse `true` for write-with-response, `false` otherwise.
 * \return `true` if write request was submitted successfully.
 */
bool BluetoothController::writeCharacteristic(uint16_t connHandle, uint16_t attrHandle,
                                               const uint8_t* data, uint16_t len,
                                               bool withResponse) {
    if (!enabled_ || !data || len == 0) return false;

    int rc;
    if (withResponse) {
        rc = ble_gattc_write_flat(connHandle, attrHandle, data, len,
                                   gattcWriteCb, nullptr);
    } else {
        // Write-without-response has no confirmation event. Do not invoke
        // writeCompleteCallback synchronously - modules treating that as a
        // TX confirmation would misreport delivery.
        rc = ble_gattc_write_no_rsp_flat(connHandle, attrHandle, data, len);
    }

    if (rc != 0) {
        LOG_E(TAG, "GATT write failed: %d", rc);
        return false;
    }

    return true;
}

/**
 * \brief Starts an asynchronous read of a remote characteristic.
 * \param connHandle Connection handle.
 * \param attrHandle Characteristic handle to read.
 * \return `true` if read request was submitted.
 */
bool BluetoothController::readCharacteristic(uint16_t connHandle, uint16_t attrHandle) {
    if (!enabled_) return false;

    int rc = ble_gattc_read(connHandle, attrHandle, gattcReadCb, nullptr);
    if (rc != 0) {
        LOG_E(TAG, "ble_gattc_read failed: %d", rc);
        return false;
    }

    return true;
}

/**
 * \brief Enables notifications by writing the CCCD.
 * \param connHandle Connection handle.
 * \param cccdHandle CCCD attribute handle.
 * \return `true` if CCCD write request was submitted.
 */
bool BluetoothController::enableNotifications(uint16_t connHandle, uint16_t cccdHandle) {
    if (!enabled_) return false;

    uint8_t val[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(connHandle, cccdHandle, val, sizeof(val),
                                   gattcWriteCb, nullptr);
    if (rc != 0) {
        LOG_E(TAG, "Enable notifications failed: %d", rc);
        return false;
    }

    LOG_I(TAG, "Notifications enabled (cccd=%d)", cccdHandle);
    return true;
}

/**
 * \brief Terminates a specific BLE connection handle.
 * \param connHandle Connection handle to terminate.
 */
void BluetoothController::disconnectHandle(uint16_t connHandle) {
    ble_gap_terminate(connHandle, BLE_ERR_REM_USER_CONN_TERM);
}

IBluetoothController::ListenerToken
BluetoothController::addServiceDiscoveryCallback(ServiceDiscoveryCallback cb) {
    return addListener(svcDiscoveryCallbacks_, cb);
}
IBluetoothController::ListenerToken
BluetoothController::addCharacteristicReadCallback(CharacteristicReadCallback cb) {
    return addListener(charReadCallbacks_, cb);
}
IBluetoothController::ListenerToken
BluetoothController::addNotificationCallback(NotificationCallback cb) {
    return addListener(notifyCallbacks_, cb);
}
IBluetoothController::ListenerToken
BluetoothController::addWriteCompleteCallback(WriteCompleteCallback cb) {
    return addListener(writeCompleteCallbacks_, cb);
}

void BluetoothController::removeServiceDiscoveryCallback(ListenerToken t) {
    removeListener(svcDiscoveryCallbacks_, t);
}
void BluetoothController::removeCharacteristicReadCallback(ListenerToken t) {
    removeListener(charReadCallbacks_, t);
}
void BluetoothController::removeNotificationCallback(ListenerToken t) {
    removeListener(notifyCallbacks_, t);
}
void BluetoothController::removeWriteCompleteCallback(ListenerToken t) {
    removeListener(writeCompleteCallbacks_, t);
}

void BluetoothController::setServiceDiscoveryCallback(ServiceDiscoveryCallback cb) {
    for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) svcDiscoveryCallbacks_[i].active = false;
    if (cb) addListener(svcDiscoveryCallbacks_, cb);
}
void BluetoothController::setCharacteristicReadCallback(CharacteristicReadCallback cb) {
    for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) charReadCallbacks_[i].active = false;
    if (cb) addListener(charReadCallbacks_, cb);
}
void BluetoothController::setNotificationCallback(NotificationCallback cb) {
    for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) notifyCallbacks_[i].active = false;
    if (cb) addListener(notifyCallbacks_, cb);
}
void BluetoothController::setWriteCompleteCallback(WriteCompleteCallback cb) {
    for (uint8_t i = 0; i < MAX_CONN_CALLBACKS; i++) writeCompleteCallbacks_[i].active = false;
    if (cb) addListener(writeCompleteCallbacks_, cb);
}

/**
 * \brief Singleton Bluetooth controller instance.
 */
static BluetoothController g_bluetoothController;

/**
 * \brief Returns the singleton Bluetooth controller instance.
 * \return Pointer to the global `IBluetoothController` implementation.
 */
IBluetoothController* getBluetoothControllerInstance() {
    return &g_bluetoothController;
}

} // namespace cdc::hal

#else // NimBLE not enabled - stub implementation

#include "esp_mac.h"
#include <cstring>

namespace cdc::hal {

/**
 * Stub Bluetooth Controller when NimBLE is not enabled
 */
class BluetoothControllerStub : public IBluetoothController {
public:
    /**
     * \brief Initializes stub controller state.
     * \return Always `true`.
     */
    bool init() override {
        LOG_W(TAG, "Bluetooth disabled (NimBLE not configured)");
        state_ = core::ServiceState::INITIALIZED;
        return true;
    }
    /**
     * \brief Starts stub controller state.
     * \return Always `true`.
     */
    bool start() override {
        state_ = core::ServiceState::STARTED;
        return true;
    }
    void stop() override { state_ = core::ServiceState::STOPPED; }
    core::ServiceState getState() const override { return state_; }
    const char* getName() const override { return "bluetooth"; }

    bool enable() override {
        LOG_W(TAG, "Cannot enable - NimBLE not configured in sdkconfig");
        return false;
    }
    void disable() override {}
    bool isEnabled() const override { return false; }
    /**
     * \brief Returns BLE MAC address using efuse fallback.
     * \param mac Output MAC buffer.
     * \return `true` if output buffer was valid.
     */
    bool getMacAddress(uint8_t* mac) const override {
        if (mac) esp_read_mac(mac, ESP_MAC_BT);
        return mac != nullptr;
    }
    /**
     * \brief Stores requested device name in local stub buffer.
     * \param name Device name string.
     */
    void setDeviceName(const char* name) override {
        if (name) {
            strncpy(deviceName_, name, sizeof(deviceName_) - 1);
        }
    }
    const char* getDeviceName() const override { return deviceName_; }
    bool isConnected() const override { return false; }
    void disconnect() override {}
    int8_t getRssi() const override { return 0; }

private:
    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;
    char deviceName_[32] = "CDC Badge";
};

static BluetoothControllerStub g_bluetoothController;

/**
 * \brief Returns singleton Bluetooth stub when NimBLE is unavailable.
 * \return Pointer to stub controller.
 */
IBluetoothController* getBluetoothControllerInstance() {
    return &g_bluetoothController;
}

} // namespace cdc::hal

#endif // CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
