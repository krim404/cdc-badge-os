# Implementation Plan: Spec-Driven Repository Transition

**Branch**: `message-transfer-framework` (spec dir `specs/001-current-system-spec`) | **Date**: 2026-06-14 | **Spec**: [spec.md](./spec.md)

**Input**: "Erstelle einen technischen Plan auf Basis der vorhandenen Spezifikation. Der Plan
soll keine neue Funktion bauen. Der Plan soll das Repo in einen spec-driven Zustand überführen."
Required content: missing specs, missing tests, architecture decisions to document, risks,
refactorings that are only documented (not implemented).

**Note**: This plan covers a **process/documentation transition only**. It produces NO new product
functionality and changes NO firmware behaviour. Every deliverable is a spec, a test, an
architecture record (ADR), a risk entry, or a documentation note. Code edits are limited to
**additive, non-behavioural** scaffolding (a host-side test target and ADR/doc files). Per the
constitution: no version bumps, no migration code, surgical changes only.

## Summary

The repository currently has exactly one spec — the reverse-spec baseline `001-current-system-spec`
— and **no own automated tests** (verified: only third-party `test/` dirs exist; CI runs `pio run`
build-only). Architecture knowledge lives implicitly in code, `CLAUDE.md`, the constitution, the
newly generated `website/` docs, and the `MEMORY.md` notes. The transition makes the repo
spec-driven by: (1) decomposing the baseline into per-capability specs, (2) standing up a
host-side test target plus hardware-in-the-loop (HIL) test plans, (3) recording the load-bearing
architecture decisions as ADRs, (4) opening a tracked risk register, and (5) cataloguing
desirable refactors as documentation only (not executed). The doc-vs-code discrepancies recorded in
the baseline (D1–D4, B-items) are folded into this work as doc-fix and clarification items so the
`website/` docs can resume their role as the long-term source of truth.

The exhaustive registries (which specs / tests / ADRs / risks / refactors) live in
[data-model.md](./data-model.md); conformance gates in [contracts/](./contracts/); the
validation checklist in [quickstart.md](./quickstart.md). Phase-0 method decisions are in
[research.md](./research.md).

## Technical Context

**Language/Version**: C++17 (firmware), Python 3 (host tooling), Astro/MD (docs). No language change.

**Primary Dependencies**: ESP-IDF + PlatformIO (`env:cdc_badge_usb`), mbedTLS, WAMR 2.4.4, NimBLE,
TinyUSB, TROPIC01 libtropic. For the transition: PlatformIO `native` test env + Unity (already
vendored by ESP-IDF) for host-runnable pure-logic units.

**Storage**: N/A for this transition (documentation + tests). Existing persistence (NVS, TROPIC01
ECC/R-Memory, FAT plugins partition) is described by the spec, not modified.

**Testing**: Currently **none owned by the project**. Target: PlatformIO `native` unit tests for
dependency-light logic; documented HIL test plans for hardware/protocol behaviour. Tests are
**additive and non-blocking** (the constitution keeps firmware hardware-verified).

**Target Platform**: ESP32-S3 (CDC Badge v1.0/v1.1) for firmware; host (Linux/macOS CI) for the
new `native` test target and doc build.

**Project Type**: Embedded firmware monorepo + generated docs site. This feature is a
cross-cutting documentation/test/architecture transition over that monorepo.

**Performance Goals**: N/A (no runtime code path added). Constraint: the `native` test target and
doc/spec builds must run in CI without flashing hardware.

**Constraints**: No new product features; no behaviour change; no version bumps; no migration code;
surgical edits only. Flash-conserving (the plan must not require firmware flashes to make progress
on specs/ADRs/host-tests). Hardware-only verification is deferred to explicit HIL runs.

**Scale/Scope**: ~12 active modules + 9 core components; ~50 FRs and ~17 open/discrepancy items in
the baseline spec to decompose, cover with tests, and back with ADRs.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Evaluated against constitution v1.0.1. This is a documentation/test transition with no new product
code, so it is strongly aligned by construction.

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Module Isolation & Self-Containment | ✅ PASS | No code moved. Per-module specs/ADRs reinforce isolation; capability docs stay with their module where applicable. |
| II. Memory Discipline: PSRAM-First | ✅ PASS | No new buffers. Captured as an ADR (documentation), not changed. |
| III. Security & Sandbox Integrity | ✅ PASS | No control weakened. The two-KDF PIN rationale, sandbox model, and slot map are *documented*, not altered. |
| IV. Simplicity & Surgical Change | ✅ PASS | Refactors are **documentation-only** (Refactoring Register); the sole code additions are a non-behavioural test target + ADR files. |
| V. Versioning & Pre-1.0 Data Freedom | ✅ PASS | No version bumps; no migration code; no on-device format touched. |

**Coding-standard gates** (Platform & Coding Standards): the new host-test target and ADR docs
follow English-only and Doxygen-backslash rules where they touch firmware. No GATE violation.
**Result: PASS — no entries in Complexity Tracking.**

## Project Structure

### Documentation (this feature)

```text
specs/001-current-system-spec/
├── spec.md              # Existing reverse-spec baseline (input)
├── plan.md              # This file
├── research.md          # Phase 0: transition-method decisions + current-state survey
├── data-model.md        # Phase 1: Spec / ADR / Test / Risk / Refactoring registries
├── contracts/
│   └── spec-driven-conformance.md   # Phase 1: the gates a "spec-driven" repo must satisfy
├── quickstart.md        # Phase 1: how to validate the spec-driven state
└── checklists/
    └── requirements.md  # Existing spec-quality checklist
```

### Source Code (repository root)

The transition introduces only additive, non-behavioural locations; existing trees are untouched.

```text
specs/                                   # NEW per-capability specs land here (NNN-<capability>)
website/src/content/docs/dev/adr/        # NEW Architecture Decision Records (part of the docs site)
test/
└── host/                                # NEW PlatformIO `native` test target (pure-logic units)
    ├── test_pin_kdf/                    # S2K + truncated-SHA256 vectors
    ├── test_crc/                        # CRC16-ISO13239, CRC32, CRC-24
    ├── test_base64/                     # backup container base64 round-trip
    └── test_capability_pins/            # GPIO allow/block-list policy
platformio.ini                           # NEW [env:native] (test-only; does not affect firmware build)
.github | .gitlab | .forgejo CI          # NEW test stage invoking the native env + doc/spec lint
```

**Structure Decision**: ADRs live under `website/src/content/docs/dev/adr/` so architecture
records are part of the single documentation system (the long-term source of truth), consistent
with the constitution's Governance pointer. Host tests live in `test/host/` behind a dedicated
PlatformIO `native` environment so they run in CI without ESP-IDF/hardware and never alter the
`cdc_badge_usb` firmware build. Per-capability specs follow the Spec Kit numbering under `specs/`.

## Deliverables (the five required outputs)

Concise here; full registries with IDs, ownership and acceptance in [data-model.md](./data-model.md).

1. **Missing specs** — decompose the monolithic baseline into per-capability feature specs:
   lock/PIN & duress, FIDO2/WebAuthn, 2FA (TOTP/HOTP/CR), password vault, OpenPGP CCID + GPG/SSH,
   GPG cross-signing, badge-to-badge messaging (cdc_msg), encrypted backup/restore, plugin
   runtime & host API, BLE controller & HID, serial console, persistence & factory-reset/duress
   wipe, i18n, power/sleep & UI/keypad. Each traces its FRs to the baseline and to code.

2. **Missing tests** — the repo owns none. Host-runnable units first (PIN KDFs, CRC16/CRC32/CRC-24,
   base64, CBOR encoders, vCard field mapping, backup container framing, capability/GPIO policy,
   message-transfer framing/bounds), then documented HIL plans for hardware/protocol behaviour
   (FIDO2 ClientPIN, OpenPGP CCID, BLE numeric-comparison transfer, duress wipe, e-paper refresh).

3. **Architecture decisions to document (ADRs)** — PSRAM-first memory model; module isolation &
   single-point registration; TROPIC01 slot allocation; **two-KDF PIN hashing (protocol-driven, from
   D2/FR-007)**; no-migration / build-profile-byte wipe; plugin sandbox & capability model; host
   API canonical + SDK byte-mirror; single BLE controller (`IBluetoothController`); e-paper refresh
   modes; CP437 display pipeline; attestation-signed PIN record.

4. **Risks** — open a tracked register: hardware-unverified WIP (cdc_msg, BLE vCard, GPG cross-sign
   send path); doc-vs-code drift (D1–D4); `DEBUG_MODE` default-on shipping risk; `credProtect` not
   enforced (B2); no test coverage today; secure-element unavailable in CI (host-test scope limit);
   flash wear during HIL verification; OpenPGP PW3 terminal-lockout footgun.

5. **Refactorings — documented only, NOT implemented** — header-guard → `#pragma once` outliers
   (`mod_fido2`/`mod_gpg`/`openpgp`); consolidating duplicated CRC/base64/hex helpers; the OpenPGP
   DEC-key-decrypted-to-RAM caveat; potential `credProtect` enforcement; the explicitly **rejected**
   PIN-hash unification (kept as a "won't-do, here's why" record). Each is captured as a Refactoring
   Register entry with rationale and an explicit "do not execute under this plan" flag.

## Complexity Tracking

> No constitution violations. No entries required.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| — | — | — |
