# Feature Specification: Encrypted Backup / Restore

**Feature Branch**: `009-encrypted-backup`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the
passphrase-encrypted backup container: exporting 2FA, password-vault, vCard and OS/WiFi settings to
an encrypted file, importing them best-effort, the AES-256-GCM + PBKDF2 crypto, and the invariant
that secure-element private keys are never included.

> **Source of truth**: This spec lifts requirements FR-060..064 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); FR numbers are preserved as cross-references. Code is
> the ground truth for current behaviour. The owning component is the `cdc_os_ui` BackupManager. The
> per-module data formats (2FA, password vault, vCard, OS settings) are owned by their respective
> capability specs; this spec owns only the container and the export/import orchestration.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Export an encrypted backup (Priority: P1)

The holder exports a passphrase-encrypted backup of their non-secure-element data so it can be kept
off-badge and restored later, without ever exporting private keys.

**Why this priority**: The device is pre-1.0 with no migration code and every flash may wipe data;
an off-badge encrypted backup is the holder's only recovery path for non-key data.

**Independent Test**: Export a backup with a non-empty passphrase and confirm an encrypted
`backup.cdcbak` is written covering 2FA, password vault, vCards and OS/WiFi settings, and that it
contains no secure-element private keys.

**Acceptance Scenarios**:

1. **Given** the Expert → Backup menu (or the serial `BACKUP EXPORT <pass>` command), **When** the
   holder exports with a non-empty passphrase, **Then** an encrypted `backup.cdcbak` is written to
   the plugins partition containing TOTP/HOTP/CR accounts, password-vault entries, vCards and OS/WiFi
   settings (language, display, sleep, timezone, badge text, WiFi credentials, module enable state).
2. **Given** any export, **When** the container is written, **Then** it contains no secure-element
   private keys (FIDO2, GPG/SSH).
3. **Given** an export, **When** the container is produced, **Then** it is AES-256-GCM encrypted with
   a PBKDF2-HMAC-SHA256 key (200,000 iterations, 16-byte random salt) and the binary layout is magic
   `CDCBAK` ‖ version ‖ kdf_iters ‖ salt ‖ nonce ‖ ciphertext ‖ tag with the header as AAD.

---

### User Story 2 - Import an encrypted backup (best-effort) (Priority: P1)

The holder imports a previously exported backup with the correct passphrase; records are merged
best-effort and a per-section tally reports what was imported, failed, or skipped.

**Why this priority**: An exported backup has no value without a working restore path; best-effort
import with an honest tally is what makes recovery usable and trustworthy.

**Independent Test**: Import a backup with the correct passphrase and confirm a per-section
(imported/failed/skipped) tally; then import with a wrong passphrase and confirm a clear failure.

**Acceptance Scenarios**:

1. **Given** an encrypted backup and the correct passphrase, **When** the holder imports it, **Then**
   records are merged best-effort and a per-section tally (imported/failed/skipped) is shown.
2. **Given** an encrypted backup and a wrong passphrase or a corrupt/altered file, **When** the
   holder imports it, **Then** the import fails with a clear error and no partial data is applied
   from undecryptable content.
3. **Given** a backup whose recorded host-API level is newer than the running firmware, **When** the
   holder imports it, **Then** the import is refused.
4. **Given** import of known sections, **When** records are merged, **Then** they are upserted by
   identity (TOTP by account name, password by title, vCard by exact text), and unknown modules or
   mismatched schema versions are skipped.

---

### User Story 3 - Confirm the secrecy boundary (Priority: P2)

The holder relies on the guarantee that no backup ever carries secure-element private keys, so a
leaked backup file cannot expose FIDO2 or GPG/SSH private keys.

**Why this priority**: The backup is the most portable artifact the device produces; if it leaked
private keys it would defeat the device's core secrecy guarantee.

**Independent Test**: Decrypt an exported backup with its passphrase and confirm no FIDO2 or GPG/SSH
private-key material is present in any section.

**Acceptance Scenarios**:

1. **Given** any exported backup, **When** it is decrypted and inspected, **Then** no FIDO2 or
   GPG/SSH private-key material is present (secure-element private keys are excluded by invariant).
2. **Given** WiFi credentials in a backup, **When** the container is at rest, **Then** they are
   present only inside the encrypted container, never as plaintext in the file.

---

### Edge Cases

- **Backup with newer host-API level**: import refuses a backup whose recorded host-API level is
  newer than the running firmware.
- **Wrong passphrase / corrupt file**: AES-256-GCM tag verification fails and import is rejected with
  a clear error; no partial data is applied.
- **Unknown module / mismatched schema version**: that section is skipped and counted in the tally,
  not failed-hard.
- **Empty passphrase**: export requires a non-empty passphrase.
- **Secure-element keys**: never included in any backup, by invariant (FR-062).

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-060**: The device MUST export a passphrase-encrypted backup container (`backup.cdcbak`, base64
  text on the plugins partition) covering 2FA accounts, password-vault entries, vCards and system
  settings (language, display, sleep, timezone, badge text, WiFi credentials, module enable state).
- **FR-061**: The backup MUST be encrypted with AES-256-GCM using a key derived via
  PBKDF2-HMAC-SHA256 (200,000 iterations, 16-byte random salt), with the container header as AAD; the
  binary layout is magic `CDCBAK` ‖ version ‖ kdf_iters ‖ salt ‖ nonce ‖ ciphertext ‖ tag.
- **FR-062**: Secure-element private keys (FIDO2, GPG/SSH) MUST NOT be included in any backup.
- **FR-063**: Import MUST be best-effort: upsert by identity (TOTP by account name, password by
  title, vCard by exact text), skip unknown modules / mismatched schema versions, refuse a backup
  with a newer host-API level, and report aggregate counts.
- **FR-064**: [NEEDS CLARIFICATION: whether backup export requires an unlocked device / badge PIN in
  addition to the passphrase is not documented. (baseline B5)]

### Key Entities *(include if feature involves data)*

- **Encrypted Backup Container** — `backup.cdcbak`, base64 text on the plugins partition; binary
  layout magic `CDCBAK` ‖ version ‖ kdf_iters ‖ salt ‖ nonce ‖ ciphertext ‖ tag, encrypted with
  AES-256-GCM under a PBKDF2-HMAC-SHA256 key (200,000 iterations, 16-byte random salt), header as AAD.
- **Backup Payload** — the cleartext-before-encryption content: TOTP/HOTP/CR accounts, password-vault
  entries, vCards, and OS/WiFi settings (language, display, sleep, timezone, badge text, WiFi
  credentials, module enable state). Excludes all secure-element private keys.
- **Import Tally** — the per-section aggregate result of an import: imported / failed / skipped counts.
- **Recorded host-API level** — the host-API level stored in the container; import refuses a backup
  whose level is newer than the running firmware.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: An encrypted backup exported with a passphrase can be imported only with that same
  passphrase; a wrong passphrase or an altered file is rejected 100% of the time.
- **SC-002**: Import reports an accurate per-section success/failure/skipped tally, and a backup with
  a newer recorded host-API level is refused.
- **SC-003**: No backup file ever contains secure-element private keys (FIDO2, GPG/SSH) — verified by
  inspecting the decrypted container.
- **SC-004**: A round-trip export → import restores 2FA, password-vault, vCard and OS/WiFi settings
  by identity (TOTP by account name, password by title, vCard by exact text) without duplicating
  matching records.

## Assumptions

- The per-module data shapes (2FA, password vault, vCard, OS/WiFi settings) are owned by their
  capability specs; this spec owns only the container format, the crypto, and the export/import
  orchestration.
- Whether export requires an unlocked device or the badge PIN in addition to the passphrase (FR-064 /
  baseline B5) is an open item to be resolved by reading the code, then documented; it is recorded
  here rather than guessed.
- Host-tier tests can cover base64 encode/decode (T-H03) and backup container framing
  (magic/version/kdf_iters/salt/nonce/AAD) plus an AES-256-GCM round-trip (T-H06); the on-device
  export/import (SE-key exclusion, plugins-partition I/O) is verified by HIL plan T-HIL08.
- The container lives on the FAT `plugins` partition alongside plugin files; it is removed by the
  same VFAT/PLUGIN delete or partition reformat paths.
