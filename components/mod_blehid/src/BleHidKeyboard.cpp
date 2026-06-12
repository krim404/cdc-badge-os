/**
 * \file
 * \brief BLE HID Keyboard implementation via IBluetoothController API.
 *
 * Uses IBluetoothController API exclusively - no direct NimBLE dependency.
 */

#include "mod_blehid/BleHidKeyboard.h"
#include "cdc_keyboard/KeyboardReportMap.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_hal/IPowerManager.h"
#include "cdc_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>

static const char* TAG = "BleHID";

namespace cdc::mod_blehid {

/** \brief Standard 8-byte boot keyboard input report payload. */
struct KeyboardReport {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycodes[6];
};

/** \brief Value of the HID Information characteristic (v1.11, generic keyboard). */
static const uint8_t HID_INFO[] = {
    0x11, 0x01,  // HID version 1.11
    0x00,        // Country code (0 = not localized)
    0x02         // Flags: Normally connectable, not remote wake
};

/** \brief HID protocol mode (`0` boot, `1` report). */
static uint8_t s_protocolMode = 1;

/** \brief Last transmitted keyboard report state. */
static KeyboardReport s_currentReport = {};

/** \brief Handle assigned by stack for the keyboard input report characteristic. */
static uint16_t s_reportHandle = 0;

/** \brief Persistent GATT characteristic storage (must outlive registration). */
static hal::GattCharacteristic s_gattChars[5];

/** \brief Persistent GATT service definition. */
static hal::GattServiceDef s_gattSvcDef;

/** \brief Singleton pointer used by static callbacks. */
static BleHidKeyboard* s_instance = nullptr;

/** \brief Tokens for cleanup of BLE controller callbacks during deinit. */
static hal::IBluetoothController::ListenerToken s_tokConn =
    hal::IBluetoothController::INVALID_LISTENER;
static hal::IBluetoothController::ListenerToken s_tokDisconn =
    hal::IBluetoothController::INVALID_LISTENER;

// ============================================================================
// Device Information + Battery services (HOGP companions)
// ============================================================================

/** \brief GAP Appearance advertised so hosts categorize the badge as a keyboard. */
static constexpr uint16_t BLE_APPEARANCE_HID_KEYBOARD = 0x03C1;

/** \brief Device Information Service string values. */
static constexpr const char* DIS_MANUFACTURER = "CDC";
static constexpr const char* DIS_MODEL = "CDC Badge v1.0";

/** \brief PnP ID (0x2A50): USB-IF vendor source, VID 0x303A, PID 0xBADE, version 1.0. */
static const uint8_t DIS_PNP_ID[] = {
    0x02,        // Vendor ID Source: USB Implementer's Forum
    0x3A, 0x30,  // Vendor ID (little-endian): 0x303A
    0xDE, 0xBA,  // Product ID (little-endian): 0xBADE
    0x00, 0x01   // Product Version (little-endian): 0x0100
};

/** \brief Persistent storage for the Device Information Service (0x180A). */
static hal::GattCharacteristic s_disChars[4];
static hal::GattServiceDef s_disSvcDef;

/** \brief Persistent storage for the Battery Service (0x180F). */
static hal::GattCharacteristic s_batChars[1];
static hal::GattServiceDef s_batSvcDef;

/** \brief Handle assigned by the stack for the Battery Level characteristic. */
static uint16_t s_batteryHandle = 0;

/** \brief Copies a fixed C-string into a GATT read buffer. */
static int disReadString(const char* value, uint8_t* buf, uint16_t* len) {
    uint16_t n = static_cast<uint16_t>(strlen(value));
    if (n > *len) n = *len;
    memcpy(buf, value, n);
    *len = n;
    return 0;
}

/**
 * \brief Registers the Device Information (0x180A) and Battery (0x180F) services.
 * \param ble Bluetooth controller to register against.
 *
 * Both are open-read companions expected by HOGP hosts, registered alongside the
 * HID service and removed again in unregisterAuxServices().
 */
static void registerAuxServices(hal::IBluetoothController* ble) {
    using namespace hal;

    // Manufacturer Name (0x2A29)
    s_disChars[0].uuid = BleUuid::from16(0x2A29);
    s_disChars[0].properties = GattProp::READ;
    s_disChars[0].permissions = GattPerm::READ;
    s_disChars[0].valueHandle = nullptr;
    s_disChars[0].onWrite = nullptr;
    s_disChars[0].onRead = [](uint16_t, uint16_t, uint8_t* buf, uint16_t* len) -> int {
        return disReadString(DIS_MANUFACTURER, buf, len);
    };

    // Model Number (0x2A24)
    s_disChars[1].uuid = BleUuid::from16(0x2A24);
    s_disChars[1].properties = GattProp::READ;
    s_disChars[1].permissions = GattPerm::READ;
    s_disChars[1].valueHandle = nullptr;
    s_disChars[1].onWrite = nullptr;
    s_disChars[1].onRead = [](uint16_t, uint16_t, uint8_t* buf, uint16_t* len) -> int {
        return disReadString(DIS_MODEL, buf, len);
    };

    // Firmware Revision (0x2A26)
    s_disChars[2].uuid = BleUuid::from16(0x2A26);
    s_disChars[2].properties = GattProp::READ;
    s_disChars[2].permissions = GattPerm::READ;
    s_disChars[2].valueHandle = nullptr;
    s_disChars[2].onWrite = nullptr;
    s_disChars[2].onRead = [](uint16_t, uint16_t, uint8_t* buf, uint16_t* len) -> int {
        return disReadString(APP_VERSION, buf, len);
    };

    // PnP ID (0x2A50)
    s_disChars[3].uuid = BleUuid::from16(0x2A50);
    s_disChars[3].properties = GattProp::READ;
    s_disChars[3].permissions = GattPerm::READ;
    s_disChars[3].valueHandle = nullptr;
    s_disChars[3].onWrite = nullptr;
    s_disChars[3].onRead = [](uint16_t, uint16_t, uint8_t* buf, uint16_t* len) -> int {
        uint16_t n = sizeof(DIS_PNP_ID);
        if (n > *len) n = *len;
        memcpy(buf, DIS_PNP_ID, n);
        *len = n;
        return 0;
    };

    s_disSvcDef.uuid = BleUuid::from16(0x180A);
    s_disSvcDef.characteristics = s_disChars;
    s_disSvcDef.numCharacteristics = 4;
    if (!ble->registerGattService(s_disSvcDef)) {
        LOG_W(TAG, "Failed to register Device Information service");
    }

    // Battery Level (0x2A19), read + notify
    s_batChars[0].uuid = BleUuid::from16(0x2A19);
    s_batChars[0].properties = GattProp::READ | GattProp::NOTIFY;
    s_batChars[0].permissions = GattPerm::READ;
    s_batChars[0].valueHandle = &s_batteryHandle;
    s_batChars[0].onWrite = nullptr;
    s_batChars[0].onRead = [](uint16_t, uint16_t, uint8_t* buf, uint16_t* len) -> int {
        uint8_t pct = 0;
        if (auto* pm = getPowerManagerInstance()) pct = pm->getBatteryPercent();
        buf[0] = pct;
        *len = 1;
        return 0;
    };

    s_batSvcDef.uuid = BleUuid::from16(0x180F);
    s_batSvcDef.characteristics = s_batChars;
    s_batSvcDef.numCharacteristics = 1;
    if (!ble->registerGattService(s_batSvcDef)) {
        LOG_W(TAG, "Failed to register Battery service");
    }
}

/** \brief Unregisters the Device Information and Battery services. */
static void unregisterAuxServices(hal::IBluetoothController* ble) {
    ble->unregisterGattService(hal::BleUuid::from16(0x180A));
    ble->unregisterGattService(hal::BleUuid::from16(0x180F));
}

// ============================================================================
// Singleton
// ============================================================================

BleHidKeyboard& BleHidKeyboard::instance() {
    static BleHidKeyboard inst;
    return inst;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool BleHidKeyboard::init() {
    if (initialized_) return true;

    s_instance = this;
    loadSettings();

    auto* ble = hal::getBluetoothControllerInstance();
    if (!ble) {
        LOG_E(TAG, "Bluetooth controller not available");
        return false;
    }

    if (!ble->isEnabled()) {
        if (!ble->enable()) {
            LOG_E(TAG, "Failed to enable Bluetooth");
            return false;
        }
    }

    // Wait for BLE stack to sync
    for (int i = 0; i < 50 && !ble->isEnabled(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    using namespace hal;

    // Report Reference descriptor for the keyboard input report.
    // Layout: [reportId, reportType] where reportType=1 is Input.
    static const GattDescriptor s_inputReportRefDesc = {
        GattDescriptorKind::REPORT_REFERENCE,
        { 0x01, 0x01, 0x00, 0x00 },
        2,
    };

    // HID Information (read-only, encrypted)
    s_gattChars[0].uuid = BleUuid::from16(0x2A4A);
    s_gattChars[0].properties = GattProp::READ;
    s_gattChars[0].permissions = GattPerm::READ_ENC;
    s_gattChars[0].valueHandle = nullptr;
    s_gattChars[0].onWrite = nullptr;
    s_gattChars[0].onRead = [](uint16_t, uint16_t, uint8_t* buf, uint16_t* len) -> int {
        uint16_t copyLen = sizeof(HID_INFO);
        if (copyLen > *len) copyLen = *len;
        memcpy(buf, HID_INFO, copyLen);
        *len = copyLen;
        return 0;
    };

    // Report Map (read-only, encrypted)
    s_gattChars[1].uuid = BleUuid::from16(0x2A4B);
    s_gattChars[1].properties = GattProp::READ;
    s_gattChars[1].permissions = GattPerm::READ_ENC;
    s_gattChars[1].valueHandle = nullptr;
    s_gattChars[1].onWrite = nullptr;
    s_gattChars[1].onRead = [](uint16_t, uint16_t, uint8_t* buf, uint16_t* len) -> int {
        size_t mapSize = keyboard::getHidReportMapSize();
        uint16_t copyLen = static_cast<uint16_t>(mapSize > *len ? *len : mapSize);
        memcpy(buf, keyboard::getHidReportMap(), copyLen);
        *len = copyLen;
        return 0;
    };

    // Keyboard Input Report (read + notify, encrypted, with Report Reference descriptor)
    s_gattChars[2].uuid = BleUuid::from16(0x2A4D);
    s_gattChars[2].properties = GattProp::READ | GattProp::NOTIFY;
    s_gattChars[2].permissions = GattPerm::READ_ENC;
    s_gattChars[2].valueHandle = &s_reportHandle;
    s_gattChars[2].onWrite = nullptr;
    s_gattChars[2].onRead = [](uint16_t, uint16_t, uint8_t* buf, uint16_t* len) -> int {
        uint16_t copyLen = sizeof(s_currentReport);
        if (copyLen > *len) copyLen = *len;
        memcpy(buf, &s_currentReport, copyLen);
        *len = copyLen;
        return 0;
    };
    s_gattChars[2].descriptors = &s_inputReportRefDesc;
    s_gattChars[2].numDescriptors = 1;

    // HID Control Point (write-only, no response, encrypted)
    s_gattChars[3].uuid = BleUuid::from16(0x2A4C);
    s_gattChars[3].properties = GattProp::WRITE_NO_RSP;
    s_gattChars[3].permissions = GattPerm::WRITE_ENC;
    s_gattChars[3].valueHandle = nullptr;
    s_gattChars[3].onRead = nullptr;
    s_gattChars[3].onWrite = [](uint16_t, uint16_t, const uint8_t* data, uint16_t len) -> int {
        if (len >= 1 && s_instance) {
            if (data[0] == 0) s_instance->onHostSuspend();
            else if (data[0] == 1) s_instance->onHostResume();
        }
        return 0;
    };

    // Protocol Mode (read + write-no-response, encrypted)
    s_gattChars[4].uuid = BleUuid::from16(0x2A4E);
    s_gattChars[4].properties = GattProp::READ | GattProp::WRITE_NO_RSP;
    s_gattChars[4].permissions = GattPerm::READ_ENC | GattPerm::WRITE_ENC;
    s_gattChars[4].valueHandle = nullptr;
    s_gattChars[4].onRead = [](uint16_t, uint16_t, uint8_t* buf, uint16_t* len) -> int {
        buf[0] = s_protocolMode;
        *len = 1;
        return 0;
    };
    s_gattChars[4].onWrite = [](uint16_t, uint16_t, const uint8_t* data, uint16_t len) -> int {
        if (len >= 1) s_protocolMode = data[0];
        return 0;
    };

    // Register GATT service via API
    s_gattSvcDef.uuid = BleUuid::from16(0x1812);  // HID Service
    s_gattSvcDef.characteristics = s_gattChars;
    s_gattSvcDef.numCharacteristics = 5;

    if (!ble->registerGattService(s_gattSvcDef)) {
        LOG_E(TAG, "Failed to register HID GATT service");
        return false;
    }

    // Companion services expected by HOGP hosts (Device Information, Battery)
    registerAuxServices(ble);

    // Register connection callbacks via API
    s_tokConn = ble->addConnectionCallback([](uint16_t connHandle) {
        if (s_instance) s_instance->onConnect(connHandle);
    });
    s_tokDisconn = ble->addDisconnectionCallback([](uint16_t connHandle, int reason) {
        if (s_instance) s_instance->onDisconnect(connHandle, reason);
    });

    initialized_ = true;
    snprintf(statusText_, sizeof(statusText_), "Ready (not advertising)");
    LOG_I(TAG, "BLE HID Keyboard initialized (reportHandle=%d)", s_reportHandle);
    return true;
}

void BleHidKeyboard::deinit() {
    if (!initialized_) return;

    stopAdvertising();
    if (auto* ble = hal::getBluetoothControllerInstance()) {
        ble->removeConnectionCallback(s_tokConn);
        ble->removeDisconnectionCallback(s_tokDisconn);
        unregisterAuxServices(ble);
    }
    s_tokConn = hal::IBluetoothController::INVALID_LISTENER;
    s_tokDisconn = hal::IBluetoothController::INVALID_LISTENER;
    s_instance = nullptr;
    initialized_ = false;
    snprintf(statusText_, sizeof(statusText_), "Not initialized");
    LOG_I(TAG, "BLE HID Keyboard deinitialized");
}

// ============================================================================
// Connection state
// ============================================================================

bool BleHidKeyboard::isConnected() const {
    auto* ble = hal::getBluetoothControllerInstance();
    return ble && ble->isConnected();
}

// ============================================================================
// Advertising via API
// ============================================================================

bool BleHidKeyboard::startAdvertising() {
    if (!initialized_ || advertising_) return advertising_;

    auto* ble = hal::getBluetoothControllerInstance();
    if (!ble || !ble->isEnabled()) return false;

    // Advertise as a HID keyboard: appearance + HID service UUID
    ble->setAppearance(BLE_APPEARANCE_HID_KEYBOARD);
    ble->addAdvertisingUuid(hal::BleUuid::from16(0x1812));

    advertising_ = true;
    snprintf(statusText_, sizeof(statusText_), "Advertising...");
    LOG_I(TAG, "BLE HID advertising started");
    return true;
}

void BleHidKeyboard::stopAdvertising() {
    if (!advertising_) return;

    auto* ble = hal::getBluetoothControllerInstance();
    if (ble) {
        ble->removeAdvertisingUuid(hal::BleUuid::from16(0x1812));
        ble->setAppearance(0);
    }

    advertising_ = false;
    if (!isConnected()) {
        snprintf(statusText_, sizeof(statusText_), "Ready (not advertising)");
    }
    LOG_I(TAG, "BLE HID advertising stopped");
}

bool BleHidKeyboard::isAdvertising() const {
    return advertising_;
}

bool BleHidKeyboard::setDiscoverable(bool on) {
    if (on) {
        if (!initialized_ && !init()) return false;
        return startAdvertising();
    }
    stopAdvertising();
    return true;
}

// ============================================================================
// Connection callbacks
// ============================================================================

void BleHidKeyboard::onConnect(uint16_t connHandle) {
    advertising_ = false;
    snprintf(statusText_, sizeof(statusText_), "Connected");
    LOG_I(TAG, "HID device connected (handle=%d)", connHandle);
}

void BleHidKeyboard::onDisconnect(uint16_t connHandle, int reason) {
    (void)connHandle;
    engine_.cancel();

    snprintf(statusText_, sizeof(statusText_), "Disconnected (reason=%d)", reason);
    LOG_I(TAG, "HID device disconnected (reason=%d)", reason);
}

void BleHidKeyboard::onHostSuspend() {
    LOG_D(TAG, "Host suspended");
}

void BleHidKeyboard::onHostResume() {
    LOG_D(TAG, "Host resumed");
}

// ============================================================================
// Key report delivery (IKeyReportSink -> BLE GATT notification)
// ============================================================================

bool BleHidKeyboard::sendKeyReport(uint8_t modifier, const uint8_t* keycodes,
                                    uint8_t numKeys) {
    if (!isConnected() || s_reportHandle == 0) return false;

    auto* ble = hal::getBluetoothControllerInstance();
    if (!ble) return false;

    static_assert(sizeof(s_currentReport) == keyboard::kBootReportSize,
                  "KeyboardReport must match the packed boot-report layout");
    keyboard::packBootReport(modifier, keycodes, numKeys,
                             reinterpret_cast<uint8_t*>(&s_currentReport));

    return ble->sendNotification(
        ble->getConnectionHandle(), s_reportHandle,
        reinterpret_cast<const uint8_t*>(&s_currentReport),
        sizeof(s_currentReport));
}

// ============================================================================
// Status text (transport-specific; typing surface lives in the base)
// ============================================================================

const char* BleHidKeyboard::getStatusText() const {
    return statusText_;
}

} // namespace cdc::mod_blehid
