---
title: ADR-0008 — Single BLE controller via IBluetoothController
description: One GAP event handler; modules use the IBluetoothController API only.
sidebar:
  order: 8
---

**Status**: accepted
**Source**: Constitution I; `components/cdc_hal/include/cdc_hal/IBluetoothController.h`

## Context

NimBLE on the ESP32-S3 supports a simultaneous GATT server and client, but there is one GAP
event handler and one host task. BLE lifecycle operations (enable/disable/register/unregister/
stop) are serialized by a recursive lifecycle mutex held across the blocking NimBLE
stop/deinit teardown; NimBLE host-task callbacks must never take that mutex or they deadlock the
host task during the drain. The GATT registry is bounded: `MAX_REGISTERED_SERVICES = 7` and
`MAX_CHARS_PER_SERVICE = 6`.

A GATT service registered while the stack is already running forces a NimBLE restart to commit
it, and each sync (re)starts advertising from the current advertised-UUID set. Bringing the stack
up per module during init would therefore restart it repeatedly and advertise before every
module's service UUID is known.

## Decision

There is a single BLE controller. `BluetoothController` is the only GAP event handler. Modules
use the `IBluetoothController` API exclusively.

- BLE-using modules MUST NOT add `bt` to their CMakeLists REQUIRES and MUST NOT touch NimBLE
  directly.
- Modules MUST NOT register their own GAP event handler.
- Service/characteristic registration goes through the controller and stays within the
  `MAX_REGISTERED_SERVICES` / `MAX_CHARS_PER_SERVICE` limits.
- The controller enforces a startup barrier: during boot `enable()` only records the request and
  defers the bring-up. `notifySystemReady()` is called once after module and plugin
  initialization; it brings the stack up a single time so all services registered during boot
  commit together and advertising starts once with the complete UUID set. Modules MUST NOT assume
  BLE is enabled during their `init()`; GATT registration while BLE is down is allowed and
  committed on enable.

## Consequences

- Enables: HID, serial, message-transfer, and cross-signing services to coexist on one stack
  without competing GAP handlers, and a single place that owns the deadlock-sensitive teardown
  ordering.
- Must hold: lifecycle operations stay serialized under the lifecycle mutex; host-task callbacks
  stay mutex-free; teardown follows the documented adv-stop → settle → drain → deinit order.
- Must hold: BLE is brought up once via `notifySystemReady()` after all modules register their
  services and advertising UUIDs, independent of module init order, so the beacon's service UUID
  is always in the first advertising start.
- Cost: a fixed cap on simultaneously registered GATT services/characteristics; features must
  fit within the registry limits.
