---
title: ADR-0003 — TROPIC01 slot allocation
description: tropic_slot_map.h is the single authoritative slot map.
sidebar:
  order: 3
---

**Status**: accepted
**Source**: `main/tropic_slot_map.h`; Constitution III; FR-012/FR-020/FR-030

## Context

The TROPIC01 secure element provides 32 ECC slots (0–31) and 512 R-Memory slots (0–511, ~444 B
each). Private keys live in ECC slots; metadata and the PIN record live in R-Memory. Slot 0 of
each space is reserved. Module IDs are permanent on-device identifiers and must never be
reassigned.

## Decision

`main/tropic_slot_map.h` is the single authoritative slot map. All consumers fetch ranges via
the central map (compile-time bounds- and overlap-validated); no module hardcodes its own
ranges elsewhere.

| Module | ECC slots | R-Memory slots |
|--------|-----------|----------------|
| SYSTEM (attestation) | 0 | 0 |
| GPG | 1–3 | 1–3 |
| CA | 4 | 4 |
| FIDO2 | 5–30 | 5–30 |
| 2FA (TOTP/HOTP/CR) | — | 31–130 |
| GPG RSA keys | — | 131–136 |
| Password vault | — | 137–500 |
| Plugin pool | 31 | 501–511 |

The plugin ECC slot 31 is the last physical ECC slot; FIDO2 ECC ends at 30, so the plugin slot
does not overlap. Plugin R-Memory sub-slots inside 501–511 are assigned dynamically at runtime,
but the range itself is part of the central map.

## Consequences

- Enables: each module reads/writes only its own slots; overlaps are caught at build time.
- Must hold: this table stays in sync with `tropic_slot_map.h`; new modules get new IDs and
  unused ranges, never a reused freed slot/ID.
- Cost: slot exhaustion (e.g. > 26 FIDO2 credentials, > 100 OATH accounts, > 369 vault entries)
  is a hard limit set by the map, not a soft one.
