---
title: Developer overview
description: How CDC Badge OS is built, its component layering, the repository topology, and where to go next.
sidebar:
  order: 0
---

CDC Badge OS is firmware for the CDC Badge v1.0 hardware security key. This section documents the firmware internals for developers: the component model, the build and flash workflow, and the host API that third-party plugins call into.

## What the firmware is built on

The firmware targets the ESP32-S3 and is built with the ESP-IDF framework, driven through PlatformIO.

- Framework: ESP-IDF (`framework = espidf`), via the `espressif32` platform.
- Build environment: `cdc_badge_usb`, board `cdc-badge-usb`, 16 MB flash, PSRAM enabled.
- Language standard: C++17 (`-std=gnu++17`).

These are defined in `platformio.ini`. See [Build and flash](/dev/build-system/) for the full command set and partition layout.

## Component layering at a glance

The firmware is split into ESP-IDF components. Lower layers know nothing about the layers above them, and the core components never reference feature modules.

| Layer | Component(s) | Responsibility |
| --- | --- | --- |
| Application | `main/` | Boot sequence, service wiring, module registration |
| OS UI | `cdc_os_ui` | Lock screen, settings, top-level app UI (`AppUi`) |
| Reusable views | `cdc_views` | List, slider, PIN entry, T9, QR, and other view components |
| UI framework | `cdc_ui` | `IView`, `ViewStack`, `I18n` |
| Feature modules | `mod_*` | Self-contained features (FIDO2, 2FA, password, GPG, and more) |
| Messaging | `cdc_msg` | Generic badge-to-badge BLE message transfer |
| Plugins | `plugin_manager`, `wamr_runtime` | Sandboxed WASM plugin runtime and host API |
| Serial and USB | `serial_cmd`, `usb_badge` | Serial command console and USB CDC/HID |
| Hardware abstraction | `cdc_hal` | Interfaces for display, keypad, power, secure element, BLE, WiFi, sleep, RTC, buses |
| Core | `cdc_core` | `ServiceRegistry`, `EventBus`, `ModuleRegistry`, `PinManager`, `IModule` |

The full responsibility breakdown, with the interface that defines each piece, is in [Architecture](/dev/architecture/).

## Repository topology

This repository is the canonical source of the plugin host API. The Rust plugin SDK lives in a separate sibling repository that consumes a copy of the host API header.

| Repo | Purpose | Host API role |
| --- | --- | --- |
| cdc-badge-os (this) | Firmware and canonical `host_api.h` | Source of truth |
| cdc-badge-plugins | Rust plugin SDK, examples, web installer | Consumes a copy of `host_api.h` |
| cdc-badge-development | Plugin dev environment + [off-device emulator](/dev/plugin-emulator/) | Compiles the firmware host-API surface read-only |

The canonical header is `components/plugin_manager/include/plugin_manager/host_api.h`. The SDK copy must stay byte-identical to it, so any change to the host API surface (signatures, error codes, level constants) has to be committed in both repositories together. The firmware repository's `upstream` remote is `ssh://git@codeberg.org/Krim/cdc-badge-os.git`.

## Where to go next

- [Architecture](/dev/architecture/): the component model, module isolation, and the `IModule` lifecycle.
- [Build and flash](/dev/build-system/): the PlatformIO build, the MODULES-list mechanism, partition layout, and the memory model.
- [Host API](/api/): the C ABI that plugins call into.
- [Plugin emulator](/dev/plugin-emulator/): run and regression-test plugins on a desktop, no badge needed.
- [Architecture Decision Records](/dev/adr/): the load-bearing technical decisions (PSRAM-first, module isolation, slot map, the two-KDF PIN hashing, plugin sandbox, and more).

## Testing

Host-side unit tests for portable logic (CRC, base64, CP437, message framing, CBOR) run via the
PlatformIO `native` environment with `pio test -e native` (no hardware, runs in CI). On-device tests
verify the shipped firmware on a real badge (flashed once, states reached at runtime), split into a
fully automatic serial-driven catalog and an operator-assisted one; both are optional. See
[Testing](/dev/testing/) for the full set and how to run it.
