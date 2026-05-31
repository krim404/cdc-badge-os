/**
 * \file
 * \brief BLE Nordic UART Service implementation via platform Bluetooth abstraction.
 */

#include "mod_ble_serial/BleUartService.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_log.h"
#include <cstring>

static const char* TAG = "BLE_UART";

namespace cdc::mod_ble_serial {

/** \brief NUS UUIDs in little-endian byte order for `BleUuid::from128`. */
static const uint8_t NUS_SVC_UUID[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e
};
/** \brief RX characteristic UUID (phone writes to badge). */
static const uint8_t NUS_RX_UUID[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e
};
/** \brief TX characteristic UUID (badge notifies phone). */
static const uint8_t NUS_TX_UUID[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e
};

/**
 * \brief Returns singleton BLE UART service instance.
 * \return Service singleton reference.
 */
BleUartService& BleUartService::instance() {
    static BleUartService* inst = new BleUartService();
    return *inst;
}

/**
 * \brief Initializes Nordic UART Service over BLE GATT.
 * \return `true` on successful initialization.
 */
bool BleUartService::init() {
    if (initialized_) {
        return true;
    }

    auto* ble = cdc::hal::getBluetoothControllerInstance();
    if (!ble || !ble->isEnabled()) {
        LOG_E(TAG, "BLE not enabled");
        return false;
    }

    // Define GATT characteristics via API types
    using namespace cdc::hal;

    static GattCharacteristic chars[2];

    // RX characteristic (phone writes to badge)
    chars[0].uuid = BleUuid::from128(NUS_RX_UUID);
    chars[0].properties = GattProp::WRITE | GattProp::WRITE_NO_RSP;
    chars[0].permissions = GattPerm::WRITE;
    chars[0].valueHandle = nullptr;
    chars[0].onWrite = [](uint16_t /*connHandle*/, uint16_t /*attrHandle*/,
                          const uint8_t* data, uint16_t len) -> int {
        BleUartService::instance().onRxData(data, len);
        return 0;
    };
    chars[0].onRead = nullptr;

    // TX characteristic (badge notifies phone)
    chars[1].uuid = BleUuid::from128(NUS_TX_UUID);
    chars[1].properties = GattProp::NOTIFY;
    chars[1].permissions = GattPerm::READ;
    chars[1].valueHandle = &txCharHandle_;
    chars[1].onWrite = nullptr;
    chars[1].onRead = nullptr;

    // Register GATT service
    static GattServiceDef svcDef;
    svcDef.uuid = BleUuid::from128(NUS_SVC_UUID);
    svcDef.characteristics = chars;
    svcDef.numCharacteristics = 2;

    if (!ble->registerGattService(svcDef)) {
        LOG_E(TAG, "Failed to register NUS GATT service");
        return false;
    }

    // Advertise NUS service UUID so scanners can find us
    ble->addAdvertisingUuid(BleUuid::from128(NUS_SVC_UUID));

    // Register connection callbacks
    connToken_ = ble->addConnectionCallback([](uint16_t /*connHandle*/) {
        BleUartService::instance().onConnectionChange(true);
    });
    disconnToken_ = ble->addDisconnectionCallback([](uint16_t /*connHandle*/, int /*reason*/) {
        BleUartService::instance().onConnectionChange(false);
    });

    // Reset RX buffer
    rxHead_.store(0, std::memory_order_relaxed);
    rxTail_.store(0, std::memory_order_relaxed);

    initialized_ = true;
    LOG_I(TAG, "NUS service registered (txHandle=%d)", txCharHandle_);
    return true;
}

/**
 * \brief Deinitializes BLE UART service runtime state.
 */
void BleUartService::deinit() {
    if (!initialized_) {
        return;
    }

    auto* ble = cdc::hal::getBluetoothControllerInstance();
    if (ble) {
        ble->removeAdvertisingUuid(cdc::hal::BleUuid::from128(NUS_SVC_UUID));
        ble->removeConnectionCallback(connToken_);
        ble->removeDisconnectionCallback(disconnToken_);
        connToken_ = cdc::hal::IBluetoothController::INVALID_LISTENER;
        disconnToken_ = cdc::hal::IBluetoothController::INVALID_LISTENER;
    }

    initialized_ = false;
    LOG_I(TAG, "NUS service deinitialized");
}

/**
 * \brief Sends binary payload to connected BLE peer via notifications.
 * \param data Data buffer.
 * \param len Data length.
 * \return Number of bytes sent.
 */
size_t BleUartService::send(const uint8_t* data, size_t len) {
    if (!initialized_ || !isConnected() || !data || len == 0) {
        return 0;
    }

    // Recursion guard - prevent logging from causing infinite loop
    if (txInProgress_) {
        return 0;
    }
    txInProgress_ = true;

    auto* ble = cdc::hal::getBluetoothControllerInstance();
    if (!ble) {
        txInProgress_ = false;
        return 0;
    }

    uint16_t connHandle = ble->getConnectionHandle();
    uint16_t mtu = ble->getMtu();
    // getMtu() already subtracts the 3-byte ATT header, but defend against
    // implementations that return the raw ATT MTU instead.
    if (mtu < 4) {
        txInProgress_ = false;
        return 0;
    }

    size_t sent = 0;
    while (sent < len) {
        size_t remaining = len - sent;
        uint16_t chunkLen = static_cast<uint16_t>(
            (remaining > mtu) ? mtu : remaining);

        if (!ble->sendNotification(connHandle, txCharHandle_, data + sent, chunkLen)) {
            break;
        }
        sent += chunkLen;
    }

    txInProgress_ = false;
    return sent;
}

/**
 * \brief Sends null-terminated string via BLE UART notifications.
 * \param str String to send.
 * \return Number of bytes sent.
 */
size_t BleUartService::send(const char* str) {
    if (!str) return 0;
    return send(reinterpret_cast<const uint8_t*>(str), strlen(str));
}

/**
 * \brief Returns whether TX path is currently ready.
 * \return `true` when initialized, connected, and uncongested.
 */
bool BleUartService::txReady() const {
    return initialized_ && isConnected() && !txCongested_;
}

/**
 * \brief Returns number of buffered RX bytes.
 * \return Available byte count.
 */
size_t BleUartService::available() const {
    if (!initialized_) return 0;

    size_t head = rxHead_.load(std::memory_order_acquire);
    size_t tail = rxTail_.load(std::memory_order_acquire);

    if (head >= tail) {
        return head - tail;
    } else {
        return RX_BUFFER_SIZE - tail + head;
    }
}

/**
 * \brief Reads one byte from RX ring buffer.
 * \return Byte value or `-1` when empty.
 */
int BleUartService::getchar() {
    if (!initialized_ || available() == 0) {
        return -1;
    }

    size_t tail = rxTail_.load(std::memory_order_relaxed);
    uint8_t c = rxBuffer_[tail];
    rxTail_.store((tail + 1) % RX_BUFFER_SIZE, std::memory_order_release);
    return c;
}

/**
 * \brief Reads up to `maxLen` bytes from RX ring buffer.
 * \param buf Output buffer.
 * \param maxLen Maximum bytes to read.
 * \return Number of bytes read.
 */
size_t BleUartService::read(uint8_t* buf, size_t maxLen) {
    if (!buf || maxLen == 0) return 0;

    size_t count = 0;
    while (count < maxLen && available() > 0) {
        int c = getchar();
        if (c < 0) break;
        buf[count++] = static_cast<uint8_t>(c);
    }
    return count;
}

/**
 * \brief Returns whether BLE link is currently connected.
 * \return `true` if controller reports active connection.
 */
bool BleUartService::isConnected() const {
    auto* ble = cdc::hal::getBluetoothControllerInstance();
    return ble && ble->isConnected();
}

/**
 * \brief Appends received BLE UART data into RX ring buffer.
 * \param data Received payload.
 * \param len Payload length.
 */
void BleUartService::onRxData(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    size_t head = rxHead_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < len; i++) {
        size_t nextHead = (head + 1) % RX_BUFFER_SIZE;
        size_t tail = rxTail_.load(std::memory_order_acquire);
        if (nextHead == tail) {
            // Buffer full - drop oldest
            rxTail_.store((tail + 1) % RX_BUFFER_SIZE, std::memory_order_release);
        }
        rxBuffer_[head] = data[i];
        head = nextHead;
    }
    rxHead_.store(head, std::memory_order_release);
}

/**
 * \brief Handles BLE connection state changes.
 * \param connected New connection state.
 */
void BleUartService::onConnectionChange(bool connected) {
    if (connected) {
        LOG_I(TAG, "Device connected");
        if (onConnect_) onConnect_();
    } else {
        LOG_I(TAG, "Device disconnected");
        rxHead_.store(0, std::memory_order_relaxed);
        rxTail_.store(0, std::memory_order_relaxed);
        if (onDisconnect_) onDisconnect_();
    }
}

} // namespace cdc::mod_ble_serial
