# Plugin Development Guide

Plugins are **WASM modules** loaded at runtime from the `plugins` FAT
partition and executed inside the WAMR sandbox. Unlike native modules (see
[MODULE_DEVELOPMENT.md](MODULE_DEVELOPMENT.md)), a plugin:

- requires **no firmware reflash** - it is uploaded over serial / the web
  flasher and loaded on demand;
- runs **sandboxed** - it cannot touch firmware memory, peripherals, or other
  plugins except through the host API;
- reaches the firmware only through the stable C ABI in
  `components/plugin_manager/include/plugin_manager/host_api.h`, mirrored into
  the Rust SDK in the `cdc-badge-plugins` repo.

The SDK, manifest format, and on-badge file layout are documented in the
`cdc-badge-plugins` repo. This guide covers the **firmware side**: the host
API surface, the capability model, and the security boundaries.

## Host API layering

Every host function is a thin adapter in `components/plugin_manager/src/
host_api_<family>.cpp` that:

1. checks the calling plugin's capability (manifest-declared), then
2. forwards to a HAL / core service that does the actual work.

**Rule:** code in `plugin_manager` must be *exclusively* plugin glue
(marshalling + capability gate). Anything generally useful lives in the HAL
(`cdc_hal`), core (`cdc_core`), or views (`cdc_views`) and is merely forwarded.
For example, raw I2C transfers and 24Cxx EEPROM paging live in
`cdc_hal/I2cBus`, not in the I2C host adapter.

A function is only callable by a plugin when it is **registered in
`WamrImports.cpp`** (wrapper + `NativeSymbol` entry). An import that is not
registered makes the plugin fail to instantiate.

## Capability reference

Capabilities are declared in the plugin manifest under `capabilities`. Calls
without the matching capability return `HOST_ERR_NO_CAPABILITY`.

| Manifest key | Type | Gates |
|--------------|------|-------|
| `background` | bool | `plugin_on_tick` while not on screen |
| `prevent_sleep` | bool | inhibits lock-screen light sleep while the plugin is loaded |
| `nvs_namespace` | string | NVS (always scoped to `plugin_<id>`) |
| `rmem` | string[] | named retained-memory slots (plugin pool) |
| `ecc` | string[] | named ECC keys (single reserved slot, see below) |
| `gpio_pins` / `pwm_pins` / `adc_pins` | u8[] | those GPIO/PWM/ADC pins |
| `grove` / `sao` | bool | Grove (pins 2/3) / SAO (pins 15/16, EEPROM) |
| `i2c_bus` | u8[] | I2C on the listed bus (expansion bus 1 only) |
| `pixel_strip` | bool | addressable LED strip |
| `display_lowlevel` | bool | direct framebuffer drawing |
| `wifi` / `ble` / `http` | bool | radios / outbound HTTP |
| `usb_cdc` | bool | raw writes to the USB-CDC serial console |
| `ui_exclusive` | bool | exclusive UI ownership |

## Security boundaries

The sandbox holds because dangerous surface is isolated, not because plugins
are trusted:

- **NVS** is scoped to a per-plugin namespace (`plugin_<id>`). `nvs_erase_all`
  wipes only the calling plugin's own keys, never another plugin or firmware.
- **Retained memory (rmem)** is allocated by name from a dedicated plugin pool
  (`PLG_RMEM_POOL_START..END`); plugins cannot reach firmware rmem slots.
- **GPIO** is gated by a hard block-list (`PluginGpioPolicy::BLOCKED`) covering
  the SPI bus, TROPIC01 CS, display control, charger I2C, USB, and PSRAM pins,
  plus a per-pin lock. The block-list is enforced on every path (host API and
  the serial GPIO shell).
- **I2C** exposes only the **expansion bus (1)**. Bus 0 (charger BQ25895 + IO
  expander TCA9535) is refused. The TROPIC01 secure element is on **SPI**, not
  I2C, and SPI is entirely off-limits to plugins (no SPI host API; all SPI pins
  are in the GPIO block-list).
- **USB-CDC write** shares the serial console; treat plugin output as
  untrusted when parsing the console.

## ECC slot reservation (important)

ECC slots on the TROPIC01 are **scarce** (`ECC_SLOT_COUNT = 32`) and primarily
reserved for firmware features (attestation in slot 0, WebAuthn / identity,
...). Those take priority over plugins.

Plugins therefore get a **single reserved ECC slot** - the last physical slot
(`PLG_ECC_POOL_START..END` in `host_api_se.cpp`, currently both
`ECC_SLOT_COUNT - 1`). Plugins address ECC keys **by name** (manifest
`capabilities.ecc`), exactly like rmem; the firmware maps each name to a pool
slot and persists the mapping in NVS (namespace `plg_ecc_map`), so a key keeps
its slot across reboot and reinstall. A slot occupied by an uninstalled
plugin's name is reclaimable; reclaiming wipes the stale key first.

**Consequence:** with a one-slot pool, only one plugin ECC key can be live at a
time. **To let multiple plugins use ECC, widen the pool in firmware** - extend
`PLG_ECC_POOL_START..END` to cover more slots, keeping them clear of any
firmware-owned slot. This is a deliberate firmware decision, never granted by a
plugin manifest alone. `ecc_allowed`-style slot escape is structurally
impossible: plugins never name a numeric slot.

## BLE for plugins

A plugin with the `ble` capability can act as a GATT **peripheral** (publish one
service) and as a **central** (scan, connect, talk to a peer). BLE always goes
through the `IBluetoothController` HAL, never NimBLE directly.

- **One reserved service slot.** `BluetoothController` keeps a pool of GATT
  service slots; the **last** is reserved for plugins
  (`PLUGIN_SERVICE_SLOT`), so system services (Nordic UART, vCard, HID, GPG)
  can never starve a plugin and a plugin can never take a system slot.
  `registerGattService(def, pluginReserved=true)` allocates only that slot.
- **Reserved-UUID blocklist.** `host_ble_register_service` rejects the
  Bluetooth SIG 16-bit range (so a plugin cannot shadow HID, Device Info,
  Battery, GAP/GATT, ...) and the system's 128-bit service UUIDs (NUS, vCard,
  GPG). Plugins must use their own random 128-bit UUIDs.
- **One connection.** `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`: central and
  peripheral share the single link.
- **Async, off the BLE task.** The NimBLE stack invokes GATT write / central
  callbacks on its own task; those never call into WASM. They copy the payload
  into a mutex-protected ring and `plg_ble_pump()` (run from the plugin tick)
  fires the plugin's `action_id`, which then pulls the payload with a
  `host_ble_consume_*` call. A plugin's BLE service is torn down automatically
  when it is unloaded (`plg_ble_on_unload`).

## Adding a host API function

1. Declare it in `host_api.h` (and mirror byte-identically into the SDK header
   in the `cdc-badge-plugins` repo - a CI job checks for drift).
2. Implement it: a thin adapter in `host_api_<family>.cpp` that gates on the
   capability and forwards to the HAL/core. If the underlying feature does not
   exist yet, add it to the HAL - not to the plugin adapter.
3. Register it in `WamrImports.cpp` (wrapper translating WASM argument types +
   a `NativeSymbol` entry with the signature string).
4. Expose a safe Rust binding in the SDK and bump nothing without instruction.

If a family is not implemented yet, its stubs live in `host_api_stubs.cpp` and
return `HOST_ERR_NOT_SUPPORTED`. When you implement a family, **move it to its
own `host_api_<family>.cpp` and delete the stub** - never leave functional code
in the stubs file.
