# Evidence ledger: Plugins (power-user pages)

Tags: VERIFIED = exact source line supports the claim; GAP = not verifiable in source.

Covers the four power-user plugin pages:
overview, install, manage, capabilities.

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Plugins run inside the WAMR runtime (WebAssembly Micro Runtime), not statically linked | components/wamr_runtime/CMakeLists.txt:1-6 | VERIFIED |
| WAMR submodule version is 2.4.4 | components/wamr_runtime/wasm-micro-runtime/core/version.h:19-21 (MAJOR 2, MINOR 4, PATCH 4) | VERIFIED |
| Fast Interpreter + AOT enabled, JIT disabled | components/wamr_runtime/CMakeLists.txt:13-16 (`WAMR_BUILD_FAST_INTERP 1`, `WAMR_BUILD_AOT 1`, `WAMR_BUILD_JIT 0`) | VERIFIED |
| CMake header comment still says "Classic Interpreter" while FAST_INTERP=1 is set | components/wamr_runtime/CMakeLists.txt:4 vs :14 | GAP (comment/build mismatch; doc states Fast Interpreter per the build flag) |
| WAMR allocations routed through a PSRAM custom allocator | components/wamr_runtime/CMakeLists.txt:4-6 (`custom allocator that lives in PSRAM`) | VERIFIED |
| Plugins live on a FAT-FS partition named `plugins` | partitions.csv:4 (`plugins, data, fat`) | VERIFIED |
| Plugins partition size = 0x200000 = 2 MB at offset 0xDF0000 | partitions.csv:4 | VERIFIED |
| Partition auto-formats on first boot if empty | components/plugin_manager/include/plugin_manager/PluginStorage.h:21-25 (`Auto-formats if empty`) | VERIFIED |
| VFS base path is `/plugins` | components/plugin_manager/include/plugin_manager/PluginStorage.h:33-35 | VERIFIED |
| A plugin is recognised by `<id>.wasm` + `<id>.meta` both present | components/plugin_manager/include/plugin_manager/PluginStorage.h:38-41 | VERIFIED |
| Loader prefers `<id>.aot` over `<id>.wasm` if present | components/plugin_manager/include/plugin_manager/PluginStorage.h:44-47 | VERIFIED |
| Optional translation overlay file is `<id>.lang` | components/plugin_manager/include/plugin_manager/PluginStorage.h:64-67 | VERIFIED |
| Disabled marker is `<id>.disabled` | components/plugin_manager/include/plugin_manager/PluginStorage.h:69-72 | VERIFIED |
| Host API level is 0.7 (major 0, minor 7) | components/plugin_manager/include/plugin_manager/host_api.h:28-30 | VERIFIED |
| Manifest api_level_major must equal firmware major, api_level_minor must be <= firmware minor | components/plugin_manager/src/CapabilityChecker.cpp:44-49 | VERIFIED |
| Operand/frame stack is a fixed 64 KB | components/plugin_manager/src/Plugin.cpp:100,108,118 (`stack_bytes = 64 * 1024`); CapabilityChecker.cpp:17-19 | VERIFIED |
| Manifest `linear_memory_kb` clamped to [16, 4096], default 64 | components/plugin_manager/src/CapabilityChecker.cpp:20-21,51-57; PluginManifest.h:78 | VERIFIED |
| Plugins menu is a fixed main-menu item ("Plugins") | components/cdc_os_ui/src/AppUi.cpp:159-160,462,559-561 | VERIFIED |
| Selecting an item in the plugin list starts that plugin | components/plugin_manager/src/PluginListView.cpp:208-215 (`onSelect` -> `startPlugin`) | VERIFIED |
| Context menu opened with key 3 (KEY_MENU) | components/cdc_views/include/cdc_views/KeyCodes.h:47 (`KEY_MENU = '3'`); components/cdc_views/src/ListView.cpp:182-193 | VERIFIED |
| Context menu offers Start (stopped), Stop (running), Disable; Enable when disabled | components/plugin_manager/src/PluginListView.cpp:217-237 | VERIFIED |
| Context-menu Stop force-unloads via unloadFromRam | components/plugin_manager/src/PluginListView.cpp:24-33 | VERIFIED |
| Disabled plugin shows 'X' icon; background-running shows sun icon | components/plugin_manager/src/PluginListView.cpp:184-190 | VERIFIED |
| `background` keeps plugin running/ticking after the user leaves its view | components/plugin_manager/include/plugin_manager/PluginManifest.h:31-36; PluginManager.h:99-101 | VERIFIED |
| Leaving a background plugin's view shows a "runs in background" toast then demotes it | components/plugin_manager/src/PluginListView.cpp:124-135 (`core.plugin_bg_running` toast) | VERIFIED |
| `background` is not a boot flag; user still starts it manually | components/plugin_manager/include/plugin_manager/PluginManifest.h:32-34 | VERIFIED |
| `autoload` starts the plugin as a resident background instance at boot | components/plugin_manager/include/plugin_manager/PluginManifest.h:45-51; PluginManager.cpp:518-543 (`loadAutoloadPlugins`) | VERIFIED |
| Autoload at boot is headless (loadIntoBackground, no foreground view) | components/plugin_manager/src/PluginManager.cpp:537 (`loadIntoBackground`); PluginManager.h:181-189 | VERIFIED |
| Autoload skips disabled plugins | components/plugin_manager/src/PluginManager.cpp:526-529 | VERIFIED |
| `autoload` is orthogonal to `background` | components/plugin_manager/include/plugin_manager/PluginManifest.h:48-50 | VERIFIED |
| `prevent_sleep` holds a sleep inhibitor while loaded | components/plugin_manager/include/plugin_manager/PluginManifest.h:38; PluginManager.cpp:66-80 (`applySleepInhibitor`) | VERIFIED |
| prevent_sleep means no auto-lock while plugin holds foreground | components/cdc_os_ui/src/AppUi.cpp:409; PluginManager.h:102-107 | VERIFIED |
| Lockscreen shows BACKGROUND status icon when any plugin is resident in background | components/cdc_os_ui/src/AppUi.cpp:298-305 (`hasBackgroundPlugin` -> `StatusIcon::BACKGROUND`) | VERIFIED |
| Lockscreen shows CAFFEINATED icon when sleep is inhibited | components/cdc_os_ui/src/SleepManager.cpp:262-271 | VERIFIED |
| Serial command set: LIST/INFO/START/STOP/CMD/DISABLE/ENABLE/DELETE/UPLOAD/UPLOAD_AOT/UPLOAD_META/UPLOAD_LANG/ABORT/DEBUG | components/plugin_manager/src/PluginSerialCommands.cpp:555-571 | VERIFIED |
| PLUGIN LIST returns JSON with id/name/version/disabled | components/plugin_manager/src/PluginSerialCommands.cpp:236-259 | VERIFIED |
| PLUGIN DELETE removes wasm + aot + meta + lang + disabled files | components/plugin_manager/src/PluginSerialCommands.cpp:335-346 | VERIFIED |
| PLUGIN DISABLE unloads from RAM and writes the disabled marker | components/plugin_manager/src/PluginSerialCommands.cpp:348-356; PluginListView.cpp:38-51 | VERIFIED |
| Upload is a binary stream: `PLUGIN UPLOAD <id> <size> <crc32_hex>` then raw bytes, CRC32 verified | components/plugin_manager/src/PluginSerialCommands.cpp:450-498,149-196 | VERIFIED |
| Upload protected by a 15 s inactivity auto-abort | components/plugin_manager/src/PluginSerialCommands.cpp:76,117-144 | VERIFIED |
| Re-uploading a `.wasm` overwriting an active foreground plugin unloads it first | components/plugin_manager/src/PluginSerialCommands.cpp:463-469 | VERIFIED |
| Re-uploading a background plugin's wasm reloads the running instance | components/plugin_manager/src/PluginSerialCommands.cpp:190-195 (`reloadBackgroundPlugin`) | VERIFIED |
| tools/upload.py uploads meta then wasm/aot then optional lang | tools/upload.py:192-214 | VERIFIED |
| tools/upload.py derives id from meta.json#id unless --id given | tools/upload.py:195 | VERIFIED |
| tools/upload.py PIN via --pin sends AUTH (FEATURE_SECURE_SERIAL) | tools/upload.py:99-114,307 | VERIFIED |
| tools/upload.py modes: --wasm/--meta, --list/--info/--delete/--start/--stop, --lang-overlay, --put | tools/upload.py:8-23,303-336 | VERIFIED |
| tools/upload.py auto-detects the serial port | tools/upload.py:42-54 | VERIFIED |
| Plugin file capability families (host_* prefixes) | components/plugin_manager/include/plugin_manager/host_api.h (host_nvs_/host_fs_/host_http_/host_wifi_/host_ble_/host_socket_/host_se_/host_rmem_/host_aes_/host_base32_/host_base64_/host_gpio_/host_adc_/host_pwm_/host_i2c_/host_pixel_/host_ui_/host_view_/host_display_/host_msg_/host_event_/host_i18n_/host_lockscreen_/host_power_/host_log_) | VERIFIED |
| Capability flags: wifi/ble/http/socket/ui_exclusive/display_lowlevel/sao/grove/pixel_strip/background/usb_cdc/prevent_sleep/vfat/autoload | components/plugin_manager/include/plugin_manager/PluginManifest.h:21-64 | VERIFIED |
| Resource requests: rmem, ecc, ble_service_uuids, message_types, gpio_pins, pwm_pins, adc_pins, i2c_bus, nvs_namespace | components/plugin_manager/include/plugin_manager/PluginManifest.h:53-63 | VERIFIED |
| vfat confines plugin file access to /plugins/data/<id>/ | components/plugin_manager/include/plugin_manager/PluginManifest.h:39-44 | VERIFIED |
| GPIO/PWM/ADC pins validated against a shared allow/block policy | components/plugin_manager/src/CapabilityChecker.cpp:75-106; PluginGpioPolicy.h | VERIFIED |
| GPIO hard-blocked pins | components/plugin_manager/include/plugin_manager/PluginGpioPolicy.h:19-41 (0,1,8,10-13,17-21,26-37,39,41,42,45-48) | VERIFIED |
| GPIO whitelist | components/plugin_manager/include/plugin_manager/PluginGpioPolicy.h:43-58 (2,3,4,5,6,7,9,14,15,16,38,40,43,44) | VERIFIED |
| I2C bus 0 reserved (internal charger + IO expander), plugins use bus 1 | components/plugin_manager/src/CapabilityChecker.cpp:108-115; PluginGpioPolicy.h:39-40 | VERIFIED |
| nvs_namespace must start with plg_ or plugin_, [a-z0-9_], <= 15 chars | components/plugin_manager/src/CapabilityChecker.cpp:117-140 | VERIFIED |
| nvs_namespace required when rmem slots are requested | components/plugin_manager/src/CapabilityChecker.cpp:137-140 | VERIFIED |
| ble_service_uuid must be a 128-bit lowercase dashed UUID | components/plugin_manager/src/CapabilityChecker.cpp:24-38,142-148 | VERIFIED |
| message_types (MIME) implies messaging; sending also requires ble | components/plugin_manager/include/plugin_manager/PluginManifest.h:56-58 | VERIFIED |
| Plugin secure-element ECC slot 31, R-Memory 501-511 | main/tropic_slot_map.h (per CLAUDE.md slot allocation table; host_api_se.cpp consumes slot 31) | GAP (slot map header not directly read in this pass; sourced from CLAUDE.md) |
| Web installer + Rust SDK live at the cdc-badge-plugins repo | external: https://github.com/krim404/cdc-badge-plugins (link only; installer internals not asserted) | VERIFIED (link target) |
