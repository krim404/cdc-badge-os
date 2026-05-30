/**
 * \file
 * \brief Bluetooth UI flow for toggling BLE, status display, and device scanning.
 */

#include "AppUiInternal.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_log.h"

#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

namespace cdc::ui {

static const char* TAG = "BleMenu";

/** \brief Bluetooth fixed menu indices and capacity limits. */

enum BluetoothMenuIdx {
    BT_IDX_ENABLE = 0,
    BT_IDX_STATUS,
    BT_IDX_SCAN,
    BT_IDX_FORGET_BONDS,
    BT_IDX_FIXED_COUNT
};

static constexpr uint8_t BT_MENU_MAX_ITEMS = 16;
// Single source of truth: mirror the controller's scan-buffer capacity.
static constexpr uint8_t BLE_MAX_SCAN_RESULTS = hal::IBluetoothController::MAX_SCAN_RESULTS;
static constexpr uint32_t BLE_SCAN_TIMEOUT_MS = 8000;

/** \brief Bluetooth menu and scan state. */

static ListView* s_bluetoothMenu = nullptr;
static ListItem s_bluetoothItems[BT_MENU_MAX_ITEMS];
static core::ModuleMenuItem s_bluetoothModuleItems[12];
static uint8_t s_bluetoothModuleCount = 0;

/** \brief BLE scan result list state. */
static ListView* s_bleScanView = nullptr;
static ListItem s_bleScanItems[BLE_MAX_SCAN_RESULTS];
static EXT_RAM_BSS_ATTR hal::BleScanResult s_bleScanResults[BLE_MAX_SCAN_RESULTS];
static uint8_t s_bleScanCount = 0;

/** \brief PSRAM-backed text buffer used for BLE status details. */
static constexpr size_t BLE_STATUS_BUF_SIZE = 256;
static EXT_RAM_BSS_ATTR char s_bleStatusBuf[BLE_STATUS_BUF_SIZE];

/** \brief Handles Bluetooth menu selection. */
static void onBluetoothMenuSelect(uint16_t index, void* userData);
/** \brief Toggles BLE controller enabled state. */
static void toggleBluetoothEnable();
/** \brief Shows BLE status and adapter information. */
static void showBluetoothStatus();
/** \brief Starts BLE scan and opens result list. */
static void startBluetoothScan();

/**
 * \brief Renders one BLE scan row with signal bars and RSSI text.
 * \param gfx Display graphics context.
 * \param item List item containing `BleScanResult` in `userData`.
 * \param index Row index.
 * \param x Row left position.
 * \param y Row top position.
 * \param w Row width.
 * \param h Row height.
 * \param selected Whether row is selected.
 * \param userCtx Optional renderer context.
 * \return `true` when rendered.
 */
static bool renderBleRow(Gdey029T94* gfx, const ListItem& item,
                         uint16_t index, int x, int y, int w, int h,
                         bool selected, void* userCtx) {
    (void)index;
    (void)h;
    (void)w;
    (void)userCtx;
    if (!gfx || !item.userData) return false;

    const auto* dev = reinterpret_cast<const hal::BleScanResult*>(item.userData);

    gfx->setFont(nullptr);
    gfx->setTextSize(1);
    int baseline = y + 6;

    drawSignalBars(gfx, x + 4, baseline - 6, dev->rssi, selected);

    // Device name (truncated to fit available width)
    char nameDisplay[36];
    strncpy(nameDisplay, dev->name, sizeof(nameDisplay) - 1);
    nameDisplay[sizeof(nameDisplay) - 1] = '\0';
    gfx->setCursor(x + 22, baseline);
    render::printText(gfx, nameDisplay);

    // RSSI value on the right
    char rssiBuf[8];
    snprintf(rssiBuf, sizeof(rssiBuf), "%d", dev->rssi);
    int16_t rx1, ry1;
    uint16_t rw, rh;
    gfx->getTextBounds(rssiBuf, 0, 0, &rx1, &ry1, &rw, &rh);
    gfx->setCursor(x + w - rw - 4, baseline);
    gfx->print(rssiBuf);

    return true;
}

/**
 * \brief Sorts BLE scan results by RSSI descending.
 */
static void sortBleScanResults() {
    for (uint8_t i = 0; i < s_bleScanCount; i++) {
        for (uint8_t j = i + 1; j < s_bleScanCount; j++) {
            if (s_bleScanResults[j].rssi > s_bleScanResults[i].rssi) {
                hal::BleScanResult temp = s_bleScanResults[i];
                s_bleScanResults[i] = s_bleScanResults[j];
                s_bleScanResults[j] = temp;
            }
        }
    }
}

/**
 * \brief Rebuilds Bluetooth menu entries and dynamic module items.
 */
void rebuildBluetoothMenu() {
    auto* ble = hal::getBluetoothControllerInstance();
    bool enabled = ble && ble->isEnabled();

    if (enabled) {
        s_bluetoothItems[BT_IDX_ENABLE] = {ui::tr("core.bluetooth_on"), '*', false, nullptr};
    } else {
        s_bluetoothItems[BT_IDX_ENABLE] = {ui::tr("core.bluetooth_off"), 0, false, nullptr};
    }
    s_bluetoothItems[BT_IDX_STATUS] = {ui::tr("core.ble_status"), 0, false, nullptr};
    s_bluetoothItems[BT_IDX_SCAN] = {ui::tr("core.ble_scan"), 0, !enabled, nullptr};
    s_bluetoothItems[BT_IDX_FORGET_BONDS] = {"Forget all bonds", 0, !enabled, nullptr};

    auto& moduleReg = core::ModuleRegistry::instance();
    s_bluetoothModuleCount = moduleReg.getMenuItems(
        core::MenuLocation::BLUETOOTH_MENU,
        s_bluetoothModuleItems,
        BT_MENU_MAX_ITEMS - BT_IDX_FIXED_COUNT
    );

    for (uint8_t i = 0; i < s_bluetoothModuleCount; i++) {
        s_bluetoothItems[BT_IDX_FIXED_COUNT + i] = {s_bluetoothModuleItems[i].label, 0, false, nullptr};
    }

    if (s_bluetoothMenu) {
        s_bluetoothMenu->init(ui::tr("core.bluetooth"), s_bluetoothItems, BT_IDX_FIXED_COUNT + s_bluetoothModuleCount);
    }
}

/**
 * \brief Shows top-level Bluetooth menu.
 */
void showBluetoothMenu() {
    if (!s_bluetoothMenu) {
        s_bluetoothMenu = new ListView();
        s_bluetoothMenu->setOnSelect(onBluetoothMenuSelect);
    }

    rebuildBluetoothMenu();
    ViewStack::instance().push(s_bluetoothMenu);
}

/**
 * \brief Handles Bluetooth menu selection and dispatches actions.
 * \param index Selected menu index.
 * \param userData Optional callback user data.
 */
static void onBluetoothMenuSelect(uint16_t index, void* userData) {
    (void)userData;

    switch (index) {
        case BT_IDX_ENABLE:
            toggleBluetoothEnable();
            return;
        case BT_IDX_STATUS:
            showBluetoothStatus();
            return;
        case BT_IDX_SCAN:
            startBluetoothScan();
            return;
        case BT_IDX_FORGET_BONDS:
            askConfirm("Forget all paired devices?", [](void*) {
                auto* ble = hal::getBluetoothControllerInstance();
                if (ble) {
                    ble->clearAllBonds();
                    showToastSuccess("Bonds cleared");
                }
            });
            return;
    }

    uint8_t moduleIdx = index - BT_IDX_FIXED_COUNT;
    if (moduleIdx < s_bluetoothModuleCount) {
        auto& item = s_bluetoothModuleItems[moduleIdx];
        if (item.getView) {
            IView* view = item.getView();
            if (view) {
                ViewStack::instance().push(view);
            }
        } else if (item.onSelect) {
            item.onSelect();
        }
        rebuildBluetoothMenu();
    }
}

/**
 * \brief Toggles BLE enable state and refreshes related menus.
 */
static void toggleBluetoothEnable() {
    auto* ble = hal::getBluetoothControllerInstance();
    if (!ble) {
        showToastError(ui::tr("core.hw_not_available"));
        return;
    }

    if (ble->isEnabled()) {
        ble->disable();
        showToastInfo(ui::tr("core.bluetooth_off"));
    } else {
        if (ble->enable()) {
            showToastSuccess(ui::tr("core.bluetooth_on"));
        } else {
            showToastError(ui::tr("core.failed"));
        }
    }

    rebuildBluetoothMenu();
    rebuildToolsMenu();
}

/**
 * \brief Builds and shows BLE status information view.
 */
static void showBluetoothStatus() {
    auto* ble = hal::getBluetoothControllerInstance();
    char* info = s_bleStatusBuf;
    memset(info, 0, BLE_STATUS_BUF_SIZE);
    size_t pos = 0;

    auto append = [&](const char* fmt, ...) {
        if (pos >= BLE_STATUS_BUF_SIZE) return;
        va_list args;
        va_start(args, fmt);
        int written = vsnprintf(info + pos, BLE_STATUS_BUF_SIZE - pos, fmt, args);
        va_end(args);
        if (written > 0) {
            size_t w = static_cast<size_t>(written);
            pos += (w < (BLE_STATUS_BUF_SIZE - pos)) ? w : (BLE_STATUS_BUF_SIZE - pos - 1);
        }
    };

    if (ble) {
        append("BLE: %s\n", ble->isEnabled() ? "ON" : "OFF");

        uint8_t mac[6] = {};
        if (ble->getMacAddress(mac)) {
            append("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }

        if (ble->isConnected()) {
            append("\n%s\n", ui::tr("core.ble_connected_to"));
            append("%s: %d dBm\n", ui::tr("core.ble_signal"), ble->getRssi());
        } else {
            append("\n%s\n", ui::tr("core.ble_not_connected"));
        }

        append("\nName: %s\n", ble->getDeviceName());
    }

    showInfo(ui::tr("core.ble_status"), info);
}

// ===========================================================================
// Async BLE device-name resolution
//
// Devices that advertise no name show their MAC. Once the scan list is on
// screen, this state machine iterates over those devices, connects, reads the
// GATT Device Name characteristic (0x2A00 in the Generic Access service
// 0x1800), and updates the row in place. One device at a time, hard-capped at
// BLE_RESOLVE_DEVICE_TIMEOUT_MS each. GATT callbacks run on the NimBLE host
// task and only set flags/data; the UI tick drives transitions and the
// (display-touching) row update. Aborted when the view closes.
// ===========================================================================

static constexpr uint16_t BLE_GAP_SVC_UUID  = 0x1800;  // Generic Access
static constexpr uint16_t BLE_DEV_NAME_UUID = 0x2A00;   // Device Name
static constexpr uint32_t BLE_RESOLVE_DEVICE_TIMEOUT_MS = 5000;
static constexpr uint32_t BLE_RESOLVE_SETTLE_TIMEOUT_MS = 800;

enum class BleResolvePhase : uint8_t { Idle, Begin, Connecting, Discovering, Reading, Settle };

static BleResolvePhase s_resolvePhase   = BleResolvePhase::Idle;
static bool            s_resolveActive   = false;
static uint8_t         s_resolveIndex    = 0;
static uint16_t        s_resolveConn     = 0xFFFF;
static uint16_t        s_resolveNameHandle = 0;
static uint32_t        s_resolveDeviceStartMs = 0;
static uint32_t        s_resolveSettleStartMs = 0;

// Set by NimBLE-task callbacks, consumed by the UI tick.
static volatile bool     s_evtConnected    = false;
static volatile bool     s_evtDiscovered   = false;
static volatile bool     s_evtGotName      = false;
static volatile bool     s_evtDisconnected = false;
static volatile uint16_t s_evtConnHandle   = 0xFFFF;
static volatile uint16_t s_evtNameHandle   = 0;
static char              s_resolveNameBuf[32];

using BleListenerToken = hal::IBluetoothController::ListenerToken;
static constexpr BleListenerToken kInvalidListener = hal::IBluetoothController::INVALID_LISTENER;
static BleListenerToken s_tokResConn = kInvalidListener;
static BleListenerToken s_tokResDisc = kInvalidListener;
static BleListenerToken s_tokResSvc  = kInvalidListener;
static BleListenerToken s_tokResRead = kInvalidListener;

static void bleResolveStop();
static void bleResolveBeginNext(uint32_t nowMs);

/** \brief Formats a scan result's MAC the same way as the no-name fallback. */
static void bleResolveMac(const hal::BleScanResult& d, char* out, size_t n) {
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
             d.mac[5], d.mac[4], d.mac[3], d.mac[2], d.mac[1], d.mac[0]);
}

/** \brief True when the row still shows the MAC fallback (no name yet). */
static bool bleDeviceNeedsName(uint8_t i) {
    char mac[18];
    bleResolveMac(s_bleScanResults[i], mac, sizeof(mac));
    return strcmp(s_bleScanResults[i].name, mac) == 0;
}

static void bleResolveClearEvents() {
    s_evtConnected = false;
    s_evtDiscovered = false;
    s_evtGotName = false;
    s_evtDisconnected = false;
    s_evtConnHandle = 0xFFFF;
    s_evtNameHandle = 0;
}

// ---- GATT callbacks (NimBLE host task) ----
static void rOnConnect(uint16_t connHandle) {
    if (!s_resolveActive) return;
    s_evtConnHandle = connHandle;
    s_evtConnected  = true;
}
static void rOnDisconnect(uint16_t connHandle, int /*reason*/) {
    if (!s_resolveActive) return;
    if (connHandle == s_resolveConn) s_evtDisconnected = true;
}
static void rOnServiceDiscovered(uint16_t connHandle,
        const hal::IBluetoothController::DiscoveredService* svc, bool complete) {
    if (!s_resolveActive || connHandle != s_resolveConn) return;
    if (svc) {
        const hal::BleUuid want = hal::BleUuid::from16(BLE_DEV_NAME_UUID);
        for (uint8_t i = 0; i < svc->numCharacteristics; i++) {
            if (svc->characteristics[i].uuid == want) {
                s_evtNameHandle = svc->characteristics[i].valueHandle;
                break;
            }
        }
    }
    if (complete) s_evtDiscovered = true;
}
static void rOnCharRead(uint16_t connHandle, uint16_t /*attrHandle*/,
        const uint8_t* data, uint16_t len) {
    if (!s_resolveActive || connHandle != s_resolveConn) return;
    uint16_t n = 0;
    if (data && len > 0) {
        const uint16_t cap = sizeof(s_resolveNameBuf) - 1;
        n = len < cap ? len : cap;
        memcpy(s_resolveNameBuf, data, n);
    }
    s_resolveNameBuf[n] = '\0';
    s_evtGotName = true;
}

/** \brief Tears down the current attempt and waits briefly for a clean stack. */
static void bleResolveCleanupToSettle(uint32_t nowMs) {
    auto* ble = hal::getBluetoothControllerInstance();
    if (ble) {
        ble->cancelConnect();
        if (s_resolveConn != 0xFFFF) ble->disconnectHandle(s_resolveConn);
    }
    s_evtDisconnected = false;
    s_resolvePhase = BleResolvePhase::Settle;
    s_resolveSettleStartMs = nowMs;
}

/** \brief Starts resolving the next device that still shows its MAC. */
static void bleResolveBeginNext(uint32_t nowMs) {
    auto* ble = hal::getBluetoothControllerInstance();
    while (s_resolveIndex < s_bleScanCount && !bleDeviceNeedsName(s_resolveIndex)) {
        s_resolveIndex++;
    }
    if (!ble || s_resolveIndex >= s_bleScanCount) {
        LOG_I(TAG, "Name resolve done");
        bleResolveStop();
        return;
    }
    bleResolveClearEvents();
    s_resolveConn = 0xFFFF;
    s_resolveNameHandle = 0;
    const auto& dev = s_bleScanResults[s_resolveIndex];
    LOG_I(TAG, "Resolving [%u] %02X:%02X:%02X:%02X:%02X:%02X (addrType %u)",
          s_resolveIndex, dev.mac[5], dev.mac[4], dev.mac[3],
          dev.mac[2], dev.mac[1], dev.mac[0], dev.addrType);
    if (!ble->connect(dev.mac, dev.addrType)) {
        LOG_W(TAG, "connect() rejected for index %u", s_resolveIndex);
        s_resolveIndex++;
        bleResolveBeginNext(nowMs);  // bounded by scan count
        return;
    }
    s_resolvePhase = BleResolvePhase::Connecting;
    s_resolveDeviceStartMs = nowMs;
}

static void bleResolveStart() {
    if (s_resolveActive) return;
    auto* ble = hal::getBluetoothControllerInstance();
    if (!ble || !ble->isEnabled() || s_bleScanCount == 0) {
        LOG_W(TAG, "Name resolve not started (ble=%d enabled=%d count=%u)",
              ble != nullptr, ble && ble->isEnabled(), s_bleScanCount);
        return;
    }

    uint8_t need = 0;
    for (uint8_t i = 0; i < s_bleScanCount; i++) {
        if (bleDeviceNeedsName(i)) need++;
    }
    LOG_I(TAG, "Name resolve start: %u of %u device(s) need a name", need, s_bleScanCount);

    s_tokResConn = ble->addConnectionCallback(rOnConnect);
    s_tokResDisc = ble->addDisconnectionCallback(rOnDisconnect);
    s_tokResSvc  = ble->addServiceDiscoveryCallback(rOnServiceDiscovered);
    s_tokResRead = ble->addCharacteristicReadCallback(rOnCharRead);

    s_resolveActive     = true;
    s_resolveIndex      = 0;
    s_resolveConn       = 0xFFFF;
    s_resolveNameHandle = 0;
    bleResolveClearEvents();
    s_resolvePhase = BleResolvePhase::Begin;
}

static void bleResolveStop() {
    auto* ble = hal::getBluetoothControllerInstance();
    if (ble) {
        ble->cancelConnect();
        if (s_resolveConn != 0xFFFF) ble->disconnectHandle(s_resolveConn);
        if (s_tokResConn != kInvalidListener) ble->removeConnectionCallback(s_tokResConn);
        if (s_tokResDisc != kInvalidListener) ble->removeDisconnectionCallback(s_tokResDisc);
        if (s_tokResSvc  != kInvalidListener) ble->removeServiceDiscoveryCallback(s_tokResSvc);
        if (s_tokResRead != kInvalidListener) ble->removeCharacteristicReadCallback(s_tokResRead);
    }
    s_tokResConn = s_tokResDisc = s_tokResSvc = s_tokResRead = kInvalidListener;
    s_resolveActive = false;
    s_resolvePhase  = BleResolvePhase::Idle;
    s_resolveConn   = 0xFFFF;
    s_resolveNameHandle = 0;
}

static void bleResolveTick(uint32_t nowMs) {
    if (!s_resolveActive) return;
    auto* ble = hal::getBluetoothControllerInstance();
    if (!ble) { bleResolveStop(); return; }

    const bool deadline = (nowMs - s_resolveDeviceStartMs) > BLE_RESOLVE_DEVICE_TIMEOUT_MS;

    switch (s_resolvePhase) {
    case BleResolvePhase::Begin:
        bleResolveBeginNext(nowMs);
        break;

    case BleResolvePhase::Connecting:
        if (s_evtConnected) {
            s_resolveConn  = s_evtConnHandle;
            s_evtConnected = false;
            LOG_I(TAG, "Connected (handle=%u), discovering Generic Access", s_resolveConn);
            if (ble->discoverServiceByUuid(s_resolveConn, hal::BleUuid::from16(BLE_GAP_SVC_UUID))) {
                s_resolvePhase = BleResolvePhase::Discovering;
            } else {
                LOG_W(TAG, "discoverServiceByUuid failed");
                bleResolveCleanupToSettle(nowMs);
            }
        } else if (deadline) {
            LOG_W(TAG, "Connect timeout for index %u", s_resolveIndex);
            bleResolveCleanupToSettle(nowMs);
        }
        break;

    case BleResolvePhase::Discovering:
        if (s_evtDiscovered) {
            s_evtDiscovered = false;
            if (s_evtNameHandle != 0 &&
                ble->readCharacteristic(s_resolveConn, s_evtNameHandle)) {
                s_resolveNameHandle = s_evtNameHandle;
                LOG_I(TAG, "Reading Device Name (handle=%u)", s_resolveNameHandle);
                s_resolvePhase = BleResolvePhase::Reading;
            } else {
                LOG_W(TAG, "No Device Name characteristic (handle=%u)", s_evtNameHandle);
                bleResolveCleanupToSettle(nowMs);
            }
        } else if (deadline) {
            LOG_W(TAG, "Discovery timeout for index %u", s_resolveIndex);
            bleResolveCleanupToSettle(nowMs);
        }
        break;

    case BleResolvePhase::Reading:
        if (s_evtGotName) {
            s_evtGotName = false;
            if (s_resolveNameBuf[0] != '\0' && s_bleScanView) {
                char* dst = s_bleScanResults[s_resolveIndex].name;
                size_t cap = sizeof(s_bleScanResults[s_resolveIndex].name);
                strncpy(dst, s_resolveNameBuf, cap - 1);
                dst[cap - 1] = '\0';
                LOG_I(TAG, "Resolved [%u] name '%s'", s_resolveIndex, dst);
                s_bleScanView->updateItem(s_resolveIndex);
            } else {
                LOG_W(TAG, "Empty Device Name for index %u", s_resolveIndex);
            }
            bleResolveCleanupToSettle(nowMs);
        } else if (deadline) {
            LOG_W(TAG, "Read timeout for index %u", s_resolveIndex);
            bleResolveCleanupToSettle(nowMs);
        }
        break;

    case BleResolvePhase::Settle:
        if (s_evtDisconnected ||
            (nowMs - s_resolveSettleStartMs) > BLE_RESOLVE_SETTLE_TIMEOUT_MS) {
            s_resolveIndex++;
            bleResolveBeginNext(nowMs);
        }
        break;

    case BleResolvePhase::Idle:
    default:
        break;
    }
}

/**
 * \brief ListView for the BLE scan results that drives async name resolution
 *        while visible and cancels it when the view closes.
 */
class BleScanView : public ListView {
public:
    void onEnter(void* context) override { ListView::onEnter(context); bleResolveStart(); }
    void onResume() override { ListView::onResume(); bleResolveStart(); }
    void onExit() override { bleResolveStop(); ListView::onExit(); }
    void onTick(uint32_t nowMs) override { bleResolveTick(nowMs); }
    const char* getName() const override { return "BleScanView"; }
};

/**
 * \brief Runs BLE device scan and displays results in a list view.
 */
static void startBluetoothScan() {
    auto* ble = hal::getBluetoothControllerInstance();
    if (!ble || !ble->isEnabled()) {
        showToastError(ui::tr("core.hw_not_available"));
        return;
    }

    showToastInfo(ui::tr("core.ble_scanning"), 0);

    s_bleScanCount = 0;
    if (ble->startScan(BLE_SCAN_TIMEOUT_MS)) {
        uint32_t startMs = esp_timer_get_time() / 1000;
        while (!ble->isScanComplete()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if ((esp_timer_get_time() / 1000 - startMs) > BLE_SCAN_TIMEOUT_MS + 1000) break;
        }

        s_bleScanCount = ble->getScanResults(s_bleScanResults, BLE_MAX_SCAN_RESULTS);
    }

    sortBleScanResults();
    ViewStack::instance().hideModal();

    LOG_I(TAG, "BLE scan complete: %u device(s) found", s_bleScanCount);

    if (s_bleScanCount == 0) {
        showToastInfo(ui::tr("core.ble_no_devices"));
        return;
    }

    if (!s_bleScanView) {
        s_bleScanView = new BleScanView();
        s_bleScanView->setItemRenderer(renderBleRow, nullptr);
    }

    for (uint8_t i = 0; i < s_bleScanCount; i++) {
        s_bleScanItems[i] = {s_bleScanResults[i].name, 0, false, &s_bleScanResults[i]};
    }

    // Persisted: ListView::init() stores the title pointer, it must not be a
    // stack buffer that dangles after this function returns.
    static char title[32];
    snprintf(title, sizeof(title), "%s (%d)", ui::tr("core.ble_scan"), s_bleScanCount);
    s_bleScanView->init(title, s_bleScanItems, s_bleScanCount);
    ViewStack::instance().push(s_bleScanView);
}

} // namespace cdc::ui
