# Phase 1 Data Model: Spec-Driven Repository Transition

The "entities" of this transition are the tracking artifacts (Spec, ADR, Test Item, Risk,
Refactoring Item) and their concrete registries. These directly satisfy the five required plan
outputs. Nothing here is product data; it is the work-tracking model for making the repo
spec-driven.

## Entities

### Spec (feature specification)
- **Fields**: `id` (NNN-slug), `capability`, `owning_module/component`, `source_FRs` (baseline FR
  refs), `open_items` (baseline B-refs assigned here), `status` (planned/draft/clarified/done).
- **Relationships**: derives FRs from the baseline spec; maps to one owning module; verified by
  Test Items; backed by ADRs.
- **Validation**: must follow the Spec Kit template; every FR must trace to code; no invented
  requirements; unresolved points stay `[NEEDS CLARIFICATION]`.

### ADR (architecture decision record)
- **Fields**: `id` (NNNN), `title`, `status` (proposed/accepted), `context`, `decision`,
  `consequences`, `source` (constitution principle / discrepancy / code anchor).
- **Relationships**: backs one or more Specs/FRs; may resolve a Discrepancy or Risk.
- **Validation**: MADR-style; lives under `website/src/content/docs/dev/adr/`; states the
  invariant, not history.

### Test Item
- **Fields**: `id` (T-Hxx host / T-HILxx hardware), `tier` (host-native | HIL), `target`,
  `source_FRs`, `dependency` (none | mbedTLS | TROPIC01 | USB | BLE | display), `automatable`.
- **Relationships**: verifies one or more FRs of a Spec.
- **Validation**: host tests run in CI without flash; HIL tests are repeatable documented
  procedures with explicit pass criteria.

### Risk
- **Fields**: `id`, `description`, `severity` (low/med/high), `area` (security/process/CI/hardware),
  `mitigation`, `owner_spec`.

### Refactoring Item
- **Fields**: `id`, `description`, `rationale`, `execute` (**always NO under this plan**),
  `blocked_by` (why not now), `target`.

---

## 1. Spec Catalog — missing specs to create

Per-capability specs derived from the baseline. The baseline (`001`) stays as the system-level
reference. Numbering is indicative (assigned at `/speckit-specify` time).

| ID | Capability | Owning module/component | Source FRs (baseline) | Assigned open items |
|----|-----------|-------------------------|-----------------------|---------------------|
| 002 | Lock / PIN / lockout / duress | `cdc_core` PinManager, `cdc_os_ui` lock | FR-001..007, FR-070 | B4 (attestation sig) |
| 003 | FIDO2 / WebAuthn | `mod_fido2` | FR-010..017 | B2 (credProtect), B1 (resolved) |
| 004 | 2FA — TOTP/HOTP/CR | `mod_2fa` | FR-020..023 | — |
| 005 | Password vault | `mod_password` | FR-030..032 | B3 (at-rest encryption) |
| 006 | OpenPGP CCID + GPG/SSH | `mod_gpg`, `openpgp` | FR-040..043 | — |
| 007 | GPG cross-signing | `mod_gpg` | FR-044 | B13 (curve labeling), B14 (WIP) |
| 008 | Badge-to-badge messaging | `cdc_msg`, `mod_vcard` | FR-050..054 | B14 (WIP HW) |
| 009 | Encrypted backup / restore | `cdc_os_ui` BackupManager | FR-060..064 | B5 (export auth) |
| 010 | Plugin runtime & host API | `plugin_manager`, `wamr_runtime` | FR-071..075 | B15 (lifecycle details) |
| 011 | BLE controller & HID | `cdc_hal` BluetoothController, `mod_blehid` | FR-090 | B10 (bond limit), B12 (HID descriptor) |
| 012 | Serial console | `serial_cmd` | FR-080..082 | B9 (AUTH timeout), D1 (default) |
| 013 | Persistence & factory-reset/duress wipe | `cdc_core` FactoryReset, NVS, slot map | FR-006, FR-070 | B6 (slot size), B7 (orphan cleanup) |
| 014 | Connectivity / time / settings / power | WiFi, settings, sleep, `cdc_os_ui` | FR-091..094 | B8 (timeouts) |
| 015 | Internationalisation | `cdc_ui` I18n | FR-100 | — |
| 016 | Keypad / UI / ViewStack | `cdc_views`, `cdc_ui` | FR-110..111 | B11 (auto-type), B15 (modal depth) |

## 2. Test Catalog — missing tests to add

The repo owns **zero** tests today. Tier 1 (host) is the CI-runnable start; Tier 2 (HIL) is
documented procedures.

### Tier 1 — host `native` unit tests (no flash, CI-runnable)

| ID | Target | Source FRs | Dependency |
|----|--------|-----------|------------|
| T-H01 | PIN KDFs: truncated SHA-256 (badge/FIDO2) + S2K (OpenPGP/duress) vectors | FR-007, FR-042 | mbedTLS |
| T-H02 | CRC16-ISO13239 (OTP HID), CRC32 (upload/transfer), CRC-24 (PGP armor) | FR-023, FR-044, FR-082 | none |
| T-H03 | base64 encode/decode (backup container) | FR-060 | none |
| T-H04 | CBOR encoders (CTAP2 maps/arrays — prior enumerate bug area) | FR-011 | none |
| T-H05 | vCard 4.0 field mapping/parse, 768-byte bound, exact-text dedup | FR-054 | none |
| T-H06 | Backup container framing (magic/version/kdf_iters/salt/nonce/AAD) + AES-256-GCM round-trip | FR-060..063 | mbedTLS |
| T-H07 | Message-transfer framing & bounds (mime≤63, name≤31, total≤4096, opcodes, BadFrame/TooLarge) | FR-050, FR-053 | none |
| T-H08 | Capability/GPIO policy: hard block list + manifest whitelist + busy conflicts | FR-073 | none |
| T-H09 | CP437 ↔ UTF-8 conversion (`cdc::core::cp437::fromUtf8`) | FR-094, FR-100 | none |

### Tier 2 — hardware-in-the-loop (HIL) test plans (documented procedures)

| ID | Target | Source FRs | Dependency |
|----|--------|-----------|------------|
| T-HIL01 | FIDO2 register + assert; ClientPIN verify uses `LEFT(SHA-256,16)`; counter increments | FR-010..017 | TROPIC01, USB |
| T-HIL02 | OpenPGP CCID: `gpg --card-status`, sign/decrypt/SSH; PW1/PW3 semantics | FR-040..043 | TROPIC01, USB |
| T-HIL03 | BLE numeric-comparison transfer (vCard) round-trip; abuse budgets | FR-050..054 | BLE |
| T-HIL04 | Duress wipe: full ECC 0–31 + R-Mem 0–511 + NVS; crash-safe re-run | FR-004, FR-070 | TROPIC01 |
| T-HIL05 | PIN lockout: 3 attempts → 60s recovery → no permanent brick; shared serial AUTH lockout | FR-002, FR-003 | TROPIC01 |
| T-HIL06 | E-paper refresh: PARTIAL_LIGHT clock never promoted to FULL | FR-094 | display |
| T-HIL07 | Plugin sandbox: blocked pin / undeclared capability / OOB pointer rejected, no crash | FR-072, FR-073 | hardware |
| T-HIL08 | Backup export/import round-trip on device; wrong passphrase rejected; SE keys excluded | FR-060..063 | TROPIC01 |

## 3. ADR Catalog — architecture decisions to document

| ID | Title | Source |
|----|-------|--------|
| ADR-0001 | PSRAM-first memory model; internal SRAM reserved | Constitution II, NFR-001 |
| ADR-0002 | Module isolation & single-point registration (`MODULES` + generated init) | Constitution I, NFR-007 |
| ADR-0003 | TROPIC01 slot allocation; `main/tropic_slot_map.h` authoritative | Persistence, slot map |
| ADR-0004 | **Two-KDF PIN hashing is protocol-driven** (CTAP2 `LEFT(SHA-256,16)` vs OpenPGP S2K) | D2, FR-007/FR-017 |
| ADR-0005 | No-migration policy + build-profile-byte factory wipe | Constitution V, NFR-002 |
| ADR-0006 | Plugin WAMR sandbox & capability model; AOT default-off | FR-071..074, Constitution III |
| ADR-0007 | `host_api.h` canonical + SDK byte-mirror + major/minor versioning | FR-074, Constitution V |
| ADR-0008 | Single BLE controller via `IBluetoothController`; modules never touch NimBLE | Constitution I, MEMORY BLE notes |
| ADR-0009 | E-paper refresh-mode discipline (PARTIAL_LIGHT never promoted) | FR-094, MEMORY display notes |
| ADR-0010 | CP437 display pipeline; never `gfx->print` for i18n text | FR-094, Display Rendering rule |
| ADR-0011 | Attestation-signed PIN record in R-Memory slot 0; tamper → reinit | FR-006, B4 |
| ADR-0012 | Secure-serial gate + `DEBUG_MODE` build profiles; **record real defaults** | D1, D3 |

## 4. Risk Register

| ID | Risk | Severity | Mitigation |
|----|------|----------|-----------|
| R-01 | WIP features unverified on hardware (cdc_msg, BLE vCard, GPG cross-sign send path) | High | HIL plans T-HIL03/07; mark WIP in specs until verified |
| R-02 | Doc-vs-code drift (D1–D4) until reconciled | Med | ADR-0004/0012 + doc-fix tasks; conformance gate C4 |
| R-03 | `DEBUG_MODE` defaults ON → sensitive logging if shipped | High | ADR-0012; release checklist gate; document in security docs |
| R-04 | `credProtect` parsed/stored but not enforced at assertion (B2) | Med (security) | spec 003 records as known gap; RF-04 design note |
| R-05 | Zero automated test coverage today | High | Tier-1 host tests T-H01..09 in CI |
| R-06 | Secure element / USB / BLE absent in CI → crypto-on-SE & protocol paths uncoverable on host | Med (scope) | explicit Tier-2 HIL scope; logged, not hidden |
| R-07 | Flash wear during HIL verification | Low/Med | batch HIL runs; serial `BOOTLOADER` path; conserve flashes |
| R-08 | OpenPGP PW3 terminal lockout → wipe-only recovery (user footgun) | Med | document prominently in spec 006 + security docs |
| R-09 | Spec/code divergence over time without traceability | Med | conformance gate C1 (every capability has a spec, FRs traceable) |

## 5. Refactoring Register — documented only, NOT implemented

All entries carry `execute: NO` for this plan. They are recorded so the knowledge is not lost.

| ID | Refactoring | Rationale | execute |
|----|-------------|-----------|---------|
| RF-01 | Convert `#ifndef` header guards → `#pragma once` in `mod_fido2`/`mod_gpg`/`openpgp` | Style uniformity (constitution outliers) | **NO** |
| RF-02 | Consolidate duplicated CRC / base64 / hex helpers into one shared util | DRY across OTP/backup/PGP/transfer | **NO** |
| RF-03 | OpenPGP DEC key is decrypted to RAM per ECDH op | Document the caveat; no design change now | **NO** |
| RF-04 | Enforce `credProtect` levels at assertion | Close B2; needs CTAP2 design + HIL | **NO** |
| RF-05 | Unify PIN hashing to one KDF | **REJECTED** — would break FIDO2 ClientPIN or OpenPGP KDF-DO (see ADR-0004); kept as won't-do record | **NO** |
| RF-06 | `feature_flags.h:48` references deleted `docs/SECURITY.md` | Repoint to `website/` security docs (trivial comment) | **NO** (flagged under D4; not edited here) |
