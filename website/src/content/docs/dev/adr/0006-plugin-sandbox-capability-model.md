---
title: ADR-0006 — Plugin WAMR sandbox and capability model
description: Plugins run as bounds-checked WASM; AOT native code is default-off.
sidebar:
  order: 6
---

**Status**: accepted
**Source**: spec 010 (plugin system); Constitution III; FR-071..074; `components/cdc_core/include/cdc_core/feature_flags.h` (`FEATURE_PLUGIN_AOT`)

## Context

Third-party plugins run inside the device instead of being statically linked into the firmware.
The plugin boundary is a trust boundary on a hardware security key. WAMR can execute either
interpreted WASM bytecode (bounds-checked by the runtime) or AOT-compiled native machine code
(which bypasses the WASM sandbox). The runtime validates only one byte for a bare `*` host
argument, so the host cannot rely on it to check buffer extents.

## Decision

Plugins run as WebAssembly inside a WAMR runtime with linear memory in PSRAM (16–4096 KB,
default 64 KB). The interpreter is the execution mode; AOT native code is disabled by default
(`FEATURE_PLUGIN_AOT` defaults 0), so only interpreted bytecode is loaded/accepted unless AOT is
explicitly enabled.

- Every plugin is gated by a manifest capability model, enforced at load time and per host call.
- Each `host_*` entry point validates plugin-supplied pointers/lengths against linear memory
  (`wbuf_ok` the full extent; a bare `*` is only 1-byte-checked) and checks manifest
  capabilities.
- GPIO/PWM/ADC/I2C honour a firmware hard block list (Display SPI, TROPIC01, charger, USB,
  PSRAM, flash, octal-PSRAM data lines GPIO 33–37) plus a per-pin manifest whitelist; conflicting
  claims are rejected as busy.

## Consequences

- Enables: extending the device without reflashing, while keeping plugin code inside a
  bounds-checked sandbox that cannot reach blocked hardware or out-of-bounds memory.
- Must hold: the GPIO hard block list stays blocked; AOT stays default-off; every new host
  function validates the full buffer extent and the manifest capability before acting.
- Cost: interpreted execution is slower than native AOT; the performance ceiling is the price of
  the sandbox.
