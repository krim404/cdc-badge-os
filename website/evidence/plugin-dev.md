# Evidence ledger: Plugin SDK, manifest and Host API

Tags: VERIFIED = exact source line supports the claim; GAP = not verifiable in source (constant exists but no implementation, or behaviour absent).

All paths are relative to the repo root `~/GIT/cdc-badge-os`.

## Plugin model and runtime

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Plugins are WebAssembly modules loaded from the plugins partition; host imports them from the WASM export table | components/plugin_manager/include/plugin_manager/plugin_lifecycle.h:4-9 | VERIFIED |
| All host functions are imported by the WASM plugin from the `cdc` module | components/plugin_manager/include/plugin_manager/host_api.h:8 | VERIFIED |
| WAMR symbol table registers host functions (the `W(...)` macro entries) | components/plugin_manager/src/WamrImports.cpp:884-1126 (`-- Symbol table --`) | VERIFIED |
| Every function declared in host_api.h (213) has a matching `W(...)` registration (213); no stubs, no extras | host_api.h vs WamrImports.cpp `W("host_*"...)` set, diffed: clean bijection | VERIFIED |
| Wrapper signature notation: `i`=i32, `I`=i64, `$`=null-terminated string, `*~`=buffer+length; WAMR injects `wasm_exec_env_t` first | components/plugin_manager/src/WamrImports.cpp:29-31 | VERIFIED |
| A bare `*` pointer arg is only 1-byte-validated by WAMR; multi-byte access re-validates the full extent | components/plugin_manager/src/WamrImports.cpp:35-41 | VERIFIED |

## Repo topology / single source of truth

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| host_api.h is the canonical C ABI contract; the SDK repo consumes a mirrored copy; CI detects drift | components/plugin_manager/include/plugin_manager/host_api.h:3-6 | VERIFIED |
| This firmware repo is the canonical source; SDK copy must stay byte-identical; any surface change committed in both repos | CLAUDE.md "Repository topology" section | VERIFIED |
| Sibling SDK repo exists locally with an `sdk/` folder | ~/GIT/cdc-badge-plugins/sdk/ (directory listing) | VERIFIED |

## API level

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| HOST_API_LEVEL_MAJOR = 0 | components/plugin_manager/include/plugin_manager/host_api.h:28 | VERIFIED |
| HOST_API_LEVEL_MINOR = 7 | components/plugin_manager/include/plugin_manager/host_api.h:29 | VERIFIED |
| HOST_API_LEVEL_STR = "0.7" | components/plugin_manager/include/plugin_manager/host_api.h:30 | VERIFIED |
| HOST_API_LEVEL_PACKED = (major<<16)|minor | components/plugin_manager/include/plugin_manager/host_api.h:31 | VERIFIED |
| Load-time check: plugin `api_level_major` must equal firmware major and `api_level_minor` must be <= firmware minor, else ApiLevelMismatch | components/plugin_manager/src/CapabilityChecker.cpp:44-49 | VERIFIED |
| Plugin exports `plugin_required_api_major/minor` (REQUIRED) | components/plugin_manager/include/plugin_manager/plugin_lifecycle.h:18-19 | VERIFIED |

## Return codes (HOST_ERR_*)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| HOST_OK 0 | components/plugin_manager/include/plugin_manager/host_api.h:37 | VERIFIED |
| HOST_ERR_GENERIC -1 | components/plugin_manager/include/plugin_manager/host_api.h:38 | VERIFIED |
| HOST_ERR_INVALID_ARG -2 | components/plugin_manager/include/plugin_manager/host_api.h:39 | VERIFIED |
| HOST_ERR_NO_CAPABILITY -3 | components/plugin_manager/include/plugin_manager/host_api.h:40 | VERIFIED |
| HOST_ERR_NOT_FOUND -4 | components/plugin_manager/include/plugin_manager/host_api.h:41 | VERIFIED |
| HOST_ERR_TIMEOUT -5 | components/plugin_manager/include/plugin_manager/host_api.h:42 | VERIFIED |
| HOST_ERR_NO_MEMORY -6 | components/plugin_manager/include/plugin_manager/host_api.h:43 | VERIFIED |
| HOST_ERR_BUSY -7 | components/plugin_manager/include/plugin_manager/host_api.h:44 | VERIFIED |
| HOST_ERR_NOT_SUPPORTED -8 | components/plugin_manager/include/plugin_manager/host_api.h:45 | VERIFIED |
| HOST_ERR_RMEM_FULL -9 | components/plugin_manager/include/plugin_manager/host_api.h:46 | VERIFIED |

## Manifest schema

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Manifest is the in-memory representation of `meta.json` | components/plugin_manager/include/plugin_manager/PluginManifest.h:2-3 | VERIFIED |
| Required fields: `id`, `version`, `host_api_level_min` (empty -> reject) | components/plugin_manager/src/PluginManifest.cpp:143-147 | VERIFIED |
| `host_api_level_min` parsed as `major.minor` | components/plugin_manager/src/PluginManifest.cpp:22-30,149 | VERIFIED |
| Fields: id, version, author, icon, host_api_level_min | components/plugin_manager/src/PluginManifest.cpp:137-141 | VERIFIED |
| `linear_memory_kb` default 64, parsed from number | components/plugin_manager/include/plugin_manager/PluginManifest.h:78; components/plugin_manager/src/PluginManifest.cpp:155-158 | VERIFIED |
| linear_memory_kb validated to [16, 4096] | components/plugin_manager/src/CapabilityChecker.cpp:20-21,51-57 | VERIFIED |
| `i18n` object: default_language (default "en"), meta, strings | components/plugin_manager/src/PluginManifest.cpp:160-164 | VERIFIED |
| `capabilities` and `prerequisites` parsed | components/plugin_manager/src/PluginManifest.cpp:166-167 | VERIFIED |

## Capability declarations (boolean flags)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Boolean caps: wifi, ble, http, socket, ui_exclusive, display_lowlevel, sao, grove, pixel_strip, background, usb_cdc, prevent_sleep, vfat, autoload | components/plugin_manager/include/plugin_manager/PluginManifest.h:22-51; components/plugin_manager/src/PluginManifest.cpp:56-69 | VERIFIED |
| List caps: rmem, ecc, ble_service_uuids, message_types, gpio_pins, pwm_pins, adc_pins, i2c_bus | components/plugin_manager/include/plugin_manager/PluginManifest.h:53-62; components/plugin_manager/src/PluginManifest.cpp:93-100 | VERIFIED |
| `nvs_namespace` string | components/plugin_manager/include/plugin_manager/PluginManifest.h:63; components/plugin_manager/src/PluginManifest.cpp:71 | VERIFIED |
| `message_types`: MIME types handled; non-empty implies messaging; sending also requires `ble` | components/plugin_manager/include/plugin_manager/PluginManifest.h:56-58 | VERIFIED |
| `background` = keep ticking after leaving the view; not a boot flag | components/plugin_manager/include/plugin_manager/PluginManifest.h:31-36 | VERIFIED |
| `autoload` = start resident at boot | components/plugin_manager/include/plugin_manager/PluginManifest.h:45-51 | VERIFIED |
| `vfat` = sandboxed file access in /plugins/data/<id>/ | components/plugin_manager/include/plugin_manager/PluginManifest.h:39-44 | VERIFIED |

## Capability validation at load

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| rmem names 1..15 chars (HOST_RMEM_NAME_MAX) | components/plugin_manager/src/CapabilityChecker.cpp:15,59-65; host_api.h:249 | VERIFIED |
| ecc names 1..15 chars (HOST_ECC_NAME_MAX) | components/plugin_manager/src/CapabilityChecker.cpp:67-73; host_api.h:281 | VERIFIED |
| GPIO/PWM/ADC pins must pass blocklist + whitelist | components/plugin_manager/src/CapabilityChecker.cpp:75-106 | VERIFIED |
| I2C bus 0 reserved for internal hardware | components/plugin_manager/src/CapabilityChecker.cpp:108-115 | VERIFIED |
| ble_service_uuids validated as 128-bit lowercase UUID form | components/plugin_manager/src/CapabilityChecker.cpp:24-38 | VERIFIED |

## Runtime capability gating (per-call HOST_ERR_NO_CAPABILITY)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| BLE calls gated by `capabilities.ble` | components/plugin_manager/src/host_api_ble.cpp:120,240,302+ | VERIFIED |
| Socket calls gated by `capabilities.socket` | components/plugin_manager/src/host_api_socket.cpp:51,115,164,181 | VERIFIED |
| vFAT (host_fs_*) gated by `capabilities.vfat` | components/plugin_manager/src/host_api_fs.cpp:49 | VERIFIED |
| Low-level display gated by `capabilities.display_lowlevel` | components/plugin_manager/src/host_api_display.cpp:7,28,51+ | VERIFIED |
| Pixel strip gated by `capabilities.pixel_strip` | components/plugin_manager/src/host_api_pixel_strip.cpp:8,45,112+ | VERIFIED |
| USB CDC write gated by `capabilities.usb_cdc` | components/plugin_manager/src/host_api_usb.cpp:15,24 | VERIFIED |
| Message transfer (host_msg_*) gated by `msg_allowed()` (ble + message_types) | components/plugin_manager/src/host_api_msg.cpp:170,240,249 | VERIFIED |
| HTTP host_api functions perform no `capabilities.http` per-call check (no NO_CAPABILITY in host_api_http.cpp) | components/plugin_manager/src/host_api_http.cpp (no NO_CAPABILITY occurrence) | GAP |
| WiFi host_api functions perform no `capabilities.wifi` per-call check (host_wifi_request acquires the shared radio) | components/plugin_manager/src/host_api_wifi.cpp:34-36 (no NO_CAPABILITY occurrence) | GAP |

## Lifecycle entry points

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| REQUIRED: plugin_init (once after load), plugin_deinit (before unload), plugin_on_enter, plugin_on_exit | components/plugin_manager/include/plugin_manager/plugin_lifecycle.h:22-26 | VERIFIED |
| OPTIONAL: plugin_on_action, plugin_on_button, plugin_on_event, plugin_on_tick, plugin_on_cmd | components/plugin_manager/include/plugin_manager/plugin_lifecycle.h:28-33 | VERIFIED |
| OPTIONAL: plugin_on_prerequisite_failed (when a prerequisite uses on_fail=callback) | components/plugin_manager/include/plugin_manager/plugin_lifecycle.h:35-36 | VERIFIED |
| Missing REQUIRED export -> plugin rejected at load | components/plugin_manager/include/plugin_manager/plugin_lifecycle.h:6-9 | VERIFIED |
| plugin_on_action signature: (action_id, selected_idx, user_data) | components/plugin_manager/include/plugin_manager/plugin_lifecycle.h:29 | VERIFIED |

## Action-callback contract

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| List select: idx = selected row index, user_data = items[i].item_id | components/plugin_manager/include/plugin_manager/host_api.h:858-866 | VERIFIED |
| Context menu select: idx = selected item position (0-based), user_data = items[i].item_id | components/plugin_manager/include/plugin_manager/host_api.h:796-801 | VERIFIED |
| Confirm: user_data = 1 on Y, 0 on N (idx unused) | components/plugin_manager/include/plugin_manager/host_api.h:786-790 | VERIFIED |
| T9 / password: confirm fires user_data=1, idx=text length; cancel user_data=0; view pops itself first | components/plugin_manager/include/plugin_manager/host_api.h:804-821 | VERIFIED |
| PIN entry: confirm user_data=1, idx=PIN length; cancel user_data=0 | components/plugin_manager/include/plugin_manager/host_api.h:823-830 | VERIFIED |
| Slider/date/time: confirm user_data=1 (read via consume_input_int), cancel user_data=0 | components/plugin_manager/include/plugin_manager/host_api.h:832-856 | VERIFIED |
| Color picker: idx = packed 0xRRGGBB, user_data=1 on Y | components/plugin_manager/include/plugin_manager/host_api.h:839-845 | VERIFIED |
| EventBus action: idx = event-type bit position, user_data = payload (key events: ASCII key) | components/plugin_manager/include/plugin_manager/host_api.h:1191-1199 | VERIFIED |
| Canvas key_action: idx = focused widget id, user_data = ASCII key | components/plugin_manager/include/plugin_manager/host_api.h:972-980 | VERIFIED |

## Host API families (defgroup, per file)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Logging group | host_api.h:48-70; components/plugin_manager/src/host_api_log.cpp | VERIFIED |
| Time / RTC group | host_api.h:72-107; components/plugin_manager/src/host_api_time.cpp | VERIFIED |
| Power group (incl. host_set_sleep_inhibit) | host_api.h:109-153; components/plugin_manager/src/host_api_power.cpp | VERIFIED |
| Crypto group | host_api.h:155-223; components/plugin_manager/src/host_api_crypto.cpp | VERIFIED |
| SecureElement / TROPIC01 group (rmem + ecc) | host_api.h:225-314; components/plugin_manager/src/host_api_se.cpp | VERIFIED |
| HTTP group | host_api.h:316-364; components/plugin_manager/src/host_api_http.cpp | VERIFIED |
| Socket group | host_api.h:366-408; components/plugin_manager/src/host_api_socket.cpp | VERIFIED |
| WiFi group | host_api.h:410-461; components/plugin_manager/src/host_api_wifi.cpp | VERIFIED |
| BLE group | host_api.h:463-627; components/plugin_manager/src/host_api_ble.cpp | VERIFIED |
| NVS group | host_api.h:629-671; components/plugin_manager/src/host_api_nvs.cpp | VERIFIED |
| vFAT group | host_api.h:673-713; components/plugin_manager/src/host_api_fs.cpp | VERIFIED |
| UI Views group | host_api.h:715-956; components/plugin_manager/src/host_api_ui_views.cpp, host_api_ui.cpp | VERIFIED |
| Canvas group | host_api.h:958-1114; components/plugin_manager/src/host_api_canvas.cpp | VERIFIED |
| Low-level GFX group | host_api.h:1116-1157; components/plugin_manager/src/host_api_display.cpp | VERIFIED |
| I18n group | host_api.h:1159-1187; components/plugin_manager/src/host_api_i18n.cpp | VERIFIED |
| EventBus group | host_api.h:1189-1234; components/plugin_manager/src/host_api_event.cpp | VERIFIED |
| Keypad group | host_api.h:1236-1265; components/plugin_manager/src/host_api_keypad.cpp | VERIFIED |
| USB CDC group | host_api.h:1267-1276; components/plugin_manager/src/host_api_usb.cpp | VERIFIED |
| System Info group | host_api.h:1278-1299; components/plugin_manager/src/host_api_sysinfo.cpp | VERIFIED |
| Command channel group (host_cmd_consume) | host_api.h:1301-1318; components/plugin_manager/src/host_api_cmd.cpp | VERIFIED |
| Message transfer group | host_api.h:1320-1411; components/plugin_manager/src/host_api_msg.cpp | VERIFIED |
| Strings group (host_str_to_display/utf8) | host_api.h:1413-1456; components/plugin_manager/src/host_api_strings.cpp | VERIFIED |
| GPIO/PWM/ADC/I2C/SAO group | host_api.h:1458-1532; components/plugin_manager/src/host_api_gpio.cpp | VERIFIED |
| Pixel strip group | host_api.h:1534-1574; components/plugin_manager/src/host_api_pixel_strip.cpp | VERIFIED |
| Lockscreen group (register_action, alert) | host_api.h:1576-1610; components/plugin_manager/src/host_api_lockscreen.cpp | VERIFIED |

## UTF-8 boundary

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Whole host API speaks UTF-8; text to UI/canvas/display converted to display codepage; text read back returned as UTF-8 | components/plugin_manager/include/plugin_manager/host_api.h:1414-1424 | VERIFIED |
| Message payload bytes opaque; for text MIME types they are UTF-8 | components/plugin_manager/include/plugin_manager/host_api.h:1336-1339 | VERIFIED |
| Optional explicit converters host_str_to_display / host_str_to_utf8; CP437 default target | components/plugin_manager/include/plugin_manager/host_api.h:1428-1454 | VERIFIED |

## Build / install

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Upload via serial: `PLUGIN UPLOAD <id> <size> <crc32_hex>` (binary stream), `PLUGIN START <id>` | components/plugin_manager/src/PluginSerialCommands.cpp:558,564 | VERIFIED |
| Python upload tool exists | tools/upload.py (file present) | VERIFIED |
| Plugin storage: `<id>.wasm` + `<id>.meta` on the plugins FAT partition; upload via tools/upload.py or web installer | CLAUDE.md "Plugin System" section | VERIFIED |
