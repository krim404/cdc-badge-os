/**
 * \file
 * \brief BLE serial module wiring NUS transport to console hooks and UI toggle.
 */

#include "mod_ble_serial/BleSerialModule.h"
#include "mod_ble_serial/BleUartService.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ToastView.h"
#include "cdc_log.h"
#include "nvs.h"
#include <cstring>

static const char* TAG = "BLE_SERIAL";

namespace cdc::mod_ble_serial {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_ble_serial.title",    "BLE Serial"},
    {"mod_ble_serial.enabled",  "Enabled"},
    {"mod_ble_serial.disabled", "Disabled"},
};

void BleSerialModule::registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

/** \brief Console hook bridge between shell I/O and BLE UART transport. */

/**
 * \brief Console output hook that forwards bytes to the BLE UART service.
 * \param data Pointer to output bytes.
 * \param len Number of bytes to forward.
 * \return void
 */
static void bleOutputHook(const char* data, size_t len) {
    auto& uart = BleUartService::instance();
    if (uart.isConnected() && uart.isInitialized()) {
        uart.send(reinterpret_cast<const uint8_t*>(data), len);
    }
}

/**
 * \brief Console input-available hook using BLE UART RX queue.
 * \return `true` when BLE serial has pending input.
 */
static bool bleInputAvailableHook() {
    auto& uart = BleUartService::instance();
    return uart.isConnected() && uart.available() > 0;
}

/**
 * \brief Console getchar hook reading one byte from BLE UART.
 * \return Character value or negative when unavailable.
 */
static int bleInputGetcharHook() {
    auto& uart = BleUartService::instance();
    return uart.getchar();
}

/**
 * \brief Registers BLE-backed console input/output hooks.
 */
void BleSerialModule::registerConsoleHooks() {
    console_register_output_hook(bleOutputHook);
    console_register_input_hook(bleInputAvailableHook, bleInputGetcharHook);
    LOG_I(TAG, "Console hooks registered");
}

/**
 * \brief Unregisters BLE-backed console hooks.
 */
void BleSerialModule::unregisterConsoleHooks() {
    console_register_output_hook(nullptr);
    console_register_input_hook(nullptr, nullptr);
    LOG_I(TAG, "Console hooks unregistered");
}

/**
 * \brief No-op: AppUi owns the shared numeric-comparison pairing callback.
 */
void BleSerialModule::registerPairingCallback() {
}

/** \brief Persistent settings helpers. */

/**
 * \brief Loads module enable-state from NVS.
 */
void BleSerialModule::loadSettings() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(nvs, "enabled", &val) == ESP_OK) {
            enabled_ = (val != 0);
        }
        nvs_close(nvs);
    }
}

/**
 * \brief Persists module enable-state to NVS.
 */
void BleSerialModule::saveSettings() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "enabled", enabled_ ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/** \brief BLE serial module lifecycle implementation. */

/**
 * \brief Returns singleton BLE serial module instance.
 * \return Module singleton reference.
 */
BleSerialModule& BleSerialModule::instance() {
    static BleSerialModule inst;
    return inst;
}

/**
 * \brief Initializes BLE serial module resources and settings.
 * \return `true` on successful initialization.
 */
bool BleSerialModule::init() {
    LOG_I(TAG, "Initializing BLE Serial module");

    registerStrings();
    loadSettings();

    state_ = core::ServiceState::INITIALIZED;
    return true;
}

/**
 * \brief Starts BLE serial module and optionally auto-enables service.
 * \return `true` if start transition succeeded.
 */
bool BleSerialModule::start() {
    if (state_ != core::ServiceState::INITIALIZED &&
        state_ != core::ServiceState::STOPPED) {
        return false;
    }

    // If auto-enable is set and BLE is enabled, start the service
    if (enabled_) {
        auto* ble = hal::getBluetoothControllerInstance();
        if (ble && ble->isEnabled()) {
            auto& uart = BleUartService::instance();
            if (uart.init()) {
                registerConsoleHooks();
                registerPairingCallback();
                LOG_I(TAG, "BLE Serial service started");
            }
        }
    }

    state_ = core::ServiceState::STARTED;
    return true;
}

/**
 * \brief Stops BLE serial module and deinitializes UART service when enabled.
 */
void BleSerialModule::stop() {
    if (enabled_) {
        unregisterConsoleHooks();
        BleUartService::instance().deinit();
    }
    state_ = core::ServiceState::STOPPED;
}

/**
 * \brief Toggles BLE serial service state and updates persisted setting.
 */
void BleSerialModule::toggle() {
    auto* ble = hal::getBluetoothControllerInstance();
    if (!ble) {
        ui::showToastError("BLE n/a");
        return;
    }

    if (!ble->isEnabled()) {
        ui::showToastError("BLE disabled");
        return;
    }

    enabled_ = !enabled_;
    saveSettings();

    if (enabled_) {
        auto& uart = BleUartService::instance();
        if (uart.init()) {
            registerConsoleHooks();
            registerPairingCallback();
            ui::showToastSuccess(ui::tr("mod_ble_serial.enabled"));
        } else {
            enabled_ = false;
            saveSettings();
            ui::showToastError("Init failed");
        }
    } else {
        unregisterConsoleHooks();
        BleUartService::instance().deinit();
        ui::showToastInfo(ui::tr("mod_ble_serial.disabled"));
    }
}

/**
 * \brief Provides Bluetooth-menu item for BLE serial toggle.
 * \param items Output menu item array.
 * \param maxItems Maximum writable entries.
 * \return Number of populated menu items.
 */
uint8_t BleSerialModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    // Build label with status
    snprintf(labelBuf_, LABEL_BUF_SIZE, "%s: %s",
             ui::tr("mod_ble_serial.title"),
             enabled_ ? ui::tr("mod_ble_serial.enabled") : ui::tr("mod_ble_serial.disabled"));

    items[0].label = labelBuf_;
    items[0].priority = 50;
    items[0].getView = nullptr;
    items[0].isVisible = nullptr;
    items[0].moduleName = getName();
    items[0].location = core::MenuLocation::BLUETOOTH_MENU;
    items[0].onSelect = []() { BleSerialModule::instance().toggle(); };

    return 1;
}

/**
 * \brief Periodic module tick hook.
 * \param nowMs Current uptime in milliseconds.
 */
void BleSerialModule::onTick(uint32_t nowMs) {
    (void)nowMs;
    // Could check connection state changes here if needed
}

} // namespace cdc::mod_ble_serial

/**
 * \brief Registers BLE serial module initializer.
 */
extern "C" void mod_ble_serial_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& moduleReg = cdc::core::ModuleRegistry::instance();
        auto& module = cdc::mod_ble_serial::BleSerialModule::instance();

        moduleReg.registerModule(&module);

        if (!module.init()) {
            moduleReg.reportModuleError(module.getName(), "Init failed");
            return;
        }
        module.start();
    });
}
