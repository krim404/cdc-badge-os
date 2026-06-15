# Evidence ledger: dev/module-development.md + dev/ui-framework.md

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| IModule is the module interface, extends IService | components/cdc_core/include/cdc_core/IModule.h:55 | verified |
| IService defines init/start/stop/getState/getName | components/cdc_core/include/cdc_core/IService.h:27,35,41,46,51,56 | verified |
| ServiceState enum UNINITIALIZED..ERROR | components/cdc_core/include/cdc_core/IService.h:10 | verified |
| getVersion() is the only extra pure virtual on IModule | components/cdc_core/include/cdc_core/IModule.h:77 | verified |
| getMenuItems default returns 0 | components/cdc_core/include/cdc_core/IModule.h:120 | verified |
| getEntryView default nullptr | components/cdc_core/include/cdc_core/IModule.h:129 | verified |
| getLockScreenContextItems default 0 | components/cdc_core/include/cdc_core/IModule.h:137 | verified |
| onUnlock/onLock default no-op | components/cdc_core/include/cdc_core/IModule.h:145,150 | verified |
| onUsbConnect/onUsbDisconnect default no-op | components/cdc_core/include/cdc_core/IModule.h:155,160 | verified |
| onTick default no-op | components/cdc_core/include/cdc_core/IModule.h:166 | verified |
| exportBackup/importBackup default no-op | components/cdc_core/include/cdc_core/IModule.h:100,112 | verified |
| setSlotRange/getSlotRequest default | components/cdc_core/include/cdc_core/IModule.h:172,178 | verified |
| MenuLocation enum (MAIN/TOOLS/SETTINGS/BLUETOOTH/WIFI/EXPERT) | components/cdc_core/include/cdc_core/IModule.h:17-24 | verified |
| ModuleMenuItem fields (label/priority/getView/isVisible/moduleName/location/onSelect) | components/cdc_core/include/cdc_core/IModule.h:29-37 | verified |
| Registry fans onUnlock/onLock/onUsbConnect/onUsbDisconnect/onTick to modules | components/cdc_core/include/cdc_core/ModuleRegistry.h:140-144 | verified |
| onTick dispatched once per main loop iteration | main/main.cpp:581 | verified |
| ModuleBase stores state_/name_, implements getName/getState | components/cdc_core/include/cdc_core/ModuleBase.h:20,32,38 | verified |
| ModuleBase::start() guards INITIALIZED/STOPPED -> STARTED | components/cdc_core/include/cdc_core/ModuleBase.h:49-56 | verified |
| ModuleBase::stop() -> STOPPED | components/cdc_core/include/cdc_core/ModuleBase.h:61-63 | verified |
| ModuleBase usage note (implement init, inherit or override start/stop) | components/cdc_core/include/cdc_core/ModuleBase.h:8-18 | verified |
| mod_sao derives from core::IModule, tracks state_ itself | components/mod_sao/include/mod_sao/SaoModule.h:7,23 | verified |
| mod_sao::start() reproduces INITIALIZED/STOPPED guard | components/mod_sao/src/SaoModule.cpp:43-53 | verified |
| MODULES list single registration point | main/CMakeLists.txt:8 | verified |
| MODULES spliced into main REQUIRES | main/CMakeLists.txt:30 | verified |
| CMake generates modules_init.gen.h with extern decls + modules_register_all | main/CMakeLists.txt:33-49 | verified |
| Boot calls modules_register_all then runAllInitializers | main/main.cpp:520-523 | verified |
| mod_sao_register registers a deferred initializer calling init() | components/mod_sao/src/SaoModule.cpp:80-85 | verified |
| registerInitializer stores initializer | components/cdc_core/include/cdc_core/ModuleRegistry.h:42; components/cdc_core/src/ModuleRegistry.cpp:53 | verified |
| runAllInitializers runs initializers and post-housekeeping | components/cdc_core/src/ModuleRegistry.cpp:67-119 | verified |
| runAllInitializers aborts if TropicStorage not STARTED | components/cdc_core/src/ModuleRegistry.cpp:68-76 | verified |
| TropicStorage up in initSystemServices before initModules | main/main.cpp:624,631 | verified |
| Boot order: run initializers -> init() each module | components/cdc_core/src/ModuleRegistry.cpp:80-84 | verified |
| Verify every module has module_defaults entry, error if missing | components/cdc_core/src/ModuleRegistry.cpp:88-93 | verified |
| Load disabled list from NVS | components/cdc_core/src/ModuleRegistry.cpp:96 | verified |
| Stop disabled module that self-started | components/cdc_core/src/ModuleRegistry.cpp:102-107 | verified |
| startModule(i) for enabled set | components/cdc_core/src/ModuleRegistry.cpp:108-112 | verified |
| mod_sao::init registers strings, registers module, sets INITIALIZED | components/mod_sao/src/SaoModule.cpp:35-41 | verified |
| registerModule stores instance, applies slot request, logs | components/cdc_core/src/ModuleRegistry.cpp:126-150 | verified |
| registerModule rejects duplicate name | components/cdc_core/src/ModuleRegistry.cpp:138-143 | verified |
| MODULE_DEFAULT_MAP location | main/module_defaults.h:15 | verified |
| Default map rows incl mod_usbhid/mod_otphid false, mod_vfat true | main/module_defaults.h:15-27 | verified |
| Default map name must match getName() and MODULES | main/module_defaults.h:9-10 | verified |
| User toggles persist in NVS independent of map | main/module_defaults.h:5-6 | verified |
| Registry collects/sorts menu items per location | components/cdc_core/include/cdc_core/ModuleRegistry.h:117 | verified |
| mod_sao getMenuItems contributes one TOOLS_MENU item | components/mod_sao/src/SaoModule.cpp:59-71 | verified |
| mod_sao keeps static view, fills on demand | components/mod_sao/src/SaoModule.cpp:21-28 | verified |
| registerEnglishTable signature, stored by pointer, keep alive | components/cdc_ui/include/cdc_ui/I18n.h:96-106 | verified |
| mod_sao kStrings table + registerStrings | components/mod_sao/src/SaoModule.cpp:13-19 | verified |
| mod_<name>.* key convention | components/cdc_ui/include/cdc_ui/I18n.h:22 | verified |
| ui::tr inline lookup | components/cdc_ui/include/cdc_ui/I18n.h:208 | verified |
| Module NVS namespace prefix "mod_" | components/cdc_core/include/cdc_core/ModuleRegistry.h:55; components/cdc_core/src/ModuleRegistry.cpp:483 | verified |
| Orphaned module NVS namespace erased on removal | components/cdc_core/src/ModuleRegistry.cpp:441-495 | verified |
| mod_sao CMakeLists REQUIRES list | components/mod_sao/CMakeLists.txt:3-15 | verified |
| BLE module must not add bt / use IBluetoothController | CLAUDE.md Module Pattern; MEMORY.md BLE Architecture | project-rule |
| IView interface | components/cdc_ui/include/cdc_ui/IView.h:25 | verified |
| IView lifecycle onEnter/onExit/onResume | components/cdc_ui/include/cdc_ui/IView.h:35,40,45 | verified |
| IView render/needsRender/markDirty/clearDirty | components/cdc_ui/include/cdc_ui/IView.h:53,58,63,70 | verified |
| IView onKey/onLongPress | components/cdc_ui/include/cdc_ui/IView.h:88,95 | verified |
| IView onTick/getFooterHint/setFooterHint/getName | components/cdc_ui/include/cdc_ui/IView.h:103,111,120,127 | verified |
| InputResult enum CONSUMED/IGNORED/REQUEST_POP/REQUEST_PUSH | components/cdc_ui/include/cdc_ui/IView.h:10 | verified |
| ViewBase default base, dirty flag, default lifecycle | components/cdc_ui/include/cdc_ui/IView.h:138,143-165 | verified |
| animating views must markDirty in onTick | components/cdc_ui/include/cdc_ui/IView.h:64-70 | verified |
| ViewStack singleton | components/cdc_ui/include/cdc_ui/ViewStack.h:18; components/cdc_ui/src/ViewStack.cpp:34 | verified |
| MAX_DEPTH = 20 | components/cdc_ui/include/cdc_ui/ViewStack.h:20 | verified |
| push -> push_unlocked -> onEnter | components/cdc_ui/src/ViewStack.cpp:174,72 | verified |
| pop -> pop_unlocked, onExit/onResume | components/cdc_ui/src/ViewStack.cpp:179,94 | verified |
| pop refuses to remove root | components/cdc_ui/src/ViewStack.cpp:95-98 | verified |
| replace swaps top | components/cdc_ui/src/ViewStack.cpp:184 | verified |
| popToRoot / popToAnchor / popToDepth | components/cdc_ui/src/ViewStack.cpp:214,221,230 | verified |
| current/at/depth inspectors | components/cdc_ui/src/ViewStack.cpp:237,243; ViewStack.h:83 | verified |
| list-to-list transition stays partial, else full | components/cdc_ui/src/ViewStack.cpp:89,117 | verified |
| Recursive mutex rationale (multi-task + reentrant callbacks) | components/cdc_ui/src/ViewStack.cpp:1-12,43-47 | verified |
| acquireExclusive / releaseExclusive | components/cdc_ui/include/cdc_ui/ViewStack.h:177; components/cdc_ui/src/ViewStack.cpp:471,486 | verified |
| Exclusive lock blocks push/pop/showModal from non-owner | components/cdc_ui/src/ViewStack.cpp:77-81,101-105,386-390 | verified |
| Modal stack: showModal pushes on top, hideModal removes top | components/cdc_ui/include/cdc_ui/ViewStack.h:127-138; ViewStack.cpp:383,419 | verified |
| MAX_MODAL_DEPTH = 4 | components/cdc_ui/include/cdc_ui/ViewStack.h:219 | verified |
| Stack full drops oldest bottom modal | components/cdc_ui/src/ViewStack.cpp:403-407 | verified |
| Re-show lifts existing modal to top (no duplicate) | components/cdc_ui/src/ViewStack.cpp:393-400 | verified |
| removeModal removes from any position | components/cdc_ui/include/cdc_ui/ViewStack.h:141-149; ViewStack.cpp:142 | verified |
| hasModal/getModal | components/cdc_ui/include/cdc_ui/ViewStack.h:154,159 | verified |
| Stacking modal forces full composite | components/cdc_ui/src/ViewStack.cpp:409-412 | verified |
| Dismissed modal full refresh to erase | components/cdc_ui/src/ViewStack.cpp:128-130 | verified |
| Keypad Key enum maps to chars 0-9 Y N | components/cdc_hal/include/cdc_hal/IKeypad.h:16-21 | verified |
| ui_process reads next key, calls dispatchKey | components/cdc_os_ui/src/AppUi.cpp:1109-1113 | verified |
| dispatchKey: modal gets input exclusively, no fallthrough | components/cdc_ui/src/ViewStack.cpp:255-263 | verified |
| dispatchKey: REQUEST_POP triggers pop | components/cdc_ui/src/ViewStack.cpp:265-271 | verified |
| dispatchLongPress: N universal back/cancel | components/cdc_ui/src/ViewStack.cpp:283-301 | verified |
| dispatchTick ticks top modal + current view | components/cdc_ui/src/ViewStack.cpp:304 | verified |
| main UI tick calls dispatchTick | components/cdc_os_ui/src/AppUi.cpp:1122 | verified |
| ListView keys 2/8/Y/N (+3 context) | components/cdc_views/include/cdc_views/ListView.h:28-32,34 | verified |
| ListView stores items by pointer, optional mutex | components/cdc_views/include/cdc_views/ListView.h:67-90 | verified |
| ListView visible rows fixed at 4 | components/cdc_views/include/cdc_views/ListView.h:38-39 | verified |
| T9InputView keys 0-9/N/Y | components/cdc_views/include/cdc_views/T9InputView.h:14-18,20 | verified |
| Other reusable views exist (PinEntry/Slider/Confirm/MessageBox/Toast/Info/QRCode/Canvas/ContextMenu/ColorPicker/Date/Time) | components/cdc_views/include/cdc_views/*.h | verified |
| CalEPD print overload assumes UTF-8, +64 on 0x84..0xBE corrupts CP437 | components/cdc_views/src/RenderHelpers.cpp:16-27 | verified |
| render namespace helpers | components/cdc_views/include/cdc_views/RenderHelpers.h:12 | verified |
| printText: built-in glcdfont, raw CP437 | components/cdc_views/include/cdc_views/RenderHelpers.h:114; RenderHelpers.cpp:442 | verified |
| drawText: font-aware raw / CP437->Latin1 | components/cdc_views/include/cdc_views/RenderHelpers.h:102; RenderHelpers.cpp:428 | verified |
| drawCp437Text: CP437->Latin1 per byte | components/cdc_views/include/cdc_views/RenderHelpers.h:89; RenderHelpers.cpp:421 | verified |
| chrome helpers byte-safe internally | components/cdc_views/include/cdc_views/RenderHelpers.h:19-40; RenderHelpers.cpp:40 | verified |
| writeRaw loops gfx->write(byte) | components/cdc_views/src/RenderHelpers.cpp:23-27,436,445 | verified |
| drawText maps CP437->Latin1 for TTF fonts | components/cdc_views/src/RenderHelpers.cpp:434-439 | verified |
| RefreshMode enum FULL/PARTIAL/PARTIAL_LIGHT | components/cdc_hal/include/cdc_hal/IDisplay.h:11-14 | verified |
| flush/flushSync default PARTIAL | components/cdc_hal/include/cdc_hal/IDisplay.h:33,39 | verified |
| render() chooses mode | components/cdc_ui/src/ViewStack.cpp:315 | verified |
| view change sets needsFullRefresh_ -> FULL | components/cdc_ui/src/ViewStack.cpp:364-366,89,117 | verified |
| plain repaint: PARTIAL_LIGHT if prefersLightRefresh else PARTIAL | components/cdc_ui/src/ViewStack.cpp:364-366 | verified |
| modal: dirty base PARTIAL, FULL for modal-stack change | components/cdc_ui/src/ViewStack.cpp:347-353 | verified |
| prefersLightRefresh default false | components/cdc_ui/include/cdc_ui/IView.h:79 | verified |
| LockScreenView prefersLightRefresh true (clock) | components/cdc_os_ui/include/cdc_os_ui/views/LockScreenView.h:120; IView.h:73-79 | verified |
| render(sync) uses flushSync else flush | components/cdc_ui/src/ViewStack.cpp:367-370 | verified |
| main tick async path | components/cdc_os_ui/src/AppUi.cpp:1131-1132 | verified |
| sleep path flushes synchronously | components/cdc_os_ui/src/SleepManager.cpp:120 | verified |
