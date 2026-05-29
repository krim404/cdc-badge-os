/**
 * \file
 * \brief Bluetooth UI flow for toggling BLE, status display, and device scanning.
 */

#include "AppUiInternal.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_views/RenderHelpers.h"

#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

namespace cdc::ui {

/** \brief Bluetooth fixed menu indices and capacity limits. */

enum BluetoothMenuIdx {
    BT_IDX_ENABLE = 0,
    BT_IDX_STATUS,
    BT_IDX_SCAN,
    BT_IDX_FORGET_BONDS,
    BT_IDX_FIXED_COUNT
};

static constexpr uint8_t BT_MENU_MAX_ITEMS = 16;
static constexpr uint8_t BLE_MAX_SCAN_RESULTS = 16;
static constexpr uint32_t BLE_SCAN_TIMEOUT_MS = 8000;

/** \brief Bluetooth menu and scan state. */

static ListView* s_bluetoothMenu = nullptr;
static ListItem s_bluetoothItems[BT_MENU_MAX_ITEMS];
static core::ModuleMenuItem s_bluetoothModuleItems[12];
static uint8_t s_bluetoothModuleCount = 0;

/** \brief BLE scan result list state. */
static ListView* s_bleScanView = nullptr;
static ListItem s_bleScanItems[BLE_MAX_SCAN_RESULTS];
static hal::BleScanResult s_bleScanResults[BLE_MAX_SCAN_RESULTS];
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

    if (s_bleScanCount == 0) {
        showToastInfo(ui::tr("core.ble_no_devices"));
        return;
    }

    if (!s_bleScanView) {
        s_bleScanView = new ListView();
        s_bleScanView->setItemRenderer(renderBleRow, nullptr);
    }

    for (uint8_t i = 0; i < s_bleScanCount; i++) {
        s_bleScanItems[i] = {s_bleScanResults[i].name, 0, false, &s_bleScanResults[i]};
    }

    char title[32];
    snprintf(title, sizeof(title), "%s (%d)", ui::tr("core.ble_scan"), s_bleScanCount);
    s_bleScanView->init(title, s_bleScanItems, s_bleScanCount);
    ViewStack::instance().push(s_bleScanView);
}

} // namespace cdc::ui
