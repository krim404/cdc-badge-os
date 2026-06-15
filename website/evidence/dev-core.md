# Evidence: Developer core pages

Pages: `dev/index.md`, `dev/architecture.md`, `dev/build-system.md`.

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Build platform is ESP-IDF via PlatformIO | platformio.ini:7,11 | VERIFIED |
| Build env is `cdc_badge_usb`, board `cdc-badge-usb`, flash 16MB, PSRAM enabled | platformio.ini:6,8,10,11 | VERIFIED |
| C++17 (`-std=gnu++17`), unflags c++2b/c++23 | platformio.ini:17,35-37 | VERIFIED |
| Partition file is partitions.csv, sdkconfig.defaults used | platformio.ini:9,12 | VERIFIED |
| pre-build scripts: python deps, submodules, component manager | platformio.ini:39-42 | VERIFIED |
| MODULES list (12 entries) lives in main/CMakeLists.txt | main/CMakeLists.txt:8-21 | VERIFIED |
| Generated header modules_init.gen.h written by CMake | main/CMakeLists.txt:33-49 | VERIFIED |
| Generated `extern "C" void <mod>_register();` + `modules_register_all()` | main/CMakeLists.txt:40-49 | VERIFIED |
| main includes modules_init.gen.h, calls modules_register_all() then runAllInitializers() | main/main.cpp:28,520,523 | VERIFIED |
| main/CMakeLists REQUIRES core+os+msg+wamr+plugin_manager+MODULES | main/CMakeLists.txt:24-30 | VERIFIED |
| Partition offsets: nvs 0x9000, app0 0x50000, plugins 0xDF0000 fat, coredump 0xFF0000 | partitions.csv:2-5 | VERIFIED |
| PSRAM octal 80MHz, BSS-in-PSRAM allowed | sdkconfig.defaults:17-22 | VERIFIED |
| psramAlloc uses heap_caps_malloc(MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT) | components/cdc_core/include/cdc_core/Raii.h:51-55 | VERIFIED |
| PsramUniquePtr = unique_ptr with CapsFreeDeleter | components/cdc_core/include/cdc_core/Raii.h:45 | VERIFIED |
| EXT_RAM_BSS_ATTR used for static large buffers (multiple components) | components/cdc_log/src/cdc_log.cpp, components/mod_2fa/src/TwoFaModule.cpp (grep) | VERIFIED |
| IService lifecycle: init/start/stop/getState/getName | components/cdc_core/include/cdc_core/IService.h:35-56 | VERIFIED |
| ServiceState enum UNINITIALIZED..ERROR | components/cdc_core/include/cdc_core/IService.h:10-16 | VERIFIED |
| ServiceRegistry: named + typed service locator, MAX_SERVICES 24, static | components/cdc_core/include/cdc_core/ServiceRegistry.h:29-31,135-141 | VERIFIED |
| ServiceRegistry initAll/startAll/stopAll | components/cdc_core/include/cdc_core/ServiceRegistry.h:103-114 | VERIFIED |
| ServiceType enum KEYBOARD/CHALLENGE_RESPONDER/CLIPBOARD/NOTIFICATION | components/cdc_core/include/cdc_core/ServiceRegistry.h:12-17 | VERIFIED |
| EventBus pub/sub, FreeRTOS queue, ISR-safe publish, static, MAX_HANDLERS 16 | components/cdc_core/include/cdc_core/EventBus.h:70-77,98,112 | VERIFIED |
| EventBus handlers run from process() in main loop | components/cdc_core/include/cdc_core/EventBus.h:73,122 | VERIFIED |
| EventType set (input/power/system/ble/timer/module) | components/cdc_core/include/cdc_core/EventBus.h:11-48 | VERIFIED |
| IModule extends IService; modules self-contained features | components/cdc_core/include/cdc_core/IModule.h:55,50-53 | VERIFIED |
| IModule lifecycle hooks onUnlock/onLock/onUsbConnect/onUsbDisconnect/onTick | components/cdc_core/include/cdc_core/IModule.h:145,150,155,160,166 | VERIFIED |
| IModule getMenuItems/getEntryView/getLockScreenContextItems | components/cdc_core/include/cdc_core/IModule.h:120,129,137 | VERIFIED |
| IModule exportBackup/importBackup semantic records | components/cdc_core/include/cdc_core/IModule.h:100,112 | VERIFIED |
| IModule slot request/range (compile-time memory map) | components/cdc_core/include/cdc_core/IModule.h:57-71,172,178 | VERIFIED |
| MenuLocation enum MAIN/TOOLS/SETTINGS/BLUETOOTH/WIFI/EXPERT | components/cdc_core/include/cdc_core/IModule.h:17-24 | VERIFIED |
| ModuleBase provides default start/stop state transitions + name/state | components/cdc_core/include/cdc_core/ModuleBase.h:20,49-63 | VERIFIED |
| ModuleRegistry: registration, lifecycle, menu collection, event dispatch | components/cdc_core/include/cdc_core/ModuleRegistry.h:10-15,29 | VERIFIED |
| ModuleRegistry MAX_MODULES 16, NVS_PREFIX "mod_" | components/cdc_core/include/cdc_core/ModuleRegistry.h:31,55 | VERIFIED |
| ModuleRegistry enable/disable persists via NVS disabled-list (by name) | components/cdc_core/include/cdc_core/ModuleRegistry.h:160-182,244-257 | VERIFIED |
| ModuleRegistry slot validation + error reporting | components/cdc_core/include/cdc_core/ModuleRegistry.h:185-215,261-272 | VERIFIED |
| ModuleRegistry orphaned NVS cleanup | components/cdc_core/include/cdc_core/ModuleRegistry.h:46-49,220-224 | VERIFIED |
| PinManager: device PINs in TROPIC01 R-Memory slot 0, attestation-signed | components/cdc_core/include/cdc_core/PinManager.h:9,56,58-61 | VERIFIED |
| PinManager badge PIN RAM retry + recovery timer; PW1/PW3 smartcard terminal | components/cdc_core/include/cdc_core/PinManager.h:32-39 | VERIFIED |
| Core has no references to any mod_* (isolation) | grep components/cdc_core for mod_* = empty | VERIFIED |
| IDisplay = E-paper display, RefreshMode FULL/PARTIAL/PARTIAL_LIGHT | components/cdc_hal/include/cdc_hal/IDisplay.h:11-19 | VERIFIED |
| IKeypad = button input, KeyCallback | components/cdc_hal/include/cdc_hal/IKeypad.h:24-31 | VERIFIED |
| IPowerManager = BQ25895 power | components/cdc_hal/include/cdc_hal/IPowerManager.h:28-30 | VERIFIED |
| ISecureElement = TROPIC01: ECC (32 slots), ECDSA/EdDSA, R-Memory 512 slots, TRNG | components/cdc_hal/include/cdc_hal/ISecureElement.h:49-57 | VERIFIED |
| IBluetoothController interface | components/cdc_hal/include/cdc_hal/IBluetoothController.h:144 | VERIFIED |
| IWifiController = WiFi stack + connection mgmt | components/cdc_hal/include/cdc_hal/IWifiController.h:52-56 | VERIFIED |
| ISleepController = light/deep sleep | components/cdc_hal/include/cdc_hal/ISleepController.h:44-53 | VERIFIED |
| IRtc = internal RTC | components/cdc_hal/include/cdc_hal/IRtc.h:10-16 | VERIFIED |
| II2cBus, IEspHardware interfaces | components/cdc_hal/include/cdc_hal/II2cBus.h:13-15, IEspHardware.h:6-12 | VERIFIED |
| All HAL interfaces extend core::IService | components/cdc_hal/include/cdc_hal/IDisplay.h:20 etc. | VERIFIED |
| IView = base UI screen, onEnter/onExit, InputResult | components/cdc_ui/include/cdc_ui/IView.h:17-24,10-15,35,40 | VERIFIED |
| ViewStack = navigation stack, push/pop/replace + modal overlays, MAX_DEPTH 20 | components/cdc_ui/include/cdc_ui/ViewStack.h:10-20,32,38,45 | VERIFIED |
| I18n = English fallback in code + runtime overlay JSON | components/cdc_ui/include/cdc_ui/I18n.h:3-23,49-55 | VERIFIED |
| cdc_views = reusable view components (ListView etc.) | components/cdc_views/include/cdc_views/ListView.h:22-24 | VERIFIED |
| cdc_os_ui = OS-level UI (AppUi ui_init/ui_process), depends on HAL via UiDeps | components/cdc_os_ui/include/cdc_os_ui/AppUi.h:17-35 | VERIFIED |
| serial_cmd SerialCmd = serial command processor (buffer, dispatch, auth) | components/serial_cmd/include/serial_cmd/SerialCmd.h:14-22 | VERIFIED |
| serial_cmd Console = printf/char I/O over CDC/UART/BLE | components/serial_cmd/include/serial_cmd/Console.h:8-13 | VERIFIED |
| usb_badge usb_cdc = TinyUSB CDC serial init/start | components/usb_badge/include/usb_badge/usb_cdc.h:1-5,24,30 | VERIFIED |
| usb_badge usb_hid = composite CDC + HID/CCID interfaces from modules | components/usb_badge/include/usb_badge/usb_hid.h:1-6 | VERIFIED |
| cdc_msg MessageTransfer = headless badge-to-badge BLE MIME transfer, ephemeral pairing | components/cdc_msg/include/cdc_msg/MessageTransfer.h:20-33 | VERIFIED |
| cdc_msg MessageHandlerRegistry = MIME -> DeliverFn, fixed-size | components/cdc_msg/include/cdc_msg/MessageHandlerRegistry.h:24-30,21-22 | VERIFIED |
| plugin_manager PluginManager = discover/load/run/unload WASM, foreground/background/autoload | components/plugin_manager/include/plugin_manager/PluginManager.h:3-12,45 | VERIFIED |
| plugins partition mounted at /plugins, label "plugins" | components/plugin_manager/src/PluginStorage.cpp:20-21 | VERIFIED |
| host_api.h canonical here; SDK in sibling repo (upstream codeberg) | CLAUDE.md + git remote -v (upstream ssh://git@codeberg.org/Krim/cdc-badge-os.git) | VERIFIED |

## GAPs / discrepancies

- ISecureElement.h:54 says R-Memory "512 slots, 476 bytes each"; CLAUDE.md says "444 bytes". Byte size per slot not asserted in docs (omitted) pending reconciliation. Slot count 512 and "32 ECC slots" are consistent across both.
- CLAUDE.md slot-allocation table (per-module ECC/R-Mem ranges) lives in main/tropic_slot_map.h (not read in full here); architecture page references the secure-element interface only, not the per-module allocation table.
