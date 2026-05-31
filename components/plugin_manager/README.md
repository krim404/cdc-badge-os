# plugin_manager

Loads WebAssembly plugins from the `/plugins` FAT-FS partition, validates their manifests, and routes the host API surface they import from. Sits between the WAMR runtime (`components/wamr_runtime`) and the rest of the firmware.

## Files

- `include/plugin_manager/host_api.h` - canonical host API header. The SDK header in `cdc-badge-plugins` is a byte-identical mirror; CI in both repos checks for drift.
- `include/plugin_manager/plugin_lifecycle.h` - declarations of the lifecycle functions a plugin can export.
- `PluginManager` - discovery and lifecycle. Runs one foreground plugin plus resident background plugins (see `background` / `autoload` capabilities).
- `PluginStorage` - VFS mount of the `plugins` partition; on-device files are `<id>.wasm` (or `<id>.aot`) plus `<id>.meta`.
- `PluginManifest` - JSON parser for the `<id>.meta` manifest.
- `CapabilityChecker` - load-time capability and resource validation.
- `host_api_<family>.cpp` - host API implementations grouped by family (log, time, power, crypto, se, http, wifi, ble, nvs, ui, ui_views, i18n, event, gpio, fs, display, canvas, pixel_strip, keypad, cmd, usb, strings, sysinfo). Registered with WAMR under module `"cdc"`.
