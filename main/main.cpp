/**
 * CDC Badge OS
 * USB CDC + Serial + Display + UI
 */

#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "cdc_log.h"

#include "cdc_core/ServiceRegistry.h"
#include "plugin_manager/PluginManager.h"
#include "plugin_manager/PluginSerialCommands.h"
#include "plugin_manager/GpioSerialCommands.h"
#include "cdc_ui/I18n.h"
#include "cdc_core/EventBus.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/SystemLock.h"
#include "cdc_core/TropicStorage.h"
#include "cdc_core/AttestationKeyService.h"
#include "cdc_core/feature_flags.h"
#include "cdc_core/PinManager.h"
#include "cdc_core/FactoryReset.h"
#include "modules_init.gen.h"  // Auto-generated module registrations
#include "usb_badge/usb_cdc.h"
#include "serial_cmd/SerialCmd.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_hal/II2cBus.h"
#include "cdc_hal/IKeypad.h"
#include "cdc_hal/IPowerManager.h"
#include "cdc_hal/ISecureElement.h"
#include "cdc_hal/ISleepController.h"
#include "cdc_hal/IWifiController.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_hal/hw_config.h"
#include "cdc_hal/IRtc.h"
#include "cdc_os_ui/AppUi.h"
#include "cdc_os_ui/WifiHandlers.h"
#include "cdc_msg/MessageTransfer.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char* TAG = "BOOT";

using namespace cdc::core;

/** \brief Cached HAL/service singleton pointers used during boot sequence. */
static cdc::hal::II2cBus* s_i2cBus = nullptr;
static cdc::hal::IKeypad* s_keypad = nullptr;
static cdc::hal::IPowerManager* s_powerManager = nullptr;
static cdc::hal::ISecureElement* s_secureElement = nullptr;
static cdc::hal::ISleepController* s_sleepController = nullptr;
static cdc::hal::IDisplay* s_display = nullptr;
static cdc::core::AttestationKeyService s_attestationService;
static esp_sleep_wakeup_cause_t s_wakeupCause = ESP_SLEEP_WAKEUP_UNDEFINED;

/**
 * \brief Draws a modal-style system-lockdown halt screen before deep sleep.
 * \param reason Reason captured by \ref SystemLock.
 * \param detail Optional detail string captured by \ref SystemLock.
 */
static void lockdownShutdownHandler(cdc::core::LockdownReason reason,
                                    const char* detail) {
    const char* reasonText;
    switch (reason) {
        case cdc::core::LockdownReason::TR01_ALARM_MODE:
            reasonText = "Secure element alarm"; break;
        case cdc::core::LockdownReason::TR01_UNREACHABLE:
            reasonText = "Secure element offline"; break;
        case cdc::core::LockdownReason::TR01_INIT_FAILED:
            reasonText = "Secure element init failed"; break;
        case cdc::core::LockdownReason::NVS_UNREADABLE:
            reasonText = "Non-volatile storage unreadable"; break;
        default:
            reasonText = "Unknown failure"; break;
    }

    LOG_E(TAG, "SYSTEM LOCKDOWN: %s%s%s", reasonText,
          detail ? " - " : "", detail ? detail : "");

    if (!s_display) {
        return;
    }

    const int16_t w = static_cast<int16_t>(s_display->getWidth());
    const int16_t h = static_cast<int16_t>(s_display->getHeight());
    constexpr int16_t kMargin = 8;
    const int16_t modalX = kMargin;
    const int16_t modalY = kMargin;
    const int16_t modalW = static_cast<int16_t>(w - 2 * kMargin);
    const int16_t modalH = static_cast<int16_t>(h - 2 * kMargin);

    s_display->clear();
    s_display->fillRect(modalX, modalY, modalW, modalH, 0xFFFF);
    s_display->drawRect(modalX, modalY, modalW, modalH, 0x0000);
    s_display->drawRect(modalX + 1, modalY + 1,
                        static_cast<int16_t>(modalW - 2),
                        static_cast<int16_t>(modalH - 2), 0x0000);

    constexpr int16_t kHeaderHeight = 18;
    s_display->fillRect(static_cast<int16_t>(modalX + 2),
                        static_cast<int16_t>(modalY + 2),
                        static_cast<int16_t>(modalW - 4),
                        kHeaderHeight, 0x0000);
    s_display->setTextColor(0xFFFF);
    s_display->setTextSize(2);
    s_display->setCursor(static_cast<int16_t>(modalX + 12),
                         static_cast<int16_t>(modalY + 4));
    s_display->print("SYSTEM LOCKED");

    s_display->setTextColor(0x0000);
    s_display->setTextSize(1);

    int16_t cursorY = static_cast<int16_t>(modalY + kHeaderHeight + 12);
    s_display->setCursor(static_cast<int16_t>(modalX + 10), cursorY);
    s_display->print(reasonText);

    if (detail && *detail) {
        cursorY = static_cast<int16_t>(cursorY + 14);
        s_display->setCursor(static_cast<int16_t>(modalX + 10), cursorY);
        s_display->printf("Detail: %s", detail);
    }

    s_display->setCursor(static_cast<int16_t>(modalX + 10),
                         static_cast<int16_t>(modalY + modalH - 14));
    s_display->print("Power cycle to recover");

    s_display->flushSync(cdc::hal::RefreshMode::FULL);
}

/**
 * \brief Stage 0: initializes NVS flash storage with automatic erase on
 *        version-mismatch or out-of-pages errors.
 * \return `true` on success.
 */
static bool initNvs() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    return ret == ESP_OK;
}

/**
 * \brief Persists the current build-profile byte, marking the factory reset
 *        complete.
 *
 * Must be called only after both NVS and TROPIC01 have been wiped, so that an
 * interrupted reset (reset/power loss, or an unavailable SE session) re-runs
 * cleanly on the next boot instead of leaving stale TROPIC01 state behind.
 */
static void seedBuildProfile() {
    nvs_handle_t handle = 0;
    if (nvs_open(kBootProfileNs, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, kBootProfileKey, BUILD_PROFILE_BYTE);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

/**
 * \brief Compares the persisted build profile byte against the compiled-in
 *        value and triggers a NVS wipe on mismatch.
 *
 * \return `true` when a wipe occurred and the TROPIC01 should be wiped too
 *         once its session is available.
 */
static bool checkBuildProfileAndWipeNvs() {
    constexpr uint8_t expected = BUILD_PROFILE_BYTE;
    constexpr uint8_t kMaxValid = 0x03;

    nvs_handle_t handle = 0;
    uint8_t stored = 0;
    esp_err_t openErr = nvs_open(kBootProfileNs, NVS_READONLY, &handle);
    esp_err_t readErr = ESP_ERR_NVS_NOT_FOUND;
    if (openErr == ESP_OK) {
        readErr = nvs_get_u8(handle, kBootProfileKey, &stored);
        nvs_close(handle);
    }

    // Treat first-boot (namespace or key absent) and a malformed value as a
    // reason to wipe and re-seed the byte. A structural NVS error (anything
    // other than ESP_OK / NOT_FOUND / TYPE_MISMATCH) is a hardware-level
    // failure and must trigger lockdown rather than a destructive wipe.
    bool structuralFault = false;
    if (openErr != ESP_OK && openErr != ESP_ERR_NVS_NOT_FOUND) {
        structuralFault = true;
    }
    if (openErr == ESP_OK
        && readErr != ESP_OK
        && readErr != ESP_ERR_NVS_NOT_FOUND
        && readErr != ESP_ERR_NVS_TYPE_MISMATCH) {
        structuralFault = true;
    }

    if (structuralFault) {
        ESP_LOGE(TAG, "NVS unreadable (open=0x%X read=0x%X), entering lockdown", openErr, readErr);
        cdc::core::SystemLock::instance().triggerLockdown(
            cdc::core::LockdownReason::NVS_UNREADABLE, "boot_profile read failed");
        return false;
    }

    bool present = (openErr == ESP_OK && readErr == ESP_OK);
    bool valid   = present && stored <= kMaxValid;
    if (valid && stored == expected) {
        return false;
    }

    if (!present) {
        ESP_LOGW(TAG, "Build profile absent, factory reset and seed 0x%02X", expected);
    } else if (!valid) {
        ESP_LOGW(TAG, "Build profile malformed (0x%02X), factory reset and seed 0x%02X", stored, expected);
    } else {
        ESP_LOGW(TAG, "Build profile changed (0x%02X -> 0x%02X), factory reset", stored, expected);
    }
    ESP_ERROR_CHECK(cdc::core::wipeNvs());

    // The profile byte is seeded only after the TROPIC01 wipe completes (see
    // seedBuildProfile / wipeTropicForFactoryReset), so an interrupted reset
    // re-runs on the next boot instead of being marked done prematurely.
    return true;
}

/**
 * \brief Wipes all TROPIC01 R-Memory and ECC slots used by application code,
 *        then seeds the build-profile byte to mark the factory reset complete.
 *
 * Called after a build-profile change has already wiped NVS. The profile byte
 * is only persisted on a successful wipe; if the SE session is unavailable the
 * reset stays pending and re-runs on the next boot.
 */
static void wipeTropicForFactoryReset() {
    LOG_W(TAG, "Build profile change: wiping TROPIC01 ECC and R-Memory slots");
    auto result = cdc::core::wipeTropic(cdc::hal::getSecureElementInstance());
    if (!result.sessionReady) {
        LOG_E(TAG, "TROPIC01 factory wipe skipped (SE session unavailable), reset stays pending");
        return;
    }
    LOG_W(TAG, "TROPIC01 factory wipe: deleted %u ECC keys, %u R-Memory slots",
          result.eccDeleted, result.rmemDeleted);
    seedBuildProfile();
}

/**
 * \brief Stage 1: brings up event bus, USB CDC and the logging subsystem.
 * \return `true` if all core services initialized successfully.
 */
static bool initCoreServices() {
    if (!EventBus::instance().init()) {
        return false;  // Cannot log yet, USB not ready
    }

    if (!usb_cdc_init()) {
        return false;  // USB failed, no output channel available
    }

    log_init();

    LOG_I(TAG, "=================================");
    LOG_I(TAG, "CDC Badge OS v" APP_VERSION);
    LOG_I(TAG, "Open Hardware Security");
    LOG_I(TAG, "=================================");

    LOG_I(TAG, "EventBus ready");
    LOG_I(TAG, "USB CDC ready");
    LOG_I(TAG, "ServiceRegistry ready (capacity: %u)", ServiceRegistry::MAX_SERVICES);
    return true;
}

/**
 * \brief Initializes the RTC and warns when the system time has not been set.
 */
static void initRtc() {
    cdc::hal::IRtc* rtc = cdc::hal::getRtcInstance();
    if (rtc) {
        rtc->init();
    }
    if (!rtc || !rtc->isTimeSet()) {
        LOG_W(TAG, "System time not set");
    }
}

/**
 * \brief Caches the wakeup cause and releases RTC GPIO when resuming from deep
 *        sleep so that subsequent I2C bus init can re-claim the IRQ pin.
 */
static void handleWakeupAndReleaseRtcGpio() {
    s_wakeupCause = esp_sleep_get_wakeup_cause();
    if (s_wakeupCause == ESP_SLEEP_WAKEUP_EXT1) {
        LOG_D(TAG, "Woke from deep sleep, deinit RTC GPIO");
        rtc_gpio_deinit(EXP_IRQ_PIN);
    }
}

/**
 * \brief Brings up the I2C bus shared by the on-board peripherals.
 */
static void initI2cBus() {
    LOG_I(TAG, "Initializing I2C bus...");
    s_i2cBus = cdc::hal::getI2cBus0();
    if (s_i2cBus && s_i2cBus->init()) {
        LOG_I(TAG, "I2C bus ready");
    } else {
        LOG_E(TAG, "I2C bus init failed!");
    }
}

/**
 * \brief Initializes the BQ25895 power manager and reports current battery
 *        state to the log.
 */
static void initPowerManager() {
    LOG_I(TAG, "Initializing Power Management...");
    s_powerManager = cdc::hal::getPowerManagerInstance();
    if (s_powerManager && s_powerManager->init() && s_powerManager->start()) {
        LOG_I(TAG, "Power Management ready (BQ25895)");
        LOG_I(TAG, "Battery: %d%% (%dmV)", s_powerManager->getBatteryPercent(),
              s_powerManager->getBatteryVoltage());
    } else {
        LOG_E(TAG, "Power Management init failed!");
    }
}

/**
 * \brief Initializes the sleep controller which manages light/deep sleep.
 */
static void initSleepController() {
    LOG_I(TAG, "Initializing Sleep Controller...");
    s_sleepController = cdc::hal::getSleepControllerInstance();
    if (s_sleepController && s_sleepController->init() && s_sleepController->start()) {
        LOG_I(TAG, "Sleep Controller ready (interval: %lus)",
              (unsigned long)s_sleepController->getLightSleepInterval());
    } else {
        LOG_E(TAG, "Sleep Controller init failed!");
    }
}

/**
 * \brief Initializes the WiFi controller HAL singleton.
 */
static void initWifiController() {
    LOG_I(TAG, "Initializing WiFi Controller...");
    cdc::hal::IWifiController* wifiController = cdc::hal::getWifiControllerInstance();
    if (wifiController && wifiController->init() && wifiController->start()) {
        LOG_I(TAG, "WiFi Controller ready");
    } else {
        LOG_E(TAG, "WiFi Controller init failed!");
    }
}

/**
 * \brief Initializes the Bluetooth controller HAL singleton.
 */
static void initBluetoothController() {
    LOG_I(TAG, "Initializing Bluetooth Controller...");
    cdc::hal::IBluetoothController* btController = cdc::hal::getBluetoothControllerInstance();
    if (btController && btController->init() && btController->start()) {
        LOG_I(TAG, "Bluetooth Controller ready");
    } else {
        LOG_E(TAG, "Bluetooth Controller init failed!");
    }
}

/**
 * \brief Initializes the keypad input scanner.
 */
static void initKeypad() {
    LOG_I(TAG, "Initializing Keypad...");
    s_keypad = cdc::hal::getKeypadInstance();
    if (s_keypad && s_keypad->init()) {
        LOG_I(TAG, "Keypad ready (12 keys)");
    } else {
        LOG_E(TAG, "Keypad init failed!");
    }
}

/**
 * \brief Initializes the TROPIC01 secure element and starts an active session.
 */
static void initSecureElement() {
    LOG_I(TAG, "Initializing Secure Element...");
    s_secureElement = cdc::hal::getSecureElementInstance();
    if (s_secureElement && s_secureElement->init() && s_secureElement->start()) {
        if (s_secureElement->sessionStart()) {
            LOG_I(TAG, "Secure Element ready (TROPIC01, session active)");
        } else {
            LOG_W(TAG, "Secure Element initialized but session start failed");
        }
    } else {
        LOG_E(TAG, "Secure Element init failed!");
    }
}

/**
 * \brief Brings up all hardware peripherals in dependency order.
 *
 * Combines I2C bus, power management, sleep controller, radios, keypad and
 * the secure element. Wakeup-cause handling is performed first so that the
 * RTC GPIO is released before the bus comes up.
 */
static void initHardware() {
    handleWakeupAndReleaseRtcGpio();
    initI2cBus();
    initPowerManager();
    initSleepController();
    initWifiController();
    initBluetoothController();
    initKeypad();
    initSecureElement();
}

/**
 * \brief Registers the attestation-key service with the global ServiceRegistry.
 */
static void initAttestationService() {
    s_attestationService.setSecureElement(s_secureElement);
    s_attestationService.init();
    s_attestationService.start();
    ServiceRegistry::instance().registerService("attestation_key", &s_attestationService);
}

/**
 * \brief Initializes and registers the TROPIC01 storage cache service.
 */
static void initTropicStorage() {
    auto& tropicStorage = cdc::core::TropicStorage::instance();
    tropicStorage.setSecureElement(s_secureElement);
    tropicStorage.init();
    tropicStorage.start();
    ServiceRegistry::instance().registerService("tropic_storage", &tropicStorage);
    LOG_I(TAG, "TropicStorage cache ready");
}

/**
 * \brief Initializes the serial command processor over USB CDC.
 */
static void initSerialCommandInterface() {
    cdc::serial::SerialCmd::init();
    LOG_I(TAG, "Serial Command Interface ready");
}

/**
 * \brief Initializes the badge-to-badge message transfer service.
 *
 * Must run before modules so they can register handlers in their initializers.
 * Honors the persisted beacon preference (auto-enables BLE when on).
 */
static void initMessageTransfer() {
    auto& msg = cdc::msg::MessageTransfer::instance();
    msg.init();
    msg.start();
    ServiceRegistry::instance().registerService("msg", &msg);
    LOG_I(TAG, "Message transfer service ready");
}

/**
 * \brief Brings up high-level OS services that depend on hardware being ready.
 */
static void initSystemServices() {
    initAttestationService();
    initTropicStorage();
    initMessageTransfer();
    initSerialCommandInterface();
}

/**
 * \brief Initializes the e-paper display and shows a boot splash.
 *
 * The wakeup cause cached during hardware init determines whether the splash
 * announces a regular boot or a deep-sleep wakeup.
 */
static void initDisplay() {
    LOG_I(TAG, "Initializing Display...");
    s_display = cdc::hal::getDisplayInstance();
    if (s_display && s_display->init() && s_display->start()) {
        LOG_I(TAG, "Display ready (%ux%u)", s_display->getWidth(), s_display->getHeight());

        cdc::core::SystemLock::instance().setShutdownHandler(lockdownShutdownHandler);

        if (s_wakeupCause == ESP_SLEEP_WAKEUP_EXT1) {
            s_display->showSplash("Waking up...");
        } else {
            s_display->showSplash();
        }
        LOG_I(TAG, "Splash screen done");
    } else {
        LOG_E(TAG, "Display init failed!");
    }
}

/**
 * \brief Initializes the App UI layer with the previously prepared HAL deps.
 */
static void initUi() {
    LOG_I(TAG, "Initializing UI...");
    cdc::ui::UiDeps deps;
    deps.display = s_display;
    deps.keypad = s_keypad;
    deps.power = s_powerManager;
    deps.sleep = s_sleepController;
    deps.secureElement = s_secureElement;
    cdc::ui::ui_init(deps);
}

/**
 * \brief Registers all auto-generated modules and runs their initializers.
 *
 * After modules are ready the UI menus are rebuilt to surface module-provided
 * entries.
 */
static void initModules() {
    modules_register_all();

    LOG_I(TAG, "Initializing modules...");
    cdc::core::ModuleRegistry::instance().runAllInitializers();

    cdc::ui::ui_on_modules_ready();
}

/**
 * \brief Bring up the WAMR runtime, mount the plugins partition and discover
 *        installed plugins. Phase 1: scaffolding only; actual plugin loading
 *        comes in Phase 2.
 */
static void initPluginSystem() {
    if (!cdc::plugin_manager::PluginManager::instance().init()) {
        LOG_W(TAG, "PluginManager init failed - plugins disabled");
        return;
    }
    cdc::plugin_manager::registerPluginSerialCommands();
    cdc::plugin_manager::registerGpioSerialCommands();

    // Scan the plugins FAT for available language files (fills the picker) and
    // load the persisted language's overlay. Fires the language-changed
    // callback registered in ui_init(), which rebuilds cached menu labels.
    cdc::ui::I18n::instance().loadOverlay();
}

/**
 * \brief Final startup step: completes USB CDC enumeration and prints banner.
 */
static void startApp() {
    usb_cdc_start();

    LOG_I(TAG, "=================================");
    LOG_I(TAG, "System ready. Entering main loop.");
    LOG_I(TAG, "=================================");
}

/**
 * \brief Single iteration of the cooperative main loop.
 *
 * Drains the event bus, services the serial console, ticks power management,
 * advances the UI and dispatches a tick to all modules.
 */
static void runMainLoopIteration() {
    // Hard lockdown gate: if any subsystem latched the lockdown flag, run the
    // halt-screen handler and deep-sleep without enabling wake sources.
    SystemLock::instance().enforceIfLocked();

    EventBus::instance().process();
    cdc::core::PinManager::instance().checkAndResetExpiredLockout();
    cdc::serial::SerialCmd::process();

    if (s_powerManager) {
        s_powerManager->update();
    }

    uint32_t nowMs = esp_timer_get_time() / 1000;
    cdc::ui::ui_process(nowMs);
    s_attestationService.onTick(nowMs);
    cdc::msg::MessageTransfer::instance().tick(nowMs);

    cdc::core::ModuleRegistry::instance().dispatchTick(nowMs);

#if DEBUG_MODE
    static UBaseType_t s_minStackFree = 0xFFFFFFFFu;
    UBaseType_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
    if (stackFree < s_minStackFree) {
        s_minStackFree = stackFree;
        LOG_I(TAG, "main stack low-water: %lu words", (unsigned long)stackFree);
    }
#endif

    vTaskDelay(pdMS_TO_TICKS(10));
}

/**
 * \brief Main firmware entry point.
 */
extern "C" void app_main(void)
{
    // STAGE 0: Hardware Minimum
    initNvs();
    bool profileChanged = checkBuildProfileAndWipeNvs();

    // STAGE 1: Core Services
    if (!initCoreServices()) {
        // No log channel and no display available at this point: best-effort
        // ESP_LOG to UART0, delay to let any output flush, then reboot.
        ESP_LOGE(TAG, "Core service init failed, restarting in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    // STAGE 2: Time
    initRtc();

    // STAGE 3: Hardware Peripherals
    initHardware();

    if (profileChanged) {
        wipeTropicForFactoryReset();
    }

    // STAGE 4: System Services
    initSystemServices();

    // STAGE 5: Display & UI
    initDisplay();
    initUi();

    // STAGE 6: Modules
    initModules();

    // Restore persisted WiFi intent: reconnect if WiFi was on before reboot.
    cdc::ui::WifiHandlers::instance().restoreOnBoot();

    // STAGE 6b: Plugin system (WAMR + plugins partition)
    initPluginSystem();

    // All modules and plugins have now registered their GATT services and
    // advertising UUIDs. Release the BLE boot barrier so any deferred enable
    // brings the stack up once, committing every service together.
    if (auto* bt = cdc::hal::getBluetoothControllerInstance()) {
        bt->notifySystemReady();
    }

    // STAGE 7: Final startup
    startApp();

    // Main loop
    while (true) {
        runMainLoopIteration();
    }
}
