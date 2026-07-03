---
title: ADR-0013 — Off-device plugin emulator re-hosts firmware sources
description: The emulator reuses the firmware's WAMR runtime, host-API bodies and drawing stack on the desktop, behind host HAL-factory backends.
sidebar:
  order: 13
---

**Status**: accepted
**Source**: spec 022 (off-device plugin WASM emulator); Constitution II/V

## Context

Plugin developers need to run and regression-test `.wasm` plugins without badge
hardware. A plugin only ever observes the 225-symbol `"cdc"` host API; frames
must be pixel-identical to the e-paper and runtime semantics identical to the
badge's WAMR interpreter. Full-system emulation (QEMU/Wokwi) models none of
this badge's peripherals, and a rewrite in another language/runtime (e.g.
Rust + wasmtime) would duplicate the drawing logic and diverge from WAMR
semantics.

## Decision

The emulator is a **C++ host CMake target in `cdc-badge-development`** that
**re-hosts read-only vendored `cdc-badge-os` sources**: the WAMR runtime with
the device's interpreter flags, `WamrImports.cpp` with the in-scope
`host_api_*.cpp` bodies, the `Adafruit_GFX`/CalEPD/`Gdey029T94` drawing core,
and the `cdc_ui`/`cdc_views`/`PluginUiState` view stack. Hardware is replaced
at the **HAL factory boundary** (`getDisplayInstance()` etc.) by host
backends; `cdc_hal`'s ESP32 implementations are never compiled. A ~15-header
ESP-IDF shim (`freertos` -> std mutexes, `esp_timer` -> virtual clock, `nvs`
-> file KV, no-op drivers) closes the remaining gap. The `EpdSpi` transport
stub captures the panel byte stream, so captured frames equal the physical
transfer. `cdc-badge-os` is consumed, never patched.

## Consequences

- Pixel-identical frames and same-runtime behaviour by construction; plugin
  regressions are catchable in CI via deterministic frame hashes.
- The firmware's plugin-facing surface is compiled on desktop, so host-API
  changes surface as emulator build breaks - an early warning, but also a
  maintenance coupling for `cdc-badge-development`.
- Out-of-scope families (BLE, GPIO/PWM/ADC/I2C/SAO, pixel strip, USB CDC,
  badge-to-badge `msg`) stay clean stubs with a defined "unavailable" error;
  residency flags (`background`/`autoload`/`prevent_sleep`) are not emulated.
- The software secure element is dev-only (plaintext files, no irreversible
  TROPIC01 operations) and must never be represented as providing security.
