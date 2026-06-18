---
title: "Plugins: overview"
description: Sandboxed WebAssembly plugins on the CDC Badge, how they are isolated, and the capability-gated security model.
sidebar:
  order: 1
---

The CDC Badge runs third-party plugins as sandboxed WebAssembly (WASM) modules. Plugins are **not** compiled into the firmware: they are uploaded to the badge as ordinary files and executed inside an in-firmware WASM runtime, so installing or removing one never requires a reflash.

## What a plugin is

A plugin is a WASM binary plus a small JSON manifest. The runtime is the WebAssembly Micro Runtime (WAMR), pinned to version 2.4.4. It is built in Fast Interpreter mode with the ahead-of-time (AOT) loader enabled and JIT disabled. All WAMR allocations are routed through a custom allocator that lives in PSRAM, so a plugin's linear memory does not compete with the badge's scarce internal RAM.

Each plugin reaches the firmware only through the host API (the `host_*` functions). It cannot call into firmware internals, link against badge code, or touch hardware directly. Everything it is allowed to do is mediated by that API and gated by its manifest.

## Where plugins live

Plugins are stored on a dedicated FAT-FS partition named `plugins`, mounted at `/vfat`. The partition is **2 MB** (`0x200000`). It auto-formats on first boot if it is empty. Plugin files live in the `system/` subfolder (`/vfat/system/`), so the partition root stays free for user files.

A single plugin is made up of these files under `/vfat/system/`:

| File | Purpose |
| --- | --- |
| `<id>.wasm` | The WebAssembly binary (required). |
| `<id>.meta` | The JSON manifest: id, version, capabilities, resources (required). |
| `<id>.aot` | Optional AOT-compiled binary. If present it is loaded in preference to `<id>.wasm`. |
| `<id>.lang` | Optional translation overlay for the plugin's UI strings. |
| `<id>.disabled` | Marker file written when a plugin is disabled. |

A plugin is only recognised when **both** `<id>.wasm` and `<id>.meta` are present.

## Sandbox and isolation model

- **Memory isolation:** each plugin instance gets its own WASM linear memory in PSRAM. The size is set by the manifest's `linear_memory_kb` (default 64, clamped to the range 16 to 4096 KB). The operand and frame stack is a fixed 64 KB per instance.
- **API-only access:** a plugin can only call the published host API. There is no shared address space with the firmware and no direct peripheral access.
- **Capability gating:** every sensitive operation (WiFi, BLE, the secure element, GPIO, file access, and so on) is unlocked only if the manifest requests the matching capability. The firmware validates the manifest before the plugin is allowed to run.
- **Hardware safety:** GPIO, PWM, ADC, and I2C requests are checked against a fixed allow and block list, so a plugin cannot drive a pin wired to the display, the secure element, the charger, USB, PSRAM, or flash. See [What plugins can do](/power/plugins/capabilities/).
- **API-level compatibility:** the manifest declares the host API level it was built for. The firmware refuses to load a plugin whose major level differs, or whose minor level is newer than the firmware provides. This firmware exposes host API level **0.7**.

:::note
Plugins are starting points for hardware bring-up and badge customisation, not a trusted-compute boundary for secrets. They run with the capabilities you grant via the manifest, so only install plugins you trust.
:::

## Next steps

- [Installing plugins](/power/plugins/install/): the web installer, the `tools/upload.py` CLI, and the serial commands.
- [Managing plugins](/power/plugins/manage/): start, stop, disable, delete, and the background and autoload residency model.
- [What plugins can do](/power/plugins/capabilities/): the host capability families and the GPIO/I2C safety policy.
- [Host API reference](/dev/host-api/) and the [plugin SDK](/dev/plugin-sdk/) for writing your own.
