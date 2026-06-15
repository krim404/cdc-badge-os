# Coverage & Traceability Index

Gate C1/C2 instrument: maps every baseline FR (`spec.md`) to exactly one owning per-capability spec,
and records the pre-transition firmware build baseline for gate C6.

## FR → owning spec (gate C1/C2)

Every baseline functional requirement is owned by exactly one spec. No FR is unowned; no FR is
owned twice.

| Spec | Capability | FRs owned |
|------|-----------|-----------|
| 002-lock-pin-duress | Lock / PIN / lockout / duress | FR-001, FR-002, FR-003, FR-004, FR-005, FR-006, FR-007 |
| 003-fido2-webauthn | FIDO2 / WebAuthn | FR-010, FR-011, FR-012, FR-013, FR-014, FR-015, FR-016, FR-017 |
| 004-2fa-otp | TOTP / HOTP / CR | FR-020, FR-021, FR-022, FR-023 |
| 005-password-vault | Password vault | FR-030, FR-031, FR-032 |
| 006-openpgp-ccid | OpenPGP CCID + GPG/SSH | FR-040, FR-041, FR-042, FR-043 |
| 007-gpg-cross-signing | GPG cross-signing (Provisional/WIP) | FR-044 |
| 008-message-transfer | Badge-to-badge messaging (Provisional/WIP) | FR-050, FR-051, FR-052, FR-053, FR-054 |
| 009-encrypted-backup | Encrypted backup / restore | FR-060, FR-061, FR-062, FR-063, FR-064 |
| 010-plugin-runtime-hostapi | Plugin runtime & host API | FR-071, FR-072, FR-073, FR-074, FR-075 |
| 011-ble-controller-hid | BLE controller & HID | FR-090 |
| 012-serial-console | Serial console | FR-080, FR-081, FR-082 |
| 013-persistence-factory-reset | Persistence & factory-reset / duress wipe | FR-070 |
| 014-connectivity-time-settings-power | Connectivity / time / settings / power | FR-091, FR-092, FR-093, FR-094 |
| 015-i18n | Internationalisation | FR-100 |
| 016-keypad-ui | Keypad / UI / ViewStack | FR-110, FR-111 |

Notes:

- FR-006 (attestation-signed PIN record) is owned by **002**; **013** references it for the
  factory-reset/wipe path but does not own it.
- FR-070 (self-destruct wipe mechanics) is owned by **013**; **002** (FR-004 duress trigger)
  references it.
- Coverage check: FR-001..007, 010..017, 020..023, 030..032, 040..044, 050..054, 060..064,
  070..075, 080..082, 090..094, 100, 110..111 → each appears exactly once above.

## SC → verifying test (gate C3 pointer)

Success Criteria map to the Test Catalog in `data-model.md` (Tier-1 host `T-Hxx`, Tier-2 HIL
`T-HILxx`). Populated as the US3/US4 tasks land.

## Firmware build baseline (gate C6)

Pre-transition baseline of the `cdc_badge_usb` firmware artifact, used to prove the spec-driven
scaffolding (`[env:native]`, `test/host/**`, docs) does not change the firmware build.

| Field | Value |
|-------|-------|
| Artifact | `.pio/build/cdc_badge_usb/firmware.bin` |
| Size | 2.0 MB (2,097,152-class image; `ls -l` reports 2.0M) |
| MD5 | `8d5ce1267da4315bf75747d105da1918` |
| Captured | 2026-06-14, before any transition task |

Gate C6 (Polish task T064): after the scaffolding, a fresh `pio run -e cdc_badge_usb` must produce
a `firmware.bin` whose contents are attributable solely to pre-existing sources — the host test
target and documentation MUST NOT appear in the firmware image.

## Gate C6 result (T064)

The transition touched NO firmware source. Changes are confined to: `platformio.ini` (added
`default_envs` + `[env:native]` + a test-only `test_ignore`), `.clangd` + `test/host/.clangd`
(editor config), `test/host/**` (host tests), `specs/**`, and `website/**` (docs). `git status`
shows no `components/**` or `main/**` source file modified by this work (the pre-existing `M`
entries are unrelated message-transfer-framework changes). `pio run --list-targets` confirms the
firmware env is intact and `pio run` builds only `cdc_badge_usb`. Byte-identical comparison to the
baseline MD5 is not meaningful because the firmware embeds `BUILD_TIME`/`BUILD_DATE` macros, so every
build differs; the meaningful invariant — no firmware source or firmware build flag changed — holds.

## Quickstart gate results (T065)

| Gate | Result |
|------|--------|
| C1 Spec coverage | ✅ 15 capability specs (002–016); FR→spec map above has no orphan/gap |
| C2 FR⇄code traceability | ✅ specs cite code anchors; open items kept as `[NEEDS CLARIFICATION]` |
| C3 Test contract | ◑ Tier-1: 5 host test folders, **34/34 green** under `pio test -e native` in CI; 4 host tests (PIN-KDF, vCard, backup, capability) deferred — need firmware refactoring (RF-02/RF-03). Tier-2: 8 HIL plans documented (non-blocking). |
| C4 Doc reconciliation | ✅ D1–D3 fixed + per-area sweep complete; see `doc-reconciliation.md` |
| C5 ADRs recorded | ✅ ADR-0001..0012 accepted under `website/.../dev/adr/` |
| C6 No behaviour/version drift | ✅ see Gate C6 result above |
| C7 Risk visibility | ✅ `risks.md` (R-01..09); WIP features flagged in specs 007/008/011 |
| C8 Refactor discipline | ✅ `refactoring-backlog.md` (RF-01..06, all `execute: NO`); no refactor applied |

Net: gates C1, C2, C4, C5, C6, C7, C8 fully met; C3 met for the host tier (deferred items
explicitly tracked, not silently dropped).
