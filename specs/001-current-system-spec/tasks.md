---
description: "Task list for the Spec-Driven Repository Transition"
---

# Tasks: Spec-Driven Repository Transition

**Input**: Design documents from `/specs/001-current-system-spec/`
**Prerequisites**: plan.md (required), spec.md, research.md, data-model.md, contracts/, quickstart.md

**Tests**: Tests ARE part of this feature (the transition's whole point is to add the repo's first
tests). Host unit tests (Tier 1) and HIL test plans (Tier 2) are first-class deliverables, not
optional add-ons.

**Scope guardrails (apply to EVERY task)**: This transition adds only documentation, specs, tests
and non-behavioural scaffolding. **No firmware behaviour changes, no version bumps, no migration
code, no hidden architecture changes.** The only code/config edits permitted are: `[env:native]`
in `platformio.ini`, files under `test/host/**`, CI workflow files, and markdown under `specs/`
and `website/src/content/docs/`. Gate C6 (firmware build unchanged) is verified in Polish.

**"User stories" here = transition work-streams** (from plan.md deliverables / conformance gates),
NOT the system-capability stories US1–US7 inside `spec.md`.

## Format: `[ID] [P?] [Story?] Description — Accept: <criterion>`

- **[P]**: parallelizable (different files, no dependency on an incomplete task)
- **[US#]**: transition work-stream label (US1..US7 below)
- Every task names a concrete file/artifact and a reviewable acceptance criterion.

Work-streams: **US1** specs · **US2** ADRs · **US3** host tests · **US4** HIL plans · **US5** risks
· **US6** doc↔code reconciliation (P1, important) · **US7** refactoring register.

---

## Phase 1: Setup (shared scaffolding)

- [x] T001 [P] Add a host-only `[env:native]` (PlatformIO `platform = native`, Unity, `test/host`) to `platformio.ini` — Accept: `pio test -e native` is recognised and runs; `[env:cdc_badge_usb]` config is untouched.
- [x] T002 [P] Create `test/host/` with a `README.md` and one placeholder passing test (`test/host/test_smoke/test_smoke.cpp`) — Accept: `pio test -e native` runs the placeholder green; no firmware source touched.
- [x] T003 [P] Create the ADR area `website/src/content/docs/dev/adr/` with `index.md` (lists the ADR catalog) and `_template.md` (MADR: Context/Decision/Status/Consequences) — Accept: index renders in the Astro docs build and links all 12 planned ADRs.
- [x] T004 Add a CI test stage running `pio test -e native` (plus a spec/ADR markdown lint) to `.github/workflows/build.yml`, `.gitlab-ci.yml`, and `.forgejo/workflows/build.yml` — Accept: all three run the native tests in addition to `pio run`; green on the placeholder.

## Phase 2: Foundational (cross-cutting, referenced by all work-streams)

- [x] T005 Create the spec coverage/traceability index `specs/001-current-system-spec/coverage.md` mapping every baseline FR (FR-001..FR-110) → its owning per-capability spec (002–016) — Accept: every FR mapped exactly once; zero orphans; zero uncovered capabilities (gate C1/C2).
- [x] T006 Record the pre-transition firmware build baseline in `specs/001-current-system-spec/coverage.md` (artifact size + map summary from `pio run -e cdc_badge_usb`) — Accept: a documented baseline exists to diff against for gate C6 in Polish.

---

## Phase 3: US1 — Per-capability specs exist (Priority: P1) 🎯 MVP

**Goal**: Decompose the monolithic baseline into one spec per capability so each can be clarified,
planned and tested independently (gates C1/C2).

**Independent test**: `specs/coverage.md` shows every baseline FR owned by exactly one spec, each FR
tracing to a code anchor or a `[NEEDS CLARIFICATION]` marker; no capability uncovered.

- [x] T007 [P] [US1] Write `specs/002-lock-pin-duress/spec.md` from baseline FR-001..007, FR-070 (owner `cdc_core` PinManager, `cdc_os_ui` lock) — Accept: follows the spec template; FRs cite code anchors; B4 left as `[NEEDS CLARIFICATION]`.
- [x] T008 [P] [US1] Write `specs/003-fido2-webauthn/spec.md` from FR-010..017 (owner `mod_fido2`) — Accept: B2 marked clarification; B1 recorded resolved (same secret as badge PIN).
- [x] T009 [P] [US1] Write `specs/004-2fa-otp/spec.md` from FR-020..023 (owner `mod_2fa`) — Accept: TOTP/HOTP/CR behaviours each have acceptance scenarios.
- [x] T010 [P] [US1] Write `specs/005-password-vault/spec.md` from FR-030..032 (owner `mod_password`) — Accept: B3 (at-rest encryption) marked clarification.
- [x] T011 [P] [US1] Write `specs/006-openpgp-ccid/spec.md` from FR-040..043 (owner `mod_gpg`/`openpgp`) — Accept: PW1/PW3 semantics incl. PW3 terminal-lockout documented.
- [x] T012 [P] [US1] Write `specs/007-gpg-cross-signing/spec.md` from FR-044 (owner `mod_gpg`) — Accept: flagged **Provisional (WIP)**; B13 (curve labeling), B14 noted.
- [x] T013 [P] [US1] Write `specs/008-message-transfer/spec.md` from FR-050..054 (owner `cdc_msg`, `mod_vcard`) — Accept: flagged **Provisional (WIP)**; framing limits and abuse budgets captured.
- [x] T014 [P] [US1] Write `specs/009-encrypted-backup/spec.md` from FR-060..064 (owner `cdc_os_ui` BackupManager) — Accept: B5 (export auth) marked clarification; SE keys-excluded invariant stated.
- [x] T015 [P] [US1] Write `specs/010-plugin-runtime-hostapi/spec.md` from FR-071..075 (owner `plugin_manager`, `wamr_runtime`) — Accept: capability/pointer-validation model captured; B15 noted.
- [x] T016 [P] [US1] Write `specs/011-ble-controller-hid/spec.md` from FR-090 (owner `cdc_hal` BluetoothController, `mod_blehid`) — Accept: B10 (bond limit), B12 (HID descriptor) marked clarification; single-controller invariant stated.
- [x] T017 [P] [US1] Write `specs/012-serial-console/spec.md` from FR-080..082 (owner `serial_cmd`) — Accept: D1 (AUTH default) + B9 (timeout) marked clarification; AUTH-gate contract stated.
- [x] T018 [P] [US1] Write `specs/013-persistence-factory-reset/spec.md` from FR-006, FR-070 (owner `cdc_core` FactoryReset, NVS, slot map) — Accept: B6 (slot size), B7 (orphan cleanup) marked clarification.
- [x] T019 [P] [US1] Write `specs/014-connectivity-time-settings-power/spec.md` from FR-091..094 (owner WiFi/settings/sleep) — Accept: B8 (timeouts) marked clarification.
- [x] T020 [P] [US1] Write `specs/015-i18n/spec.md` from FR-100 (owner `cdc_ui` I18n) — Accept: overlay/CP437/endonym rules captured.
- [x] T021 [P] [US1] Write `specs/016-keypad-ui/spec.md` from FR-110..111 (owner `cdc_views`, `cdc_ui`) — Accept: B11, B15 (modal depth) marked clarification.

**Checkpoint**: every capability has a spec; `coverage.md` shows full FR ownership.

---

## Phase 4: US2 — Architecture decisions recorded (Priority: P1)

**Goal**: Record load-bearing decisions as ADRs the specs can cite (gate C5).
**Independent test**: ADR-0001..0012 exist under `website/src/content/docs/dev/adr/`, accepted, each citing its source; index links all.

- [x] T022 [P] [US2] Write `website/src/content/docs/dev/adr/0001-psram-first-memory.md` (source: Constitution II, NFR-001) — Accept: MADR format; states the internal-SRAM-reserved invariant.
- [x] T023 [P] [US2] Write `.../adr/0002-module-isolation-registration.md` (Constitution I, NFR-007) — Accept: single-registration-point invariant stated.
- [x] T024 [P] [US2] Write `.../adr/0003-tropic01-slot-allocation.md` (slot map) — Accept: cites `main/tropic_slot_map.h` as authoritative; reproduces the allocation table.
- [x] T025 [P] [US2] Write `.../adr/0004-two-kdf-pin-hashing.md` (D2, FR-007/FR-017) — Accept: explains CTAP2 `LEFT(SHA-256,16)` vs OpenPGP S2K as protocol-driven; records why unification is rejected.
- [x] T026 [P] [US2] Write `.../adr/0005-no-migration-build-profile-wipe.md` (Constitution V, NFR-002) — Accept: build-profile-byte wipe + no-migration policy stated.
- [x] T027 [P] [US2] Write `.../adr/0006-plugin-sandbox-capability-model.md` (FR-071..074) — Accept: capability gating + AOT-default-off + pointer validation captured.
- [x] T028 [P] [US2] Write `.../adr/0007-host-api-canonical-sdk-mirror.md` (FR-074) — Accept: byte-identical SDK mirror + major/minor versioning rule stated.
- [x] T029 [P] [US2] Write `.../adr/0008-single-ble-controller.md` (Constitution I, MEMORY BLE) — Accept: modules-never-touch-NimBLE invariant stated.
- [x] T030 [P] [US2] Write `.../adr/0009-epaper-refresh-modes.md` (FR-094) — Accept: PARTIAL_LIGHT-never-promoted rule stated.
- [x] T031 [P] [US2] Write `.../adr/0010-cp437-display-pipeline.md` (FR-094) — Accept: no-`gfx->print`-for-i18n rule stated.
- [x] T032 [P] [US2] Write `.../adr/0011-attestation-signed-pin-record.md` (FR-006, B4) — Accept: ECDSA-P256 slot-0 signature + tamper→reinit invariant stated.
- [x] T033 [P] [US2] Write `.../adr/0012-build-profiles-and-defaults.md` (D1, D3) — Accept: records the REAL `FEATURE_SECURE_SERIAL`/`DEBUG_MODE` defaults from `feature_flags.h` and the release gate SC-013.

---

## Phase 5: US3 — Host-runnable unit tests in CI (Priority: P1)

**Goal**: Give the repo its first automated tests for portable security-critical logic (gate C3, Tier 1).
**Independent test**: `pio test -e native` runs all of T-H01..09 green in CI without a badge.
**Depends on**: T001, T002 (native env + skeleton).

- [ ] T034 [P] [US3] Implement `test/host/test_pin_kdf/` — known-answer vectors for truncated SHA-256 (badge/FIDO2) and S2K (OpenPGP/duress) per `PinManager` (FR-007) — Accept: vectors pass; no firmware change. — **DEFERRED**: needs firmware refactoring to be host-testable (RF-02/RF-03); not implemented.
- [x] T035 [P] [US3] Implement `test/host/test_crc/` — CRC16-ISO13239, CRC32, CRC-24 vectors (FR-023/044/082) — Accept: residual/known-answer checks pass.
- [x] T036 [P] [US3] Implement `test/host/test_base64/` — backup-container base64 round-trip (FR-060) — Accept: encode→decode identity over random buffers.
- [x] T037 [P] [US3] Implement `test/host/test_cbor/` — CTAP2 CBOR map/array encoder vectors (FR-011) — Accept: encoder output matches reference bytes (regression for prior enumerate bug).
- [ ] T038 [P] [US3] Implement `test/host/test_vcard/` — vCard 4.0 field mapping, 768-byte bound, exact-text dedup (FR-054) — Accept: mapping + bound + dedup cases pass. — **DEFERRED**: needs firmware refactoring to be host-testable (RF-02/RF-03); not implemented.
- [ ] T039 [P] [US3] Implement `test/host/test_backup/` — container framing (magic/version/kdf_iters/salt/nonce/AAD) + AES-256-GCM round-trip (FR-060..063) — Accept: round-trip + tamper-rejects-tag pass. — **DEFERRED**: needs firmware refactoring to be host-testable (RF-02/RF-03); not implemented.
- [x] T040 [P] [US3] Implement `test/host/test_msg_frame/` — message-transfer framing & bounds (mime≤63, name≤31, total≤4096, opcodes, BadFrame/TooLarge) (FR-050/053) — Accept: bound/opcode cases pass.
- [ ] T041 [P] [US3] Implement `test/host/test_capability_pins/` — GPIO hard-block list + manifest whitelist + busy-conflict (FR-073) — Accept: blocked pin rejected, whitelisted allowed, conflict→busy. — **DEFERRED**: needs firmware refactoring to be host-testable (RF-02/RF-03); not implemented.
- [x] T042 [P] [US3] Implement `test/host/test_cp437/` — `cdc::core::cp437::fromUtf8` umlaut/round-trip vectors (FR-094/100) — Accept: ä ö ü Ä Ö Ü ß map to expected CP437 bytes.

**Checkpoint**: CI native stage green; the repo has owned tests for the first time.

---

## Phase 6: US4 — HIL test plans documented (Priority: P2)

**Goal**: Repeatable hardware procedures for behaviour not coverable on host (gate C3, Tier 2; non-blocking).
**Independent test**: each plan has prerequisites, steps, and explicit pass criteria; marked non-blocking.

- [x] T043 [P] [US4] Write `specs/001-current-system-spec/hil/T-HIL01-fido2.md` (register+assert, ClientPIN=`LEFT(SHA-256,16)`, counter) — Accept: steps + pass criteria; non-blocking.
- [x] T044 [P] [US4] Write `.../hil/T-HIL02-openpgp-ccid.md` (`gpg --card-status`, sign/decrypt/SSH, PW1/PW3) — Accept: steps + pass criteria.
- [x] T045 [P] [US4] Write `.../hil/T-HIL03-ble-transfer.md` (numeric-comparison vCard round-trip, abuse budgets) — Accept: marked Provisional/WIP; non-blocking.
- [x] T046 [P] [US4] Write `.../hil/T-HIL04-duress-wipe.md` (full ECC+R-Mem+NVS wipe, crash-safe re-run) — Accept: documented destructive procedure + recovery.
- [x] T047 [P] [US4] Write `.../hil/T-HIL05-pin-lockout.md` (3 attempts → 60s recovery → no brick; shared serial AUTH) — Accept: pass criteria incl. no-permanent-brick.
- [x] T048 [P] [US4] Write `.../hil/T-HIL06-epaper-refresh.md` (PARTIAL_LIGHT clock not promoted to FULL) — Accept: observation procedure + criteria.
- [x] T049 [P] [US4] Write `.../hil/T-HIL07-plugin-sandbox.md` (blocked pin / undeclared capability / OOB rejected, no crash) — Accept: criteria; non-blocking.
- [x] T050 [P] [US4] Write `.../hil/T-HIL08-backup-roundtrip.md` (export/import on device; wrong passphrase rejected; SE keys excluded) — Accept: round-trip + negative case criteria.

---

## Phase 7: US5 — Risks visible (Priority: P2)

**Goal**: A living risk register with owners; WIP features flagged in their specs (gate C7).

- [x] T051 [US5] Create `specs/001-current-system-spec/risks.md` with R-01..R-09 (severity, mitigation, owning spec) from data-model.md — Accept: 9 risks, each cross-referencing a spec/ADR.
- [x] T052 [P] [US5] Add a "Provisional (WIP, not hardware-verified)" banner to specs `007`, `008`, and the BLE-vCard parts of `011` — Accept: each WIP spec carries the banner per Clarifications 2026-06-14. (depends on T012, T013, T016)

---

## Phase 8: US6 — Existing docs reconciled to code (Priority: P1, important)

**Goal**: Bring the freshly generated `website/src/content/docs/**` into agreement with the code so
the documentation can resume its role as the long-term source of truth (gate C4). This covers the
confirmed Category-A defects AND a systematic page-by-page sweep of every doc area against code and
the new per-capability specs.

**Independent test**: the reconciliation tracker shows every doc page checked against a code/spec
reference with status `matches` or `fixed`; remaining mismatches are logged as Category-B
clarifications, none left silently wrong.

**Depends on**: per-capability specs (US1) as the reference, and ADRs T025/T033 for D1–D3.

### Confirmed Category-A fixes (pre-identified)

- [x] T053 [US6] Fix D1 in `website/src/content/docs/power/serial-console.md` — state the real `FEATURE_SECURE_SERIAL` default from `feature_flags.h`/Kconfig — Accept: doc matches code; cites ADR-0012. (depends T033)
- [x] T054 [US6] Fix D2 in `website/src/content/docs/security/{pin-lockout,secure-element-keys}.md` — explain the two-KDF rationale instead of presenting it as inconsistency — Accept: text matches FR-007; cites ADR-0004. (depends T025)
- [x] T055 [US6] Fix D3 in `website/src/content/docs/security/caveats.md` — clarify DEBUG_MODE does NOT bypass the badge-PIN lockout recovery — Accept: text matches code behaviour; cites ADR-0012. (depends T033)

### Systematic per-area reconciliation sweep

- [x] T056 [US6] Create the reconciliation tracker `specs/001-current-system-spec/doc-reconciliation.md` enumerating every `website/` doc page → code/spec reference → status (matches / fixed / logged) — Accept: all doc pages listed with a status column; drives T057–T062.
- [x] T057 [P] [US6] Reconcile `website/src/content/docs/security/**` (overview, attestation, duress, pin-lockout, secure-element-keys, caveats) against code + specs 002/006/013 — Accept: each page matches code; divergences fixed or logged in the tracker.
- [x] T058 [P] [US6] Reconcile `website/src/content/docs/dev/**` (architecture, build-system, host-api, plugin-sdk, secure-element, ui-framework, module-development, api-reference) against code + specs 010/013/016 — Accept: matches code; divergences fixed/logged.
- [x] T059 [P] [US6] Reconcile `website/src/content/docs/dev/proto/**` (fido2-ctap, openpgp-ccid, otp-hid-cr, vcard, message-transfer, backup-format, gpg-cross-signing, serial-commands) against code + specs 003/004/006/007/008/009/012 — Accept: protocol details match code; divergences fixed/logged.
- [x] T060 [P] [US6] Reconcile `website/src/content/docs/guide/**` (user-facing flows) against actual UI behaviour + specs — Accept: each flow matches device behaviour; divergences fixed/logged.
- [x] T061 [P] [US6] Reconcile `website/src/content/docs/power/**` (plugins, expert-menu, languages, companion-tools, storage-tools) against code + specs 010/014 — Accept: matches code; divergences fixed/logged.
- [x] T062 [P] [US6] Reconcile `website/src/content/docs/start/**` (overview, first-flash, first-boot, keypad-navigation) against code + specs 002/016 — Accept: matches code; divergences fixed/logged.

**Checkpoint**: `website/` docs match code; tracker has no unexplained mismatch; docs can lead again.

---

## Phase 9: US7 — Refactors catalogued, not executed (Priority: P3)

**Goal**: Preserve the refactoring backlog as documentation only (gate C8); execute none.

- [x] T063 [US7] Create `website/src/content/docs/dev/refactoring-backlog.md` with RF-01..RF-06, each `execute: NO` + rationale (incl. RF-05 rejected PIN-hash unification, RF-06 D4 stale `docs/SECURITY.md` reference in `feature_flags.h:48`) — Accept: 6 entries, all execute:NO; no code modified.

---

## Phase 10: Polish & Cross-Cutting Concerns

- [x] T064 Verify gate C6: run `pio run -e cdc_badge_usb` and diff against the T006 baseline — Accept: firmware artifact unchanged by the scaffolding (no behaviour/size drift attributable to `test/host`, `[env:native]`, or doc files).
- [x] T065 Run `quickstart.md` steps 1–7 and record outcomes in `specs/001-current-system-spec/coverage.md` — Accept: gates C1–C8 hold; any failures listed.
- [x] T066 [P] Update the dev-docs index/nav (`website/src/content/docs/dev/index.md` + Astro sidebar config) to link ADRs, the refactoring backlog, the testing approach, and the reconciliation tracker — Accept: new pages reachable from the docs nav; site builds.

---

## Dependencies & Execution Order

- **Setup (T001–T004)** → blocks US3 (native env/CI) and US2 (ADR dir).
- **Foundational (T005–T006)** → guides US1 (coverage) and enables C6 baseline.
- **US1 specs (T007–T021)** → mutually independent `[P]`; cross-references to ADRs are added once US2 exists.
- **US2 ADRs (T022–T033)** → independent `[P]`; need T003.
- **US3 host tests (T034–T042)** → independent `[P]`; need T001/T002; CI via T004.
- **US4 HIL plans (T043–T050)** → independent `[P]`.
- **US5 (T052)** → depends on T012/T013/T016.
- **US6 (T053–T062)** → depends on US1 specs (reference) + ADRs T025/T033; sweep tasks T057–T062 are `[P]` and need the tracker T056.
- **Polish (T064–T066)** → depends on the bulk of the above; T064 needs T006.

## Parallel Opportunities

- All of Setup T001–T003 in parallel; T004 after T001.
- Entire US1 spec set (T007–T021) in parallel once Foundational is done.
- Entire US2 ADR set (T022–T033) in parallel once T003 is done.
- Entire US3 host-test set (T034–T042) in parallel once T001/T002 are done.
- Entire US4 HIL-plan set (T043–T050) in parallel anytime.
- US6 per-area sweeps (T057–T062) in parallel once the tracker T056 exists and the referenced specs are written.

## Implementation Strategy

- **MVP scope**: Phase 1 (Setup) + Phase 2 (Foundational) + **US1 (per-capability specs)**. This
  alone makes the repo spec-driven at the documentation level (gates C1/C2). US2 (ADRs), US3
  (host tests in CI) and **US6 (doc↔code reconciliation)** are the immediate P1 follow-ons that
  satisfy gates C5, C3 and C4.
- **Incremental delivery**: Setup → US1 (specs) → US2 (ADRs) → US3 (host tests) → **US6 (doc
  reconciliation, P1)** → US4 (HIL) → US5 (risks) → US7 (refactor backlog) → Polish. US6 sequences
  after US1/US2 because the specs and ADRs are the reference the docs are reconciled against.
- **Hard rule**: no task changes firmware behaviour, bumps a version, or refactors code. T064 is
  the gate that proves it. Doc reconciliation edits `website/**` text only — never code.

## Notes

- `[P]` = different files, no incomplete-task dependency.
- Every task names a concrete file/artifact and an `Accept:` criterion, and is independently
  reviewable.
- Tests (US3 host) are real deliverables here; HIL plans (US4) are documented procedures and are
  non-blocking for CI.
- US6 reconciliation is editing `website/` documentation to match code; the code stays the
  ground truth until the sweep completes, after which the docs resume the leading role.
