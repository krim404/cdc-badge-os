---
title: Plugin SDK & manifest
description: The CDC Badge plugin model, repository topology, manifest schema, capability declarations and lifecycle entry points.
sidebar:
  order: 6
---

A CDC Badge plugin is a sandboxed WebAssembly module. The badge loads it from
the plugins partition and links it against a fixed host API. This page covers
the plugin model, where the canonical API lives, the manifest schema, the
capability flags and the lifecycle exports a plugin implements.

For the full per-function host API surface, see the [Host API reference](/dev/host-api/).
To build and flash plugins onto a badge, see [Installing plugins](/power/plugins/install/).

## The plugin model

Plugins run inside a WAMR (WebAssembly Micro Runtime) interpreter, not as
statically linked firmware. Each plugin is a WASM module whose exports the host
calls (lifecycle entry points) and whose imports come from a single host module
named `cdc`. All host functions a plugin can call are imported from that module
(`host_api.h:8`).

The bridge between WASM and firmware is a native symbol table in
`components/plugin_manager/src/WamrImports.cpp`. Each row uses a `W(...)` macro
that maps a host function name to its wrapper and a type signature
(`WamrImports.cpp:884-1126`). The signature notation is:

| Token | Meaning |
| --- | --- |
| `i` | 32-bit integer |
| `I` | 64-bit integer |
| `$` | NUL-terminated string |
| `*~` | buffer pointer followed by its length |
| `*` | bare pointer (see note below) |

WAMR injects the `wasm_exec_env_t` as the first C argument automatically
(`WamrImports.cpp:29-31`).

:::caution[Pointer validation]
A bare `*` pointer argument is only 1-byte-validated by WAMR. Any host call
that reads or writes more than one byte through such a pointer re-validates the
full extent against the calling instance's linear memory; otherwise a plugin
could drive an out-of-bounds access into native heap (`WamrImports.cpp:35-41`).
:::

## Repository topology

The firmware repository is the canonical source of the host API contract. The
header `components/plugin_manager/include/plugin_manager/host_api.h` is the C ABI
contract (`host_api.h:3-6`). The sibling Rust SDK repository
(`cdc-badge-plugins`) consumes a copy of that header under its `sdk/` folder.

| Repository | Role | Host API |
| --- | --- | --- |
| `cdc-badge-os` (this repo) | Firmware + canonical `host_api.h` | source of truth |
| `cdc-badge-plugins` | Rust SDK, examples, web installer | mirrored copy in `sdk/` |

The SDK copy must stay byte-identical to this repo's `host_api.h`. A CI job in
both repositories detects drift between the two copies (`host_api.h:5-6`). Any
change to the host API surface (signatures, error codes, level constants) has to
be committed in both repositories together, or existing plugins break.

## Manifest schema

Every plugin ships a `meta.json` parsed into a `PluginManifest`
(`PluginManifest.h:2-3`). The parser is in
`components/plugin_manager/src/PluginManifest.cpp`.

### Top-level fields

| Field | Type | Notes |
| --- | --- | --- |
| `id` | string | Required (`PluginManifest.cpp:143-147`) |
| `version` | string | Required |
| `host_api_level_min` | string `"major.minor"` | Required; parsed into major/minor (`PluginManifest.cpp:22-30,149`) |
| `author` | string | Optional (`PluginManifest.cpp:137-141`) |
| `icon` | string | Optional |
| `linear_memory_kb` | number | Default 64; validated to the range 16..4096 (`PluginManifest.h:78`, `CapabilityChecker.cpp:20-21,51-57`) |
| `i18n` | object | `default_language` (default `"en"`), `meta`, `strings` (`PluginManifest.cpp:160-164`) |
| `capabilities` | object | See below (`PluginManifest.cpp:166`) |
| `prerequisites` | object | Named prerequisite specs, each with an `on_fail` default of `"abort"` (`PluginManifest.cpp:103-127,167`) |

A manifest with an empty `id`, `version` or `host_api_level_min` is rejected
(`PluginManifest.cpp:143-147`).

### API level matching

At load time the firmware checks the requested level against its own. The
plugin's major version must equal the firmware major, and the plugin's minor
must be less than or equal to the firmware minor; otherwise the load fails with
an API-level mismatch (`CapabilityChecker.cpp:44-49`). The current firmware
level is read from `host_api.h` (see the [Host API reference](/dev/host-api/)).

## Capability declarations

Capabilities are declared under `capabilities` in `meta.json`. Boolean flags
opt into a subsystem; list flags reserve named resources.

### Boolean capabilities

These map one-to-one to `PluginCapabilities` (`PluginManifest.h:22-51`,
`PluginManifest.cpp:56-69`):

| Flag | Effect |
| --- | --- |
| `wifi` | Use the WiFi family |
| `ble` | Use the BLE family |
| `http` | Use the HTTP family |
| `socket` | Use the TCP/UDP socket family |
| `ui_exclusive` | Intend to acquire the exclusive UI lock |
| `display_lowlevel` | Use the low-level framebuffer GFX family |
| `sao` | Use the SAO add-on (EEPROM/pins) |
| `grove` | Use the Grove connector pins |
| `pixel_strip` | Drive an addressable LED strip |
| `background` | Keep ticking after the user leaves the view; not a boot flag (`PluginManifest.h:31-36`) |
| `usb_cdc` | Write to the USB-CDC stream |
| `prevent_sleep` | Hold a sleep inhibitor while loaded |
| `vfat` | Sandboxed file access in `/plugins/data/<id>/` (`PluginManifest.h:39-44`) |
| `autoload` | Start the plugin as a resident background instance at boot (`PluginManifest.h:45-51`) |

`background` and `autoload` are orthogonal: `autoload` governs loading at boot,
`background` governs survival after the user leaves the view.

### List capabilities

These reserve named or numbered resources (`PluginManifest.h:53-62`,
`PluginManifest.cpp:93-100`):

| Flag | Type | Notes |
| --- | --- | --- |
| `rmem` | string array | Named retained-memory slots; each name 1..15 chars (`CapabilityChecker.cpp:59-65`) |
| `ecc` | string array | Named ECC key slots; each name 1..15 chars (`CapabilityChecker.cpp:67-73`) |
| `ble_service_uuids` | string array | 128-bit lowercase UUIDs, validated at load (`CapabilityChecker.cpp:24-38`) |
| `message_types` | string array | MIME types this plugin can receive; a non-empty list implies messaging, and sending also requires `ble` (`PluginManifest.h:56-58`) |
| `gpio_pins` | number array | Must pass the GPIO blocklist and whitelist (`CapabilityChecker.cpp:75-84`) |
| `pwm_pins` | number array | Same pin policy as `gpio_pins` (`CapabilityChecker.cpp:86-95`) |
| `adc_pins` | number array | Same pin policy as `gpio_pins` (`CapabilityChecker.cpp:97-106`) |
| `i2c_bus` | number array | Bus 0 is reserved for internal hardware (`CapabilityChecker.cpp:108-115`) |
| `nvs_namespace` | string | Length-bounded NVS namespace (`PluginManifest.cpp:71`) |

### How capabilities are enforced

There are two enforcement points.

1. **Load time.** `CapabilityChecker::validate` rejects bad API levels, an
   out-of-range `linear_memory_kb`, over-long `rmem`/`ecc` names, blocked or
   non-whitelisted GPIO/PWM/ADC pins, I2C bus 0, malformed BLE UUIDs and an
   over-long NVS namespace (`CapabilityChecker.cpp:42-120`).
2. **Per call.** Some families return `HOST_ERR_NO_CAPABILITY` on every call
   when the matching flag is missing: BLE (`host_api_ble.cpp:120,240`), socket
   (`host_api_socket.cpp:51,115`), vFAT (`host_api_fs.cpp:49`), low-level
   display (`host_api_display.cpp:28,51`), pixel strip
   (`host_api_pixel_strip.cpp:45,112`), USB CDC (`host_api_usb.cpp:15,24`) and
   message transfer (`host_api_msg.cpp:170,240`).

:::note[HTTP and WiFi gating]
The HTTP and WiFi host functions do not perform a per-call
`HOST_ERR_NO_CAPABILITY` check in their implementation files; `host_wifi_request`
acquires the shared radio handle directly (`host_api_wifi.cpp:34-36`). Declare
`http` / `wifi` in the manifest as the documented contract, but do not rely on a
runtime capability rejection for those two families.
:::

## Lifecycle entry points

A plugin implements the exports in
`components/plugin_manager/include/plugin_manager/plugin_lifecycle.h`. The host
imports these from the WASM export table; a missing REQUIRED export causes the
plugin to be rejected at load (`plugin_lifecycle.h:6-9`).

### Required

| Export | When it fires |
| --- | --- |
| `plugin_required_api_major()` / `plugin_required_api_minor()` | Read at load to match the API level (`plugin_lifecycle.h:18-19`) |
| `plugin_init()` | Once after load (`plugin_lifecycle.h:23`) |
| `plugin_deinit()` | Before unload (`plugin_lifecycle.h:24`) |
| `plugin_on_enter()` | The user opens the plugin (`plugin_lifecycle.h:25`) |
| `plugin_on_exit()` | The user leaves the plugin (`plugin_lifecycle.h:26`) |

### Optional

Omit any of these if the plugin does not need them
(`plugin_lifecycle.h:28-36`):

| Export | Purpose |
| --- | --- |
| `plugin_on_action(action_id, selected_idx, user_data)` | A UI view, event subscription or message handler fired an action |
| `plugin_on_button(button_code)` | A button event |
| `plugin_on_event(event_type, event_value)` | A subscribed system event |
| `plugin_on_tick(uptime_ms)` | Periodic tick |
| `plugin_on_cmd(len)` | The host pushed a command; pull it with `host_cmd_consume` |
| `plugin_on_prerequisite_failed(prereq_id, error_code)` | A prerequisite with `on_fail=callback` failed |

## The action-callback contract

Most interactive host calls take an `action_id`. When the corresponding event
happens, the host calls `plugin_on_action(action_id, selected_idx, user_data)`.
The meaning of `selected_idx` (often written `idx`) and `user_data` depends on
the view:

| View / source | `idx` | `user_data` |
| --- | --- | --- |
| List select (`host_ui_push_list`) | selected row index | `items[i].item_id` (`host_api.h:858-866`) |
| Context menu (`host_ui_push_context_menu`) | selected item position, 0-based | `items[i].item_id` (`host_api.h:796-801`) |
| Confirm (`host_ui_push_confirm`) | unused | 1 on Y, 0 on N (`host_api.h:786-790`) |
| T9 / password input | text length on confirm, 0 on cancel | 1 on confirm, 0 on cancel (`host_api.h:804-821`) |
| PIN entry | PIN length on confirm | 1 on confirm, 0 on cancel (`host_api.h:823-830`) |
| Slider / date / time | n/a | 1 on confirm (read via `host_ui_consume_input_int`), 0 on cancel (`host_api.h:832-856`) |
| Color picker | packed `0xRRGGBB` | 1 on Y (`host_api.h:839-845`) |
| EventBus subscription | event-type bit position | event payload; for key events the ASCII key code (`host_api.h:1191-1199`) |
| Canvas key callback | focused widget id | the ASCII key code (`host_api.h:972-980`) |

:::tip[idx is on-screen position, user_data is your id]
For lists and menus, `idx` is the on-screen row position and `user_data` is the
`item_id` the plugin set in each `ui_item_t`. Bind list commands to `user_data`,
never to `idx`: the displayed order may differ from the plugin's storage order
(`host_api.h:858-866`).
:::

The input views that fire on both confirm and cancel pop themselves before the
action fires; read committed text with `host_ui_consume_input_text` and integers
with `host_ui_consume_input_int` (`host_api.h:804-856`).

## Building and installing

Plugins are stored on the plugins FAT partition as a `<id>.wasm` payload plus a
`<id>.meta` manifest. Upload them over the serial console with
`PLUGIN UPLOAD <id> <size> <crc32_hex>` followed by the binary stream, then run
`PLUGIN START <id>` (`PluginSerialCommands.cpp:558,564`). The repository ships a
Python helper at `tools/upload.py`, and the SDK repository provides a web
installer.

See [Installing plugins](/power/plugins/install/) for the end-to-end build and
flash workflow.
