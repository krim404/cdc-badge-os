---
title: ADR-0001 — PSRAM-first memory model
description: Internal SRAM is reserved; PSRAM is the default allocation pool.
sidebar:
  order: 1
---

**Status**: accepted
**Source**: Constitution II (Memory Discipline: PSRAM-First); NFR-001

## Context

The ESP32-S3 build has very tight internal SRAM, while 8 MB of Octal PSRAM at 80 MHz is
plentiful. Internal RAM is the binding constraint: exhausting it bricks the device. The main
task stack alone is 24576 bytes. BLE/WiFi internal buffers, ISR-touched data, and task stacks
must live in internal RAM; everything else can live in PSRAM.

## Decision

PSRAM is the default allocation pool. Every new buffer, cache, vector, or growing allocation
targets PSRAM unless it is performance- or interrupt-critical.

- Internal RAM is reserved ONLY for: task stacks, ISR-touched data, BLE/WiFi-internal buffers,
  and small static state (< 256 bytes).
- Runtime buffers use `cdc::core::psramAlloc<T>(n)`; large static buffers use `EXT_RAM_BSS_ATTR`;
  heap allocations > 4 KB use `MALLOC_CAP_SPIRAM`.
- No raw `new T[]`: use `PsramUniquePtr<T>` or a fixed `std::array<T, N>`.
- No `std::vector` / `std::map` / `std::string` in the public APIs of `cdc_core`, `cdc_hal`, or
  `cdc_ui`. Modules MAY use them locally in PSRAM-allocated scope.

## Consequences

- Enables: large i18n tables, plugin linear memory, message-transfer reassembly buffers, and
  module-local containers without starving the scarce internal pool.
- Must hold: any reviewer can reject an internal-RAM allocation that is not a stack, ISR datum,
  radio buffer, or < 256-byte static. New features are planned PSRAM-first by default.
- Cost: PSRAM access is slower than internal SRAM, so the few interrupt- and
  performance-critical paths must be explicitly kept in internal RAM.
