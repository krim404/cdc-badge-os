---
title: ADR-0007 — host_api.h canonical, SDK byte-mirror, versioned
description: One canonical host API header, mirrored byte-identically into the plugin SDK.
sidebar:
  order: 7
---

**Status**: accepted
**Source**: spec 010 (plugin system); Constitution V; FR-074; `components/plugin_manager/include/plugin_manager/host_api.h`

## Context

The firmware exposes a host API (60+ `host_*` functions under WAMR module `"cdc"`) that plugins
call. The Rust plugin SDK in the sibling `cdc-badge-plugins` repo consumes a copy of the same
header. Any divergence in signatures, error codes, or level constants breaks existing user
plugins. The header carries a packed version: `HOST_API_LEVEL_MAJOR` (currently 0),
`HOST_API_LEVEL_MINOR` (currently 7), `HOST_API_LEVEL_STR` ("0.7").

## Decision

`components/plugin_manager/include/plugin_manager/host_api.h` is the canonical host API surface
in this repo. The SDK copy (`sdk/host_api.h` in cdc-badge-plugins) MUST stay byte-identical, and
any surface change is committed in both repos together.

- Compatibility rule: a plugin's required level matches when its major equals the firmware major
  and its minor is ≤ the firmware minor.
- The SDK copy is only ever `cp`-ed over from the canonical header, never hand-edited.
- Version numbers (`HOST_API_LEVEL_*`) are never bumped without an explicit user instruction.
- Adding a host function: declare in `host_api.h` (mirror to the SDK), implement in
  `host_api_<family>.cpp` (drop the stub), register the WAMR wrapper in `WamrImports.cpp`, ship
  both repos' CI together.

## Consequences

- Enables: a single source of truth for the plugin ABI and a deterministic compatibility check
  at plugin load.
- Must hold: the two header copies stay byte-identical; CI mirrors the canonical header against
  the SDK; surface changes land in both repos in the same change.
- Cost: every host API change is a two-repo operation and cannot be done unilaterally in the
  firmware.
