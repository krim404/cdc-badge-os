/**
 * \file
 * \brief Expert UI menus including module control and TROPIC maintenance actions.
 */

#include "AppUiInternal.h"
#include "cdc_os_ui/AppUi.h"
#include "cdc_os_ui/HardwareInfo.h"
#include "cdc_core/TropicStorage.h"
#include "cdc_core/UsbManager.h"
#include "cdc_core/EventBus.h"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"

#include <cstdio>
#include <cstring>

namespace cdc::ui {

/** \brief Expert menu sizing constants. */

static constexpr uint8_t EXPERT_MAX_ITEMS = 12;
static constexpr uint8_t MODULES_VIEW_MAX = 16;

// Forward declarations so the fixed-entry tables can name their handlers
// (defined later in this file; showModulesView comes from AppUiInternal.h).
static void runSystemTest();
static void runTropicCacheRebuild();
static void runTropicCacheCleanup();
void rebootIntoBootloader();

/// Fixed expert entries above (top) and below (bottom) the dynamic module
/// entries. The counts derive from the table sizes via sizeof, so adding or
/// removing a row needs no separate counter update.
struct FixedExpertEntry { const char* key; void (*action)(); };
static const FixedExpertEntry kExpertTop[] = {
    {"core.hardware_info",  runSystemTest},
    {"core.modules",        showModulesView},
    {"core.backup",         showBackupMenu},
    {"core.set_duress_pin", showDuressPinSetup},
};
static const FixedExpertEntry kExpertBottom[] = {
    {"core.tr01_cache_rebuild", runTropicCacheRebuild},
    {"core.tr01_cache_cleanup", runTropicCacheCleanup},
    {"core.bootloader",         rebootIntoBootloader},
};
static constexpr uint8_t EXPERT_TOP_COUNT    = sizeof(kExpertTop) / sizeof(kExpertTop[0]);
static constexpr uint8_t EXPERT_BOTTOM_COUNT = sizeof(kExpertBottom) / sizeof(kExpertBottom[0]);
static constexpr uint8_t EXPERT_FIXED_COUNT  = EXPERT_TOP_COUNT + EXPERT_BOTTOM_COUNT;

/** \brief Static view pointers and menu item storage for expert/module views. */

static ListView* s_expertMenu = nullptr;
static ListItem s_expertItems[EXPERT_MAX_ITEMS];
static core::ModuleMenuItem s_expertModuleItems[EXPERT_MAX_ITEMS - EXPERT_FIXED_COUNT];
static uint8_t s_expertModuleCount = 0;

static ListView* s_modulesView = nullptr;
static ListItem s_modulesItems[MODULES_VIEW_MAX];
static char s_moduleLabels[MODULES_VIEW_MAX][48];

/** \brief Rebuilds module status list view content. */
static void rebuildModulesView();

/**
 * \brief Retries failed module initialization after user confirmation.
 * \param userData Encoded module index.
 */
static void onModuleRetryConfirm(void* userData) {
    uint8_t index = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(userData));
    auto& moduleReg = core::ModuleRegistry::instance();

    if (moduleReg.retryModule(index)) {
        showToastSuccess("OK", TOAST_DURATION_SHORT_MS);
    } else {
        const char* error = moduleReg.getModuleSlotError(index);
        showToastError(error ? error : ui::tr("core.failed"), TOAST_DURATION_MEDIUM_MS);
    }

    ui_rebuild_menus();
    rebuildModulesView();
}

/**
 * \brief Handles module list selection for retry/toggle behavior.
 * \param index Selected module index.
 * \param userData Optional callback user data.
 */
static void onModuleSelect(uint16_t index, void* userData) {
    (void)userData;

    auto& moduleReg = core::ModuleRegistry::instance();
    if (index >= moduleReg.getModuleCount()) return;

    core::IModule* module = moduleReg.getModuleAt(index);
    if (!module) return;

    uint8_t idx = static_cast<uint8_t>(index);

    // If module has error, show retry dialog
    if (moduleReg.hasModuleSlotError(idx)) {
        const char* error = moduleReg.getModuleSlotError(idx);

        static char confirmMsg[128];
        snprintf(confirmMsg, sizeof(confirmMsg), "%s\n%s",
                 error ? error : ui::tr("core.module_error_generic"),
                 ui::tr("core.module_retry_prompt"));

        showConfirm(confirmMsg, onModuleRetryConfirm, nullptr,
                    ConfirmView::Icon::ERROR, reinterpret_cast<void*>(static_cast<uintptr_t>(idx)));
        return;
    }

    // Remember USB state before toggle
    bool needsReplugBefore = core::UsbManager::instance().needsReplug();

    // Normal toggle: enable/disable module
    bool nowEnabled = moduleReg.toggleModuleEnabled(idx);

    if (nowEnabled) {
        if (!moduleReg.startModule(idx)) {
            switch (moduleReg.classifyStartFailure(idx)) {
                case core::ModuleStartFailure::SlotError:
                    showToastError(moduleReg.getModuleSlotError(idx), TOAST_DURATION_MEDIUM_MS);
                    break;
                case core::ModuleStartFailure::UsbBudgetFull:
                    showToastError(ui::tr("core.usb_no_free_slot"), TOAST_DURATION_MEDIUM_MS);
                    break;
                case core::ModuleStartFailure::Generic:
                    showToastError(ui::tr("core.failed"), TOAST_DURATION_MEDIUM_MS);
                    break;
            }
        }
    } else {
        if (module->getState() == core::ServiceState::STARTED) {
            module->stop();
        }
    }

    // If USB config changed by THIS module toggle, show sticky alert
    if (core::UsbManager::instance().newlyRequiresReplug(needsReplugBefore)) {
        showToastAlertSticky(ui::tr("core.usb_replug_required"));
    }

    ui_rebuild_menus();
    rebuildModulesView();
}

/**
 * \brief Rebuilds module list rows with current enabled/state/error markers.
 */
static void rebuildModulesView() {
    auto& moduleReg = core::ModuleRegistry::instance();
    uint8_t count = moduleReg.getModuleCount();

    if (count == 0) {
        s_modulesItems[0] = {"(none)", 0, false, nullptr};
        count = 1;
    } else {
        for (uint8_t i = 0; i < count && i < MODULES_VIEW_MAX; i++) {
            core::IModule* module = moduleReg.getModuleAt(i);
            if (module) {
                snprintf(s_moduleLabels[i], sizeof(s_moduleLabels[i]),
                         "%s %s", module->getName(),
                         moduleReg.getModuleStatusLabel(i));
                s_modulesItems[i] = {s_moduleLabels[i], 0, false, nullptr};
            }
        }
        if (count > MODULES_VIEW_MAX) count = MODULES_VIEW_MAX;
    }

    if (s_modulesView) {
        s_modulesView->init(ui::tr("core.modules"), s_modulesItems, count);
    }
}

/**
 * \brief Shows module management list view.
 */
void showModulesView() {
    if (!s_modulesView) {
        s_modulesView = new ListView();
    }

    rebuildModulesView();
    s_modulesView->setOnSelect(onModuleSelect);
    ViewStack::instance().push(s_modulesView);
}

/**
 * \brief Opens hardware information/system-test screen.
 */
static void runSystemTest() {
    showHardwareInfo();
}

/**
 * \brief Rebuilds cached TROPIC metadata and reports operation result.
 */
static void runTropicCacheRebuild() {
    showToastTask(ui::tr("core.task_working"), 0);
    bool ok = core::TropicStorage::instance().rebuild();
    ViewStack::instance().hideModal();
    if (ok) {
        showToastSuccess(ui::tr("core.ok"));
    } else {
        showToastError(ui::tr("core.failed"));
    }
}

/**
 * \brief Cleans cached TROPIC metadata and reports operation result.
 */
static void runTropicCacheCleanup() {
    showToastTask(ui::tr("core.task_working"), 0);
    bool ok = core::TropicStorage::instance().cleanup();
    ViewStack::instance().hideModal();
    if (ok) {
        showToastSuccess(ui::tr("core.ok"));
    } else {
        showToastError(ui::tr("core.failed"));
    }
}

/** \brief Rebuilds expert menu item list including module-provided entries. */
static void rebuildExpertMenu();
/** \brief Handles expert menu selection actions. */
static void onExpertMenuSelect(uint16_t index, void* userData);

/**
 * \brief Shows expert menu and initial warning toast.
 */
void showExpertMenu() {
    // Duration 0: stays until the user dismisses it with Y/N, so the dismiss
    // key is consumed by the modal and never selects the first list entry.
    showToastInfo(ui::tr("core.expert_warning"), 0);
    if (!s_expertMenu) {
        s_expertMenu = new ListView();
        s_expertMenu->setOnSelect(onExpertMenuSelect);
    }

    rebuildExpertMenu();
    ViewStack::instance().push(s_expertMenu);
}

/**
 * \brief Rebuilds expert menu entries including dynamically provided items.
 */
static void rebuildExpertMenu() {
    auto& moduleReg = core::ModuleRegistry::instance();
    uint8_t n = 0;

    // Top fixed entries (system test, modules), then the dynamic module
    // entries (e.g. vFAT), then the bottom fixed entries (TROPIC, bootloader).
    for (uint8_t i = 0; i < EXPERT_TOP_COUNT; i++)
        s_expertItems[n++] = {ui::tr(kExpertTop[i].key), 0, false, nullptr};

    uint8_t rawModules = moduleReg.getMenuItems(
        core::MenuLocation::EXPERT_MENU,
        s_expertModuleItems,
        EXPERT_MAX_ITEMS - EXPERT_FIXED_COUNT
    );
    uint8_t visible = 0;
    for (uint8_t i = 0; i < rawModules; i++) {
        const auto& item = s_expertModuleItems[i];
        if (item.isVisible && !item.isVisible()) continue;
        s_expertModuleItems[visible] = item;  // compact visible entries
        s_expertItems[n++] = {item.label, 0, false, nullptr};
        visible++;
    }
    s_expertModuleCount = visible;

    for (uint8_t i = 0; i < EXPERT_BOTTOM_COUNT; i++)
        s_expertItems[n++] = {ui::tr(kExpertBottom[i].key), 0, false, nullptr};

    s_expertMenu->init(ui::tr("core.expert"), s_expertItems, n);
}

/**
 * \brief Worker that detaches USB, arms the download-boot bit and
 *        triggers a hard system reset.
 *
 * - `tud_disconnect()` first so the host sees a USB disconnect and will
 *   re-enumerate after the reset; without this, the CDC endpoint stays
 *   "connected but unresponsive" until the cable is unplugged manually.
 * - `esp_rom_software_reset_system()` instead of `esp_restart()` because
 *   shutdown handlers can hang in the active state (USB CDC enumerated,
 *   BLE/HID running, plugins ticking).
 * - `RTC_CNTL_FORCE_DOWNLOAD_BOOT` survives the soft reset, so the ROM
 *   bootloader enters USB download mode on the next boot.
 */
[[noreturn]] static void bootloaderResetTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(200));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
    while (true) { vTaskDelay(portMAX_DELAY); }
}

/**
 * \brief Reboots into USB download mode.
 *
 * Forces the UI to the lock screen with a "BOOTLOADER MODE" banner,
 * flushes the EPD and switches the backlight off (the lock-screen path is
 * the known-quiet rendering state), then spawns a dedicated worker that
 * detaches USB and triggers the hard reset. The caller returns
 * immediately so any locks it holds are released before reset proceeds.
 */
void rebootIntoBootloader() {
    prepareForBootloaderReset();

    xTaskCreate(bootloaderResetTask, "btldr_reset", 4096, nullptr,
                configMAX_PRIORITIES - 1, nullptr);
}

/**
 * \brief Handles selected expert-menu action.
 * \param index Selected menu item index.
 * \param userData Optional callback user data.
 */
static void onExpertMenuSelect(uint16_t index, void* userData) {
    (void)userData;

    if (index < EXPERT_TOP_COUNT) {
        kExpertTop[index].action();
        return;
    }

    uint16_t modIdx = index - EXPERT_TOP_COUNT;
    if (modIdx < s_expertModuleCount) {
        const auto& item = s_expertModuleItems[modIdx];
        if (item.getView) {
            IView* view = item.getView();
            if (view) {
                ViewStack::instance().push(view);
            }
        }
        return;
    }

    uint16_t botIdx = modIdx - s_expertModuleCount;
    if (botIdx < EXPERT_BOTTOM_COUNT) {
        kExpertBottom[botIdx].action();
    }
}

/**
 * \brief Displays toast notification for module error events.
 * \param evt Event payload from module registry.
 */
void onModuleErrorEvent(const core::Event& evt) {
    if (evt.type != core::EventType::MODULE_ERROR) return;

    auto& moduleReg = core::ModuleRegistry::instance();
    uint8_t index = static_cast<uint8_t>(evt.data.value);

    if (index >= moduleReg.getModuleCount()) return;

    const char* error = moduleReg.getModuleSlotError(index);
    core::IModule* module = moduleReg.getModuleAt(index);
    const char* name = module ? module->getName() : "?";

    static char errMsg[96];
    snprintf(errMsg, sizeof(errMsg), "%s: %s", name, error ? error : "Fehler");

    showToastError(errMsg, TOAST_DURATION_LONG_MS);
}

} // namespace cdc::ui
