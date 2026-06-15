---
title: Architecture
description: The CDC Badge OS component model, the core services, hardware abstraction, the UI stack, messaging and plugins, plus the module isolation rules and the IModule lifecycle.
sidebar:
  order: 1
---

CDC Badge OS is organized as a set of ESP-IDF components with a strict layering: hardware sits behind interfaces in `cdc_hal`, shared services live in `cdc_core`, the UI framework builds on top, and self-contained feature modules plug in without the core ever referencing them.

## Core: `cdc_core`

`cdc_core` holds the cross-cutting services that everything else depends on.

### Service lifecycle (`IService`)

Every long-lived component (HAL drivers and modules alike) implements `IService` (`cdc_core/IService.h`). The contract is four methods plus identity:

- `init()`: one-time initialization, called once during boot.
- `start()`: begin operation; callable after `init()` or `stop()`.
- `stop()`: reversible pause and cleanup.
- `getState()`: returns the current `ServiceState`.
- `getName()`: a name for logging.

`ServiceState` moves through `UNINITIALIZED`, `INITIALIZED`, `STARTED`, `STOPPED`, and `ERROR`. The constructor is meant to be lightweight and must not touch hardware.

### ServiceRegistry

`ServiceRegistry` (`cdc_core/ServiceRegistry.h`) is a statically allocated service locator (no heap, `MAX_SERVICES = 24`). It is a Meyer's singleton and offers two access patterns:

- Named services: `registerService("display", &display)` and `get<IDisplay>("display")`, used for the core HAL services.
- Typed services: `provide<T>(ServiceType, &impl)` and `request<T>(ServiceType)` for optional inter-module dependencies. `ServiceType` currently enumerates `KEYBOARD`, `CHALLENGE_RESPONDER`, `CLIPBOARD`, and `NOTIFICATION`.

It also drives bulk lifecycle: `initAll()`, `startAll()`, and `stopAll()` (the last in reverse order).

### EventBus

`EventBus` (`cdc_core/EventBus.h`) is a publish/subscribe bus backed by a FreeRTOS queue (`MAX_HANDLERS = 16`, default queue size 32, no heap). Publishing is ISR-safe (`publish(event, fromISR)`); handlers are not invoked at publish time but drained from the main loop by `process()`. Subscribers pass a bitmask of `EventType` values (0 means all). Event categories include input (`KEY_PRESSED`, ...), power (`POWER_USB_CONNECTED`, ...), system (`SYSTEM_LOCK`, `SYSTEM_SLEEP`, ...), Bluetooth (`BLE_CONNECTED`, `BLE_CONSENT_REQUEST`, ...), a timer tick, and generic `MODULE_EVENT` / `MODULE_ERROR`.

### ModuleRegistry

`ModuleRegistry` (`cdc_core/ModuleRegistry.h`) manages all feature modules (`MAX_MODULES = 16`). Responsibilities:

- Registration and lifecycle (`registerModule`, `initAll`, `startAll`, `startModule`, `stopAll`).
- Module enable/disable that persists across reboot. Disabled modules are stored in NVS as a comma-separated list of module names, so the setting is robust against module ordering changes.
- Menu item collection from modules, filtered by `MenuLocation` and sorted by priority, plus lock-screen context items.
- Event fan-out to modules: `dispatchUnlock`, `dispatchLock`, `dispatchUsbConnect`, `dispatchUsbDisconnect`, `dispatchTick`.
- Slot-map validation and per-module error reporting (`reportModuleError`, `retryModule`, `getModuleStatusLabel`).
- NVS cleanup for modules that no longer exist (`cleanupOrphanedModuleData`). The module NVS namespace prefix is `mod_`.

### PinManager

`PinManager` (`cdc_core/PinManager.h`) manages every device PIN in TROPIC01 R-Memory slot 0, with the payload covered by a slot-0 attestation signature so tampering forces a reset to defaults. It distinguishes a Badge/FIDO2 PIN (RAM-only retry counter with a recovery timer, so a crash mid-verify cannot brick the badge) from OpenPGP PW1/PW3, which use smartcard semantics where reaching zero retries is terminal until an admin reset.

:::caution[Slot byte size unverified]
The secure-element interface documents R-Memory as 512 slots; the exact byte size per slot is not asserted here because two in-tree sources disagree on it. Treat `main/tropic_slot_map.h` as authoritative for the slot layout.
:::

## Hardware abstraction: `cdc_hal`

`cdc_hal` exposes hardware only through pure-virtual interfaces, each extending `core::IService`. Drivers implement them; the rest of the firmware depends on the interface, never the driver. The interfaces are:

| Interface | Hardware / role |
| --- | --- |
| `IDisplay` | E-paper display. `RefreshMode` is `FULL`, `PARTIAL`, or `PARTIAL_LIGHT` (the last is for tiny low-churn updates such as the lock-screen clock and is never promoted to a full refresh). |
| `IKeypad` | Button input, with a `KeyCallback` for key events. |
| `IPowerManager` | Battery and charger (BQ25895); `PowerSource` is `BATTERY`, `USB`, or `UNKNOWN`. |
| `ISecureElement` | TROPIC01: ECC key storage (32 slots), ECDSA/EdDSA signing, R-Memory (512 slots), hardware TRNG. |
| `IBluetoothController` | BLE GAP/GATT controller. |
| `IWifiController` | WiFi stack initialization and connection management. |
| `ISleepController` | Light sleep (CPU paused, fast wake) and deep sleep (full power down, reset on wake). |
| `IRtc` | ESP32-S3 internal RTC; time survives light sleep but not deep sleep or power cycles. |
| `II2cBus`, `ISpiBus`, `IEspHardware` | Shared buses and ESP-specific hardware helpers. |

## UI framework: `cdc_ui`

`cdc_ui` is the UI core that everything above the HAL builds on.

- `IView` (`cdc_ui/IView.h`): the base for all screens. Each view has lifecycle hooks (`onEnter`, `onExit`, `onResume`, `onPause`) and returns an `InputResult` (`CONSUMED`, `IGNORED`, `REQUEST_POP`, `REQUEST_PUSH`) from input handling.
- `ViewStack` (`cdc_ui/ViewStack.h`): a singleton navigation stack (`MAX_DEPTH = 20`) supporting `push`, `pop`, `replace`, and modal overlays.
- `I18n` (`cdc_ui/I18n.h`): internationalization. English fallbacks are registered in code as `I18nEntry` tables (in rodata, always available); other languages live as flat `/plugins/i18n/lang_<code>.json` files parsed into a PSRAM-backed table at runtime. Lookup falls back to English, then to `?<key>` if even English is missing.

## Reusable views: `cdc_views`

`cdc_views` provides reusable, self-contained view components built on `IView`: list, slider, PIN entry, T9 text input, QR code, confirm, context menu, color picker, toast, and others. Modules and the OS UI compose these rather than drawing screens by hand.

## OS UI: `cdc_os_ui`

`cdc_os_ui` is the OS-level UI: lock screen, settings, sleep management, backup, and the top-level app loop (`AppUi`). `ui_init(const UiDeps&)` wires it to the HAL through a `UiDeps` struct holding pointers to `IDisplay`, `IKeypad`, `IPowerManager`, `ISleepController`, and `ISecureElement`; `ui_process(nowMs)` runs the per-loop input, timer, and render work; `ui_on_modules_ready()` rebuilds menus once modules have registered.

## Serial and USB

- `serial_cmd`: `SerialCmd` is the serial command processor (input buffering and line editing, command dispatch via `ICommandRegistry`, optional authentication, special input modes). `Console` is the printf-style I/O abstraction that works over USB CDC, UART, or BLE UART.
- `usb_badge`: `usb_cdc` brings up the TinyUSB CDC serial interface; `usb_hid` builds the composite device where modules contribute HID/CCID interfaces. The USB interface descriptor types are owned by `cdc_core/UsbManager.h` and re-exported for the C-style apply API.

## Messaging: `cdc_msg`

`cdc_msg` is a generic badge-to-badge BLE message transfer service. `MessageTransfer` is a headless core (no UI dependency) that sends a typed payload (MIME type plus bytes) to a nearby badge over a MIME-typed GATT profile, with the link encrypted by an ephemeral numeric-comparison pairing that is forgotten after the transfer. `MessageHandlerRegistry` maps MIME types to delivery callbacks (`DeliverFn`) and the consent description shown to the user; it is fixed-size with no dynamic allocation. Received payloads are treated as untrusted and must be validated by the handler.

## Plugins: `plugin_manager`

`plugin_manager` discovers, loads, runs, and unloads sandboxed WASM plugins on top of the `wamr_runtime` component. At most one foreground plugin runs at a time; a plugin can declare `background` (keeps running and ticking after the user leaves its view) or `autoload` (loaded into the background at boot). Plugins are stored on the `plugins` FAT partition, mounted at `/plugins`. The host API they call is documented in [Host API](/api/).

## Module isolation and the IModule lifecycle

Modules are self-contained features. The architectural rule is one-directional: `cdc_core` (and the rest of the core) never references any `mod_*` component. A scan of `cdc_core` for module names returns nothing, which keeps modules independently removable.

### IModule

A module implements `IModule` (`cdc_core/IModule.h`), which extends `IService` and adds the module-facing surface:

- `getVersion()` returns the module version string.
- Lifecycle hooks called by `ModuleRegistry`: `onUnlock`, `onLock`, `onUsbConnect`, `onUsbDisconnect`, and `onTick(nowMs)` for periodic background work.
- UI contribution: `getMenuItems(items, max)` returns `ModuleMenuItem` entries tagged with a `MenuLocation` (`MAIN_MENU`, `TOOLS_MENU`, `SETTINGS_MENU`, `BLUETOOTH_MENU`, `WIFI_MENU`, `EXPERT_MENU`); `getEntryView()` returns the main view; `getLockScreenContextItems` adds lock-screen actions.
- Backup: `exportBackup(cJSON*)` writes semantic records (never raw slot or NVS blobs) and `importBackup(const cJSON*)` restores them best-effort, returning a `BackupResult` tally. Both default to no-ops, so a module that does not implement them is simply absent from the backup.
- Secure-element slots: `getSlotRequest()` declares ECC and R-Memory needs from the compile-time memory map, and `setSlotRange()` receives the assigned range.

### ModuleBase

Most modules derive from `ModuleBase` (`cdc_core/ModuleBase.h`) rather than `IModule` directly. It stores the name and state and provides the standard `start()` / `stop()` transitions (`start()` is valid only from `INITIALIZED` or `STOPPED`), so a concrete module typically implements only `init()` and its feature logic.

To add a module, see [Build and flash](/dev/build-system/): the only registration point is the MODULES list in `main/CMakeLists.txt`.
