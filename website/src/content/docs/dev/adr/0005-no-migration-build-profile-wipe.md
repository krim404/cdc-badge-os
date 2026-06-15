---
title: ADR-0005 — No-migration policy and build-profile factory wipe
description: Pre-1.0 data is disposable; a build-profile byte mismatch triggers a full wipe.
sidebar:
  order: 5
---

**Status**: accepted
**Source**: Constitution V (Pre-1.0 Data Freedom); NFR-002; `components/cdc_core/include/cdc_core/feature_flags.h` (`BUILD_PROFILE_BYTE`)

## Context

The project is pre-1.0. Every firmware flash may break on-device data formats. Holders are
expected to keep off-badge backups of critical material. A compile-time `BUILD_PROFILE_BYTE`
encodes the security-relevant build flags: `(FEATURE_SECURE_SERIAL ? 0x02 : 0x00) | (DEBUG_MODE
? 0x01 : 0x00)`. The byte compiled into the running firmware is compared to the byte stored in
NVS at boot.

## Decision

There is no migration code. New on-device formats replace old ones in the same change; old
formats are deleted, not branched on.

- No version-detection or fallback paths that read an older on-device format.
- No `MAGIC_V1` + `MAGIC_V2` style branches and no fields kept only to read a previous schema.
- Validation MAY reject a mismatching magic / schema and reinit to defaults; that is not
  migration.
- A mismatch between the stored and compiled `BUILD_PROFILE_BYTE` triggers a complete factory
  wipe (NVS partition + TROPIC01 R-Memory + ECC slots) at the next boot.

## Consequences

- Enables: a codebase free of dead compatibility paths; security-relevant flag changes
  (toggling `DEBUG_MODE` or `FEATURE_SECURE_SERIAL`) cannot silently run on stale data.
- Must hold: breaking format changes are documented in the release notes; if a fix appears to
  need a migration step, prefer wipe + reinit instead.
- Cost: the build-profile byte is a beta-phase software guard; bypass-resistant enforcement
  against an active attacker requires Secure Boot v2 with anti-rollback (on the 1.0 roadmap).
