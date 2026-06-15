---
title: ADR-0002 — Module isolation and single-point registration
description: Modules are self-contained and added at one place in the build.
sidebar:
  order: 2
---

**Status**: accepted
**Source**: Constitution I (Module Isolation & Self-Containment); NFR-007

## Context

The firmware is composed of feature modules (`mod_fido2`, `mod_2fa`, `mod_gpg`, `mod_vcard`,
…) layered on core components (`cdc_core`, `cdc_hal`, `cdc_ui`, `cdc_views`). On a constrained
device, features must be droppable to save RAM and flash, and the core must not accrete
feature debt. The build system generates the registration glue from the `main/CMakeLists.txt`
MODULES list; each module exports `mod_<name>_register()`.

## Decision

Modules are completely isolated and self-contained. A module owns its views, its i18n strings
(registered dynamically), its NVS namespace, and its helper classes. Adding a module requires
only creating `components/mod_<name>/`, implementing `mod_<name>_register()`, and adding one
entry to the `main/CMakeLists.txt` MODULES list.

- Core (`cdc_core`, `cdc_hal`, `cdc_ui`, `cdc_views`) MUST NOT reference any module.
- Deleting a module folder MUST NOT break the build of core or other modules.
- BLE-using modules MUST use `IBluetoothController` only; they MUST NOT add `bt` to their
  CMakeLists REQUIRES and MUST NOT touch NimBLE directly (see ADR-0008).

## Consequences

- Enables: composability, per-build feature selection to fit RAM/flash, and an auditable core
  that does not depend on optional features.
- Must hold: no cross-reference from core to a module; module-specific UI, strings, and storage
  stay inside the module; the single registration point is the only place a module is wired in.
- Cost: shared functionality must be promoted into a core component rather than reached for
  across modules.
