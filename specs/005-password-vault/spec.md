# Feature Specification: Password Vault

**Feature Branch**: `005-password-vault`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the on-device
password vault and HID auto-type of vault fields.

> **Source of truth**: This spec lifts requirements FR-030..032 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); FR numbers are preserved as cross-references. Code is
> the ground truth for current behaviour. The owning module is `mod_password`. Open item B3
> (at-rest encryption of vault entries) is kept as a clarification.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Create and store a password entry (Priority: P1)

The holder keeps login credentials in an on-device vault, optionally generating a strong random
password on the device.

**Why this priority**: Storing entries is the foundational value of the vault; without it nothing
can be retrieved or typed.

**Independent Test**: Complete the new-entry wizard and confirm the entry is saved and reappears in
the Passwords list.

**Acceptance Scenarios**:

1. **Given** the Passwords menu, **When** the holder completes the new-entry wizard (title, username,
   password or generated password, URL, optional TOTP slot, notes), **Then** the entry is saved.
2. **Given** the new-entry wizard, **When** the holder requests a generated password, **Then** a
   random password is generated on-device and stored with the entry.
3. **Given** a saved entry, **When** the holder reopens the Passwords list, **Then** the entry is
   present with its stored fields.

---

### User Story 2 - Auto-type a stored field to a host (Priority: P2)

The holder types a stored field into a connected host via USB or BLE keyboard emulation, on explicit
confirmation.

**Why this priority**: Auto-type is the main way the vault delivers value to the host; it is gated
by holder confirmation for safety.

**Independent Test**: Connect a HID keyboard target, open an entry, trigger auto-type from the detail
view, and confirm the field is typed into the host.

**Acceptance Scenarios**:

1. **Given** a vault entry and a connected HID keyboard (USB or BLE), **When** the holder triggers
   auto-type from the detail view, **Then** the stored field is sent as keystrokes to the host.
2. **Given** an entry with a linked TOTP slot, **When** the holder views the entry, **Then** the
   linked one-time password is available alongside the credential fields.

---

### Edge Cases

- **Capacity**: vault entries occupy R-Memory slots 132-500 (up to 369 entries); adding beyond
  capacity must fail cleanly.
- **No HID target connected**: triggering auto-type with no connected keyboard target does nothing
  harmful.
- **Secure-element failure**: a secure-element slot in error prevents saving/reading the entry.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-030**: The device MUST store password entries (title, optional username, optional password
  with on-device random generation, optional URL, optional linked TOTP slot, optional notes) in
  secure-element R-Memory (slots 132-500, up to 369 entries).
- **FR-031**: The device MUST type a selected entry's field to a connected USB or BLE HID keyboard on
  holder confirmation.
- **FR-032**: [NEEDS CLARIFICATION: at-rest protection of vault entries within R-Memory — the
  encryption scheme (per-entry vs container, cipher, key) is not documented; one source notes the
  payload is written with header + checksum without an explicit application-level cipher.
  (baseline B3)]

### Key Entities *(include if feature involves data)*

- **Password Vault Entry** — title, optional username, optional password (with on-device random
  generation), optional URL, optional linked TOTP slot, optional notes; stored in secure-element
  R-Memory slots 132-500 (up to 369 entries).
- **Linked TOTP slot** — an optional reference from a vault entry to a 2FA account (owned by spec
  004), surfacing the one-time password alongside the credential fields.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A stored password is delivered to a connected host exactly as entered when auto-type is
  triggered (byte-for-byte keystroke fidelity).
- **SC-002**: A saved vault entry survives a power cycle and reappears with all stored fields intact.
- **SC-003**: The vault never stores more than its 369-entry capacity, and adding beyond capacity
  fails without corrupting existing entries.
- **SC-004**: Auto-type is sent only after explicit holder confirmation from the detail view (never
  unprompted).

## Assumptions

- Vault entries are stored in TROPIC01 R-Memory; hardware-backed acceptance is verified on a CDC
  Badge v1.0/v1.1. The at-rest encryption scheme is unresolved (FR-032 / baseline B3) and must be
  read from code before being asserted.
- Auto-type relies on the USB or BLE HID keyboard interface; the exact keyboard report format and
  inter-keystroke throttling are owned/clarified by the BLE/HID and keypad-UI specs (baseline B11).
- The optional linked TOTP slot references a 2FA account owned by spec `004-2fa-otp`.
