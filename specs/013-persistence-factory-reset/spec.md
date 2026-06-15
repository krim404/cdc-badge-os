# Feature Specification: Persistence & Factory-Reset / Duress Wipe

**Feature Branch**: `013-persistence-factory-reset`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the on-device
persistence model (NVS, TROPIC01 ECC slots, R-Memory, the slot map) and the full factory-reset /
self-destruct wipe that backs the duress trigger and the build-profile-mismatch path.

> **Source of truth**: This spec lifts requirement FR-070 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); the FR number is preserved as a cross-reference. Code
> is the ground truth for current behaviour. The owning components are `cdc_core` factory-reset, the
> NVS store, and the authoritative slot map (`main/tropic_slot_map.h`). The *duress trigger*
> behaviour (FR-004) is owned by spec `002-lock-pin-duress` and only referenced here; this spec owns
> the wipe *mechanics* it invokes.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Wipe every on-chip secret on a duress trigger (Priority: P1)

When the duress trigger fires (a duress-PIN entry, owned by spec 002), the device performs a full
self-destruct: it erases the NVS boot marker and reboots, and on the next boot runs a complete
factory wipe that reinitialises NVS, deletes all TROPIC01 ECC slots 0–31, erases all R-Memory slots
0–511, and regenerates the attestation key.

**Why this priority**: The wipe is the security payload of the duress feature; if it is incomplete
or recoverable, the plausible-deniability self-destruct fails its purpose.

**Independent Test**: Trigger the duress path, let the device reboot, and confirm that after the
next boot no previously stored secret (FIDO2 credential, GPG key, TOTP seed, password entry,
vCard, OS setting) is recoverable on-device and the device is back at first-run defaults.

**Acceptance Scenarios**:

1. **Given** a duress trigger, **When** the device erases the NVS boot marker and reboots, **Then**
   on the next boot it runs a full factory wipe: NVS reinit, all ECC slots 0–31 deleted, all
   R-Memory slots 0–511 erased, and the attestation key regenerated.
2. **Given** a completed wipe, **When** the holder uses the device afterwards, **Then** no
   previously stored secret is recoverable on-device and the device behaves as a freshly
   provisioned badge at defaults.
3. **Given** a wipe in progress, **When** the firmware image and any off-badge exports are
   considered, **Then** they are out of scope of the wipe (only on-device data is erased).

---

### User Story 2 - Survive a power loss mid-wipe without a half-wiped state (Priority: P1)

The wipe is crash-safe: the NVS boot marker is reseeded only after the wipe completes, so a power
loss mid-wipe leaves the marker absent and the wipe simply re-runs on the next boot.

**Why this priority**: A security key that can be left half-wiped — some secrets gone, some still
recoverable — would be both a data-integrity and a security hazard; the re-run guarantee closes
that window.

**Independent Test**: Interrupt power partway through a wipe, power the device back on, and confirm
the wipe re-runs to completion before the device becomes usable.

**Acceptance Scenarios**:

1. **Given** a wipe interrupted by power loss, **When** the device next boots, **Then** the boot
   marker is still absent and the full wipe re-runs from the start.
2. **Given** a wipe that completes normally, **When** the device finishes, **Then** the boot marker
   is reseeded only after the wipe is complete, so a subsequent normal boot does not re-wipe.

---

### User Story 3 - Factory-wipe on a build-profile or format mismatch (Priority: P2)

This is a pre-1.0 system with no migration code. A mismatch of the persisted build-profile byte (or
a breaking on-device format change) triggers the same full factory wipe + reinit on the next boot
rather than any attempt to read an older format.

**Why this priority**: The no-migration policy keeps the firmware simple and avoids fragile
read-old-format branches; the build-profile-byte gate makes data-breaking changes safe by wiping
deterministically.

**Independent Test**: Flash a build whose profile byte differs from the persisted one (e.g. toggle
`DEBUG_MODE` or `FEATURE_SECURE_SERIAL`), boot, and confirm a full factory wipe + reinit occurs.

**Acceptance Scenarios**:

1. **Given** a persisted build-profile byte that differs from the running firmware's, **When** the
   device boots, **Then** it performs a full factory wipe + reinit before becoming usable.
2. **Given** a breaking on-device format change, **When** the device boots against pre-existing
   data, **Then** it wipes and reinitialises to defaults rather than migrating the old format.
3. **Given** any wipe path, **When** the device reinitialises, **Then** no version-detection or
   read-old-format fallback is used (no migration code).

---

### Edge Cases

- **Interrupted duress wipe**: if power is lost mid-wipe, the boot marker remains absent and the
  wipe re-runs on next boot; the device cannot be left half-wiped.
- **Build-profile mismatch / format change**: a mismatch of the build-profile byte (or a breaking
  on-device format) triggers a full factory wipe + reinit on next boot (no migration).
- **Tampered attestation identity**: the attestation-key public-key hash mirrored in NVS supports
  tamper detection; the attestation key is regenerated on wipe (PIN-record reinit on a failed slot-0
  signature check is owned by spec 002).
- **Secure-element slot in error**: a module whose secure-element slot is in an error state fails to
  start; the slot map (`main/tropic_slot_map.h`) is the authoritative allocation that the wipe and
  reinit honour.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-070** *(self-destruct, referenced by FR-004)*: On duress trigger the device MUST erase the
  NVS boot marker and reboot; on the next boot it MUST run a full factory wipe — NVS reinit, all
  TROPIC01 ECC slots 0–31 deleted, all R-Memory slots 0–511 erased, attestation key regenerated.
  Firmware image and off-badge exports are out of scope of the wipe.
  [NEEDS CLARIFICATION: exact byte size per R-Memory slot — docs say ~444 B guaranteed, up to ~475 B
  runtime-dependent; defer to hardware/datasheet. (baseline B6)]
  [NEEDS CLARIFICATION: orphaned-module NVS cleanup trigger — what counts as a module "no longer
  existing" (compile-time removal vs runtime disable). (baseline B7)]

### Key Entities *(include if feature involves data)*

- **NVS store** — partition `nvs` (~287 KB) holding OS settings, per-module state, WiFi config, the
  module enable list (comma-separated, `mod_`-prefixed), the attestation-key hash, the
  boot/build-profile marker, the cross-sign key store, and plugin NVS (`plg_`/`plugin_` namespaces);
  reinitialised by a duress wipe or a build-profile mismatch.
- **TROPIC01 ECC slots (0–31)** — private keys: attestation (0), GPG (1–3), CA (4), FIDO2 (5–30),
  plugin pool (31); all deleted by a duress/factory wipe.
- **TROPIC01 R-Memory (0–511)** — PIN record (0), GPG (1–3), CA (4), FIDO2 metadata (5–31),
  TOTP/HOTP/CR (32–131), password vault (132–500), plugin pool (501–511); all erased by a
  duress/factory wipe. Slot byte size is per baseline B6.
- **Boot / build-profile marker** — NVS marker that gates the wipe: erased on a duress trigger so
  the wipe runs on next boot, and reseeded only after the wipe completes (crash-safe re-run);
  carries the build-profile byte whose mismatch forces a wipe + reinit.
- **Attestation / Device Identity Key** — P-256 key in ECC slot 0 whose public-key hash is mirrored
  in NVS for tamper detection; regenerated on every wipe. (Signs the PIN record — that linkage is
  owned by spec 002.)
- **Slot map** — `main/tropic_slot_map.h`, the authoritative ECC/R-Memory allocation that the wipe
  and reinit honour.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: After a duress trigger and the subsequent boot, no previously stored secret (across
  NVS, all ECC slots 0–31, and all R-Memory slots 0–511) is recoverable on-device, and the
  attestation key has been regenerated.
- **SC-002**: A wipe interrupted by power loss re-runs to completion on the next boot 100% of the
  time; the device is never left in a half-wiped state.
- **SC-003**: A boot with a mismatched build-profile byte (or a breaking on-device format) triggers
  a full factory wipe + reinit before the device becomes usable 100% of the time, with no
  read-old-format / migration path exercised.
- **SC-004**: The firmware image and any off-badge exports are never erased by the wipe (only
  on-device data is affected).

## Assumptions

- The duress *trigger* (duress-PIN entry, default-off) is owned and verified by spec
  `002-lock-pin-duress`; this spec owns only the wipe *mechanics* the trigger invokes.
- This is a pre-1.0 system: every firmware flash MAY wipe all on-device data, and there is NO
  migration code — breaking format changes wipe and reinit (baseline NFR-002). Holders are expected
  to keep off-badge backups of critical material.
- Hardware-backed acceptance (full wipe coverage, crash-safe re-run) is verified on a CDC Badge
  v1.0/v1.1 with a TROPIC01 secure element; the host test tier cannot exercise secure-element-bound
  state.
- The slot allocation in `main/tropic_slot_map.h` is authoritative; the exact R-Memory slot byte
  size (baseline B6) defers to the TROPIC01 datasheet.
