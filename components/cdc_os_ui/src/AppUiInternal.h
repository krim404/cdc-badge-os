#pragma once

/**
 * Internal header shared between AppUi sub-files.
 * Not part of the public API - only used by AppUi*.cpp and *MenuUi.cpp files.
 */

#include "cdc_ui/ViewStack.h"
#include "cdc_ui/I18n.h"
#include "cdc_views/ListView.h"
#include "cdc_views/ToastView.h"
#include "cdc_views/InfoView.h"
#include "cdc_views/ConfirmView.h"
#include "cdc_views/T9InputView.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/EventBus.h"

#include <cstdint>
#include <goodisplay/gdey029T94.h>
#include <Fonts/FreeMonoBold9pt7b.h>

namespace cdc::ui {

// ============================================================================
// Shared Constants
// ============================================================================

static constexpr uint32_t TOAST_DURATION_SHORT_MS = 1000;
static constexpr uint32_t TOAST_DURATION_MEDIUM_MS = 1500;
static constexpr uint32_t TOAST_DURATION_LONG_MS = 2500;

// ============================================================================
// Shared Drawing Helpers
// ============================================================================

/**
 * Draw RSSI signal strength bars (used by WiFi and BLE scan views)
 */
void drawSignalBars(Gdey029T94* gfx, int x, int y, int8_t rssi, bool inverted);

// ============================================================================
// Cross-file Menu Rebuild Functions
// ============================================================================

// Called by feature files when they need to trigger menu rebuilds
void rebuildMainMenu();
void rebuildToolsMenu();

/// \brief True if the lock screen is the only view on the stack (badge locked).
bool isBadgeLocked();

/**
 * \brief Wake the screen and run the PIN-unlock flow for a transfer accepted on
 *        the lock screen, then invoke `onUnlocked(userData)` on success.
 *
 * Lets a feature surface its own request prompt over the lock screen and, once
 * the user accepts, gate the action behind the badge PIN (mirrors FIDO2). The
 * callback runs only after a successful unlock; cancel/lockout keeps the badge
 * locked and never calls it.
 */
void requestUnlockForTransfer(void (*onUnlocked)(void*), void* userData);

// ============================================================================
// Feature Sub-file Entry Points
// ============================================================================

// WiFi (WifiMenuUi.cpp)
void showWifiMainMenu();
void rebuildWifiMainMenu();

// Bluetooth (BluetoothMenuUi.cpp)
void showBluetoothMenu();
void rebuildBluetoothMenu();

// Message transfer (MsgTransferUi.cpp)
void msgTransferUiInit();
void msgTransferUiProcess(uint32_t nowMs);
void showBeaconMenu();      // complete beacon flow (toggle, name, scan) under Tools
void showMsgBeaconScan();   // read-only beacon scan, also reachable from the BT menu

// Expert & Modules (ExpertMenuUi.cpp)
void showExpertMenu();
void showModulesView();
void onModuleErrorEvent(const core::Event& evt);

// USB services (UsbServicesUi.cpp)
void showUsbServicesMenu();

// Backup (BackupMenuUi.cpp)
void showBackupMenu();
void registerBackupSerialCommand();

// Duress / self-destruct PIN setup (AppUi.cpp, owns the PinChangeView instance)
void showDuressPinSetup();

/**
 * \brief Forces the badge into a quiet state ahead of the bootloader reset:
 *        pops every view back to the lock screen, replaces the lock-screen
 *        text with "BOOTLOADER MODE" / reset-button hint and triggers a full
 *        EPD refresh + backlight off.
 *
 * Provided by AppUi.cpp because it touches the static lock-screen instance.
 */
void prepareForBootloaderReset();

} // namespace cdc::ui
