#pragma once

#include "cdc_core/IService.h"
#include <cstdint>
#include <cstring>
#include <functional>

namespace cdc::hal {

/**
 * BLE scan result with raw advertising data for module-specific parsing
 */
struct BleScanResult {
    char name[32];
    uint8_t mac[6];
    uint8_t addrType;          // 0=public, 1=random
    int8_t rssi;
    uint8_t advData[31];       // Raw advertising data (AD structures)
    uint8_t advDataLen;
};

/**
 * Bonded (paired) peer identity, stack-independent.
 */
struct BleBondInfo {
    uint8_t addr[6];           // Identity address
    uint8_t addrType;          // 0=public, 1=random
    bool connected;            // Currently in an active connection
};

/**
 * Stack-independent BLE UUID (no NimBLE/Bluedroid dependency)
 */
struct BleUuid {
    enum Type : uint8_t { UUID_16 = 0, UUID_128 = 1 };
    Type type;
    union {
        uint16_t u16;
        uint8_t u128[16];
    };

    static BleUuid from16(uint16_t v) {
        BleUuid u;
        u.type = UUID_16;
        // u16 and u128 alias in the union: zero first, then set the value
        std::memset(u.u128, 0, sizeof(u.u128));
        u.u16 = v;
        return u;
    }

    static BleUuid from128(const uint8_t v[16]) {
        BleUuid u;
        u.type = UUID_128;
        std::memcpy(u.u128, v, 16);
        return u;
    }

    bool operator==(const BleUuid& other) const {
        if (type != other.type) return false;
        if (type == UUID_16) return u16 == other.u16;
        return std::memcmp(u128, other.u128, 16) == 0;
    }
};

/**
 * Stack-independent GATT characteristic property flags
 * Values match BLE spec (Vol 3, Part G, 3.3.1.1)
 */
namespace GattProp {
    constexpr uint8_t READ         = 0x02;
    constexpr uint8_t WRITE_NO_RSP = 0x04;
    constexpr uint8_t WRITE        = 0x08;
    constexpr uint8_t NOTIFY       = 0x10;
    constexpr uint8_t INDICATE     = 0x20;
}

/**
 * Stack-independent GATT attribute permission flags
 */
namespace GattPerm {
    constexpr uint8_t READ         = 0x01;
    constexpr uint8_t WRITE        = 0x02;
    constexpr uint8_t READ_ENC     = 0x04;  // Requires encryption (pairing)
    constexpr uint8_t WRITE_ENC    = 0x08;  // Requires encryption (pairing)
}

/**
 * GATT write callback for characteristic writes
 */
using GattWriteCallback = std::function<int(uint16_t connHandle, uint16_t attrHandle,
                                             const uint8_t* data, uint16_t len)>;

/**
 * GATT read callback for characteristic reads
 */
using GattReadCallback = std::function<int(uint16_t connHandle, uint16_t attrHandle,
                                            uint8_t* buf, uint16_t* len)>;

/**
 * GATT descriptor type identifiers used by GattDescriptor.kind
 */
enum class GattDescriptorKind : uint8_t {
    NONE = 0,
    REPORT_REFERENCE = 1,   // 0x2908 - HID Report Reference (2 bytes: reportId, reportType)
};

/**
 * GATT descriptor definition attached to a characteristic.
 * Only well-known descriptors are modeled; arbitrary descriptors are not supported.
 */
struct GattDescriptor {
    GattDescriptorKind kind;
    uint8_t data[4];           // Descriptor value bytes
    uint8_t dataLen;           // Number of valid bytes in data
};

/**
 * GATT characteristic definition for service registration
 */
struct GattCharacteristic {
    BleUuid uuid;
    uint8_t properties;        // GattProp flags
    uint8_t permissions;       // GattPerm flags
    uint16_t* valueHandle;     // Output: handle assigned by stack
    GattWriteCallback onWrite;
    GattReadCallback onRead;
    const GattDescriptor* descriptors = nullptr;  // Optional descriptor list
    uint8_t numDescriptors = 0;
};

/**
 * GATT service definition for registration via registerGattService()
 */
struct GattServiceDef {
    BleUuid uuid;
    GattCharacteristic* characteristics;
    uint8_t numCharacteristics;
};

/**
 * Bluetooth Controller Interface
 * Handles BLE stack initialization, power control, scanning and advertising
 */
class IBluetoothController : public core::IService {
public:
    virtual ~IBluetoothController() = default;

    /**
     * Opaque listener token returned by add*Callback() methods.
     * Pass to the matching remove*Callback() to unregister.
     */
    using ListenerToken = uint16_t;
    static constexpr ListenerToken INVALID_LISTENER = 0xFFFF;

    // GATT server limits (single source of truth for the controller and any
    // caller validating a service before registerGattService()).
    static constexpr uint8_t MAX_REGISTERED_SERVICES = 7;
    static constexpr uint8_t MAX_CHARS_PER_SERVICE   = 6;

    // === Power Control ===

    /**
     * Enable Bluetooth (initialize BLE stack)
     * @return true if successfully enabled
     */
    virtual bool enable() = 0;

    /**
     * Disable Bluetooth (shutdown BLE stack to save power)
     */
    virtual void disable() = 0;

    /**
     * \brief Signals that system startup is complete.
     *
     * Before this is called, enable() only records the request and defers the
     * actual stack bring-up. This lets every GATT service registered during boot
     * commit together in a single NimBLE start, so advertising begins once with
     * the complete service-UUID set instead of racing per-module restarts.
     */
    virtual void notifySystemReady() {}

    /**
     * Check if Bluetooth is currently enabled
     */
    virtual bool isEnabled() const = 0;

    // === Device Info ===

    /**
     * Get current Bluetooth MAC address
     * @param mac Output buffer (6 bytes)
     * @return true if address retrieved successfully
     */
    virtual bool getMacAddress(uint8_t* mac) const = 0;

    /**
     * Set device name for BLE advertising
     * @param name Device name (null-terminated)
     */
    virtual void setDeviceName(const char* name) = 0;

    /**
     * Get device name
     */
    virtual const char* getDeviceName() const = 0;

    // === Connection ===

    /**
     * Check if a device is currently connected
     */
    virtual bool isConnected() const = 0;

    /**
     * Disconnect current connection if any
     */
    virtual void disconnect() = 0;

    /**
     * Get RSSI of connected device
     * @return RSSI in dBm, or 0 if not connected
     */
    virtual int8_t getRssi() const = 0;

    /**
     * Get connected device name
     * @param buf Output buffer
     * @param bufLen Buffer size
     * @return true if connected and name retrieved
     */
    virtual bool getConnectedDeviceName(char* buf, size_t bufLen) const { (void)buf; (void)bufLen; return false; }

    /**
     * Maximum number of bonded peers retained/returned. Single source of truth
     * for both the controller's bond store and any caller-side buffers.
     */
    static constexpr uint8_t MAX_BONDED_DEVICES = 5;

    /**
     * Get number of bonded (paired) devices
     */
    virtual uint8_t getBondedDeviceCount() const { return 0; }

    /**
     * Enumerate bonded (paired) peers into a caller-provided buffer.
     * @param out Output array of BleBondInfo
     * @param maxCount Capacity of the output array
     * @return Number of bonds written
     */
    virtual uint8_t getBondedDevices(BleBondInfo* out, uint8_t maxCount) const {
        (void)out; (void)maxCount;
        return 0;
    }

    // === Advertising ===

    /**
     * Start BLE advertising
     */
    virtual void startAdvertising() {}

    /**
     * Stop BLE advertising
     */
    virtual void stopAdvertising() {}

    /**
     * Check if currently advertising
     */
    virtual bool isAdvertising() const { return false; }

    /**
     * Register a service UUID to include in advertising scan response.
     * Enables remote devices to discover services before connecting.
     * @param uuid Service UUID to advertise
     * @return true if registered (max 4 UUIDs)
     */
    virtual bool addAdvertisingUuid(const BleUuid& uuid) { (void)uuid; return false; }

    /**
     * Remove a service UUID from advertising scan response.
     * @param uuid Service UUID to remove
     */
    virtual void removeAdvertisingUuid(const BleUuid& uuid) { (void)uuid; }

    /**
     * Set manufacturer-specific data in advertising scan response.
     * Used for module-specific broadcast data (e.g., vCard minicard).
     * @param companyId Bluetooth company ID (0xFFFF for testing)
     * @param data Payload data
     * @param len Payload length (max ~27 bytes)
     * @return true if set successfully
     */
    virtual bool setAdvertisingManufacturerData(uint16_t companyId,
                                                 const uint8_t* data, uint16_t len) {
        (void)companyId; (void)data; (void)len;
        return false;
    }

    /**
     * Clear manufacturer data from advertising
     */
    virtual void clearAdvertisingManufacturerData() {}

    /**
     * Set the GAP Appearance value advertised in the primary PDU.
     * Lets HOGP hosts categorize the device (e.g. 0x03C1 = HID Keyboard).
     * @param appearance Appearance value, or 0 to advertise none
     */
    virtual void setAppearance(uint16_t appearance) { (void)appearance; }

    // === Scanning ===

    /**
     * Maximum number of scan results retained/returned. Single source of truth
     * for both the controller's result buffer and any caller-side buffers.
     */
    static constexpr uint8_t MAX_SCAN_RESULTS = 64;

    /**
     * Start BLE scan
     * @param durationMs Scan duration in milliseconds; 0 scans continuously until stopScan().
     * @param keepAdvertising If true, advertising keeps running during the scan
     *        (Peripheral + Observer multi-role) instead of being stopped.
     * @return true if scan started
     */
    virtual bool startScan(uint32_t durationMs = 5000, bool keepAdvertising = false) {
        (void)durationMs; (void)keepAdvertising; return false;
    }

    /**
     * Stop ongoing scan
     */
    virtual void stopScan() {}

    /**
     * Check if scan is complete
     */
    virtual bool isScanComplete() const { return true; }

    /**
     * Get scan results
     * @param results Output array
     * @param maxResults Maximum results to return
     * @return Number of results
     */
    virtual uint8_t getScanResults(BleScanResult* results, uint8_t maxResults) { (void)results; (void)maxResults; return 0; }

    // === Pairing Callbacks ===

    using PasskeyCallback = std::function<void(uint32_t passkey)>;
    using AuthCompleteCallback = std::function<void(bool success)>;

    /**
     * Set callback for passkey display during pairing
     */
    virtual void setPasskeyCallback(PasskeyCallback cb) { (void)cb; }

    /**
     * Set callback for authentication completion
     */
    virtual void setAuthCompleteCallback(AuthCompleteCallback cb) { (void)cb; }

    /**
     * Numeric comparison callback for secure pairing.
     * Called when a connecting device requires confirmation.
     * Display the passkey and let the user accept/reject.
     */
    using NumericComparisonCallback = std::function<void(uint16_t connHandle, uint32_t passkey)>;

    /**
     * Register a numeric-comparison pairing callback.
     * Multiple listeners are supported. Only the first listener that calls
     * respondToNumericComparison() will succeed; later responses are no-ops.
     * \return Token usable with removeNumericComparisonCallback(), or INVALID_LISTENER on overflow.
     */
    virtual ListenerToken addNumericComparisonCallback(NumericComparisonCallback cb) {
        (void)cb;
        return INVALID_LISTENER;
    }

    /**
     * Unregister a numeric-comparison callback previously added via add*().
     */
    virtual void removeNumericComparisonCallback(ListenerToken token) { (void)token; }

    /**
     * Legacy single-listener setter retained for source compatibility.
     * Equivalent to calling addNumericComparisonCallback() on a freshly cleared list.
     */
    virtual void setNumericComparisonCallback(NumericComparisonCallback cb) { (void)cb; }

    /**
     * Respond to a numeric comparison pairing request
     * @param connHandle Connection handle from the callback
     * @param accept true to accept, false to reject
     */
    virtual void respondToNumericComparison(uint16_t connHandle, bool accept) {
        (void)connHandle; (void)accept;
    }

    /**
     * Called when link encryption is established or fails on a connection.
     * status == 0 means the link is now encrypted.
     */
    using EncChangeCallback = std::function<void(uint16_t connHandle, int status)>;

    /**
     * Register a callback for encryption-change events (multi-listener).
     * \return Token usable with removeEncryptionChangeCallback(), or INVALID_LISTENER on overflow.
     */
    virtual ListenerToken addEncryptionChangeCallback(EncChangeCallback cb) {
        (void)cb; return INVALID_LISTENER;
    }

    /**
     * Unregister a previously added encryption-change callback.
     */
    virtual void removeEncryptionChangeCallback(ListenerToken token) { (void)token; }

    /**
     * Initiate link encryption / pairing as central on an existing connection.
     * Triggers the configured pairing flow (e.g. numeric comparison). Used to
     * upgrade a connection to an encrypted link on demand.
     * @param connHandle Connection handle
     * @return true if the security procedure was started
     */
    virtual bool initiateSecurity(uint16_t connHandle) { (void)connHandle; return false; }

    /**
     * Resolve the identity address of a connected peer.
     * @param connHandle Connection handle
     * @param addr Output 6-byte address
     * @param addrType Output address type (0=public, 1=random)
     * @return true if the peer address was resolved
     */
    virtual bool getPeerIdAddr(uint16_t connHandle, uint8_t addr[6], uint8_t* addrType) const {
        (void)connHandle; (void)addr; (void)addrType; return false;
    }

    // === Connection Callbacks (multi-listener) ===

    using ConnectionCallback = std::function<void(uint16_t connHandle)>;
    using DisconnectionCallback = std::function<void(uint16_t connHandle, int reason)>;

    /**
     * Register a callback for device connection events.
     * Multiple callbacks are supported (up to 4).
     * \return Token usable with removeConnectionCallback(), or INVALID_LISTENER on overflow.
     */
    virtual ListenerToken addConnectionCallback(ConnectionCallback cb) { (void)cb; return INVALID_LISTENER; }

    /**
     * Register a callback for device disconnection events.
     * Multiple callbacks are supported (up to 4).
     * \return Token usable with removeDisconnectionCallback(), or INVALID_LISTENER on overflow.
     */
    virtual ListenerToken addDisconnectionCallback(DisconnectionCallback cb) { (void)cb; return INVALID_LISTENER; }

    /**
     * Unregister a previously added connection callback.
     */
    virtual void removeConnectionCallback(ListenerToken token) { (void)token; }

    /**
     * Unregister a previously added disconnection callback.
     */
    virtual void removeDisconnectionCallback(ListenerToken token) { (void)token; }

    // === Bond Management ===

    /**
     * Erase all bonded peer information from the bond store.
     */
    virtual void clearAllBonds() {}

    /**
     * Forget (unpair) a single bonded peer by identity address.
     * Used for ephemeral pairings that should not persist.
     * @param addr 6-byte identity address
     * @param addrType Address type (0=public, 1=random)
     */
    virtual void forgetBond(const uint8_t addr[6], uint8_t addrType) {
        (void)addr; (void)addrType;
    }

    // === GATT Server ===

    /**
     * Register a GATT service with characteristics.
     * Service definitions are translated to stack-native format internally.
     * Must be called after enable() and before advertising.
     * @param service Service definition (struct must remain valid until unregistered)
     * @param pluginReserved true to allocate from the slot reserved for plugins;
     *                       false (system modules) to allocate from the system pool
     * @return true if successfully registered
     */
    virtual bool registerGattService(const GattServiceDef& service,
                                     bool pluginReserved = false) {
        (void)service; (void)pluginReserved;
        return false;
    }

    /**
     * Unregister a previously registered GATT service by its service UUID and
     * rebuild the GATT database. Intended for dynamically (un)loaded owners
     * such as plugins.
     * @param serviceUuid UUID of the service to remove
     * @return true if a matching service was found and removed
     */
    virtual bool unregisterGattService(const BleUuid& serviceUuid) {
        (void)serviceUuid;
        return false;
    }

    /**
     * Send notification on a characteristic
     * @param connHandle Connection handle (0xFFFF for all connections)
     * @param attrHandle Attribute handle of the characteristic
     * @param data Data to send
     * @param len Data length
     * @return true if notification sent
     */
    virtual bool sendNotification(uint16_t connHandle, uint16_t attrHandle,
                                  const uint8_t* data, uint16_t len) {
        (void)connHandle; (void)attrHandle; (void)data; (void)len;
        return false;
    }

    /**
     * Send indication on a characteristic (with acknowledgment)
     * @param connHandle Connection handle
     * @param attrHandle Attribute handle
     * @param data Data to send
     * @param len Data length
     * @return true if indication sent
     */
    virtual bool sendIndication(uint16_t connHandle, uint16_t attrHandle,
                                const uint8_t* data, uint16_t len) {
        (void)connHandle; (void)attrHandle; (void)data; (void)len;
        return false;
    }

    /**
     * Get negotiated MTU for current connection (payload size)
     * @return Usable payload size (MTU - 3 for ATT overhead), or 20 if not connected
     */
    virtual uint16_t getMtu() const { return 20; }

    // === Central Role (GATT Client) ===

    /**
     * Connect to a peripheral device
     * @param addr BLE address (6 bytes)
     * @param addrType Address type (0=public, 1=random)
     * @return true if connection initiated
     */
    virtual bool connect(const uint8_t* addr, uint8_t addrType = 0) {
        (void)addr; (void)addrType;
        return false;
    }

    /**
     * Cancel an in-progress connection attempt started via connect().
     * No-op if no connection is pending.
     */
    virtual void cancelConnect() {}

    /**
     * Discover a specific service by UUID on connected device.
     * Results delivered via ServiceDiscoveryCallback with discovered characteristics.
     * @param connHandle Connection handle
     * @param uuid Service UUID to discover
     * @return true if discovery started
     */
    virtual bool discoverServiceByUuid(uint16_t connHandle, const BleUuid& uuid) {
        (void)connHandle; (void)uuid;
        return false;
    }

    /**
     * Write to a characteristic on remote device
     * @param connHandle Connection handle
     * @param attrHandle Attribute handle
     * @param data Data to write
     * @param len Data length
     * @param withResponse true for write with response
     * @return true if write initiated
     */
    virtual bool writeCharacteristic(uint16_t connHandle, uint16_t attrHandle,
                                     const uint8_t* data, uint16_t len,
                                     bool withResponse = true) {
        (void)connHandle; (void)attrHandle; (void)data; (void)len; (void)withResponse;
        return false;
    }

    /**
     * Read a characteristic from remote device
     * @param connHandle Connection handle
     * @param attrHandle Attribute handle
     * @return true if read initiated
     */
    virtual bool readCharacteristic(uint16_t connHandle, uint16_t attrHandle) {
        (void)connHandle; (void)attrHandle;
        return false;
    }

    /**
     * Enable notifications on a remote characteristic
     * @param connHandle Connection handle
     * @param cccdHandle CCCD handle (usually attrHandle + 1)
     * @return true if successful
     */
    virtual bool enableNotifications(uint16_t connHandle, uint16_t cccdHandle) {
        (void)connHandle; (void)cccdHandle;
        return false;
    }

    /**
     * Subscribe to notifications on a remote characteristic by its value
     * handle. Discovers the characteristic's CCCD descriptor (UUID 0x2902)
     * and writes it, falling back to valueHandle + 1 if descriptor discovery
     * yields nothing. Requires a prior discoverServiceByUuid() on the same
     * connection whose range still covers the characteristic (the last
     * discovered service's end handle bounds the descriptor search).
     * @param connHandle Connection handle
     * @param valueHandle Characteristic value handle from discovery
     * @return true if the subscribe sequence was initiated
     */
    virtual bool subscribeToCharacteristic(uint16_t connHandle, uint16_t valueHandle) {
        (void)connHandle; (void)valueHandle;
        return false;
    }

    /**
     * Disconnect a specific connection (central or peripheral)
     * @param connHandle Connection handle to disconnect
     */
    virtual void disconnectHandle(uint16_t connHandle) { (void)connHandle; }

    // === Central Role Callbacks ===

    /**
     * Discovered characteristic from service discovery
     */
    struct DiscoveredCharacteristic {
        BleUuid uuid;
        uint16_t valueHandle;
        uint8_t properties;  // GattProp flags
    };

    /**
     * Discovered service from service discovery
     */
    struct DiscoveredService {
        BleUuid uuid;
        static constexpr uint8_t MAX_DISCOVERED_CHARS = 8;
        DiscoveredCharacteristic characteristics[MAX_DISCOVERED_CHARS];
        uint8_t numCharacteristics;
    };

    /**
     * Called when service discovery completes.
     * @param connHandle Connection handle
     * @param service Discovered service (null if discovery failed)
     * @param complete true when discovery is finished
     */
    using ServiceDiscoveryCallback = std::function<void(uint16_t connHandle,
                                                         const DiscoveredService* service,
                                                         bool complete)>;

    /**
     * Called when characteristic read completes
     */
    using CharacteristicReadCallback = std::function<void(uint16_t connHandle,
                                                           uint16_t attrHandle,
                                                           const uint8_t* data, uint16_t len)>;

    /**
     * Called when notification/indication received from remote device
     */
    using NotificationCallback = std::function<void(uint16_t connHandle,
                                                      uint16_t attrHandle,
                                                      const uint8_t* data, uint16_t len)>;

    /**
     * Called when write to remote characteristic completes
     */
    using WriteCompleteCallback = std::function<void(uint16_t connHandle,
                                                       uint16_t attrHandle,
                                                       int status)>;

    /**
     * Multi-listener registration for GATT client events.
     * Each module should call removeXxx() in its deinit to avoid stale callbacks.
     * \return Token for removeXxx(), or INVALID_LISTENER on overflow.
     */
    virtual ListenerToken addServiceDiscoveryCallback(ServiceDiscoveryCallback cb) {
        (void)cb; return INVALID_LISTENER;
    }
    virtual ListenerToken addCharacteristicReadCallback(CharacteristicReadCallback cb) {
        (void)cb; return INVALID_LISTENER;
    }
    virtual ListenerToken addNotificationCallback(NotificationCallback cb) {
        (void)cb; return INVALID_LISTENER;
    }
    virtual ListenerToken addWriteCompleteCallback(WriteCompleteCallback cb) {
        (void)cb; return INVALID_LISTENER;
    }

    virtual void removeServiceDiscoveryCallback(ListenerToken token) { (void)token; }
    virtual void removeCharacteristicReadCallback(ListenerToken token) { (void)token; }
    virtual void removeNotificationCallback(ListenerToken token) { (void)token; }
    virtual void removeWriteCompleteCallback(ListenerToken token) { (void)token; }

    /**
     * Legacy single-listener setters retained for source compatibility.
     * Equivalent to clearing all listeners and then add*Callback(cb).
     */
    virtual void setServiceDiscoveryCallback(ServiceDiscoveryCallback cb) { (void)cb; }
    virtual void setCharacteristicReadCallback(CharacteristicReadCallback cb) { (void)cb; }
    virtual void setNotificationCallback(NotificationCallback cb) { (void)cb; }
    virtual void setWriteCompleteCallback(WriteCompleteCallback cb) { (void)cb; }

    /**
     * Get connection handle for current peripheral connection
     * @return Connection handle or 0xFFFF if not connected
     */
    virtual uint16_t getConnectionHandle() const { return 0xFFFF; }
};

// Namespace-level aliases for the multi-listener token type, so callers can
// write cdc::hal::ListenerToken / cdc::hal::INVALID_LISTENER.
using ListenerToken = IBluetoothController::ListenerToken;
inline constexpr ListenerToken INVALID_LISTENER = IBluetoothController::INVALID_LISTENER;

// Factory function
IBluetoothController* getBluetoothControllerInstance();

/**
 * Stack-independent BLE advertising data parser.
 * Parses raw AD structures per BLE Core Spec Vol 3, Part C, Section 11.
 */
namespace BleAdvParser {
    /**
     * Find manufacturer-specific data in advertising data
     * @return true if found
     */
    bool findManufacturerData(const uint8_t* advData, uint8_t len,
                               uint16_t* companyId, const uint8_t** data, uint8_t* dataLen);

    /**
     * Check if a 128-bit service UUID is advertised
     * @return true if found
     */
    bool findServiceUuid128(const uint8_t* advData, uint8_t len,
                              const uint8_t uuid128[16]);

    /**
     * Extract device name from advertising data
     * @return true if found
     */
    bool findName(const uint8_t* advData, uint8_t len,
                    char* name, uint8_t nameMaxLen);
}

} // namespace cdc::hal
