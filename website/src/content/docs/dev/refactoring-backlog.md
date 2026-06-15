---
title: Refactoring backlog
description: Documented-only refactoring register (RF-01..RF-06) for the spec-driven transition; none are executed under the current plan.
sidebar:
  order: 98
---

This is the refactoring backlog for CDC Badge OS. It records desirable refactors so the knowledge
is not lost, without acting on them. Under the spec-driven repository transition every entry carries
an explicit `execute: NO` flag: the transition is documentation, test, and architecture work only,
with no firmware behaviour change. Entries are revisited when a future spec or plan picks them up.

The register mirrors RF-01..RF-06 from
[data-model.md](https://github.com/Krim/cdc-badge-os/blob/release/specs/001-current-system-spec/data-model.md)
in the reverse-spec baseline.

## RF-01 — Header guards to `#pragma once`

- **Description**: Convert `#ifndef` header guards to `#pragma once` in `mod_fido2`, `mod_gpg`,
  and `openpgp`.
- **Rationale**: Style uniformity; these are the remaining constitution outliers from the
  `#pragma once` convention.
- **Target**: `components/mod_fido2`, `components/mod_gpg`, `components/openpgp` headers.
- **`execute: NO`**

## RF-02 — Consolidate shared helpers

- **Description**: Consolidate the duplicated CRC, base64, and hex helpers into one shared utility.
- **Rationale**: DRY across the OTP HID, backup container, PGP armor, and message-transfer paths
  that each carry their own copy.
- **Target**: Shared util consumed by OTP, backup, PGP, and transfer code paths.
- **`execute: NO`**

## RF-03 — OpenPGP DEC key decrypted to RAM

- **Description**: The OpenPGP decryption (DEC) key is decrypted to RAM per ECDH operation.
- **Rationale**: Document the caveat; no design change is proposed now.
- **Target**: `components/openpgp` ECDH operation path.
- **`execute: NO`**

## RF-04 — Enforce `credProtect` at assertion

- **Description**: Enforce the `credProtect` levels at assertion time.
- **Rationale**: Closes open item B2; needs CTAP2 design work and HIL verification.
- **Target**: `mod_fido2` assertion path (tracked by spec 003).
- **`execute: NO`**

## RF-05 — Unify PIN hashing (REJECTED, won't-do)

- **Description**: Unify PIN hashing to a single KDF across badge/FIDO2, OpenPGP, and duress.
- **Status**: **REJECTED — permanent won't-do.** Unifying the KDFs would break either FIDO2
  ClientPIN or the OpenPGP card. The badge/FIDO2 PIN hash is fixed by CTAP2 ClientPIN to
  `LEFT(SHA-256(PIN), 16)`, and the OpenPGP PW1/PW3 hash is fixed by the OpenPGP card KDF-DO to
  Iterated+Salted S2K. The two KDFs are protocol-driven, not an inconsistency. See
  [ADR-0004](./adr/0004-two-kdf-pin-hashing/) (two-KDF PIN hashing is protocol-driven). Kept here as
  a "won't-do, here's why" record.
- **Rationale**: Retained so the rejection rationale is not lost and the unification is not
  re-proposed.
- **Target**: None (won't-do).
- **`execute: NO`**

## RF-06 — Stale `docs/SECURITY.md` reference in code

- **Description**: `components/cdc_core/include/cdc_core/feature_flags.h:48` references the deleted
  `docs/SECURITY.md`. Repoint the in-code comment to the `website/` security docs.
- **Rationale**: Trivial comment cleanup; flagged under discrepancy D4.
- **Target**: `components/cdc_core/include/cdc_core/feature_flags.h:48`.
- **`execute: NO`** (flagged under D4; not edited under this plan)
