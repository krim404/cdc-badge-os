# Feature Specification: 2FA — TOTP / HOTP / Challenge-Response

**Feature Branch**: `004-2fa-otp`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns one-time
password accounts: TOTP, HOTP, and HMAC challenge-response (CR).

> **Source of truth**: This spec lifts requirements FR-020..023 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); FR numbers are preserved as cross-references. Code is
> the ground truth for current behaviour. The owning module is `mod_2fa`.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Add and view a TOTP account (Priority: P1)

The holder stores an OATH TOTP secret via an on-device wizard and reads the time-based one-time
password on the display with a live countdown.

**Why this priority**: TOTP is the most common 2FA use; adding and viewing a code is the core value
of the module.

**Independent Test**: Add a TOTP account via the on-device wizard, then open it and confirm a
6-digit code with a countdown is displayed.

**Acceptance Scenarios**:

1. **Given** the 2FA menu, **When** the holder completes the add-account wizard (type, name, Base32
   secret, digits, algorithm, period), **Then** the account is saved to the secure element and
   confirmed.
2. **Given** a saved TOTP account, **When** the holder opens it, **Then** the current code, a
   progress bar and a per-second countdown are shown.
3. **Given** a TOTP account configured with SHA1/SHA256/SHA512 and 6/7/8 digits, **When** it is
   viewed, **Then** the code matches what a standard authenticator computes for the same secret and
   parameters.

---

### User Story 2 - Use HOTP counter-based codes (Priority: P2)

The holder maintains a counter-based HOTP account; advancing it yields the next code and the counter
persists.

**Why this priority**: HOTP is less common than TOTP but required for full OATH coverage; its
defining behaviour is the persistent counter.

**Independent Test**: Add a HOTP account, advance the counter, and confirm a new code is shown and
the counter persists across a reopen.

**Acceptance Scenarios**:

1. **Given** a saved HOTP account, **When** the holder advances the counter, **Then** a new code is
   shown and the counter persists.
2. **Given** a HOTP account whose counter was advanced, **When** the device is power-cycled and the
   account is reopened, **Then** the persisted counter value is retained.

---

### User Story 3 - Answer a challenge-response over USB/serial/BLE (Priority: P2)

A host sends a challenge to a CR account and the badge returns an HMAC response (HMAC-SHA1, or
HMAC-SHA256 over serial/BLE), optionally gated by a touch confirmation.

**Why this priority**: Challenge-response supports Yubico-style integrations; it is the only OTP
type that interacts with a host rather than the display.

**Independent Test**: Configure a CR account, send a 64-byte challenge over the OTP HID interface,
confirm touch if required, and confirm a 20-byte HMAC-SHA1 response is returned.

**Acceptance Scenarios**:

1. **Given** a challenge-response account, **When** a host sends a 64-byte challenge over the OTP HID
   interface (and touch is confirmed if required), **Then** the badge returns a 20-byte HMAC-SHA1
   response.
2. **Given** a CR account with touch required, **When** a challenge arrives and touch is not
   confirmed, **Then** the response is not produced.
3. **Given** a CR account, **When** a challenge is delivered over serial or BLE instead of USB HID,
   **Then** the badge produces the response for that account's algorithm (20-byte HMAC-SHA1, or
   32-byte HMAC-SHA256 for a SHA256 account).

---

### Edge Cases

- **Capacity**: TOTP/HOTP/CR accounts share R-Memory slots 32-131 (up to 100 accounts across all
  three types); adding beyond capacity must fail cleanly.
- **Invalid Base32**: a malformed Base32 secret entered in the wizard must be rejected.
- **Secure-element failure**: a secure-element slot in error prevents saving/reading the account.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-020**: The device MUST support TOTP, HOTP and HMAC challenge-response (CR) accounts,
  configured via an on-device wizard and via serial commands, stored in secure-element R-Memory
  (slots 32-131, up to 100 accounts shared across the three types).
- **FR-021**: TOTP accounts MUST support SHA1/SHA256/SHA512, 6/7/8 digits and a configurable period;
  the view MUST show the current code with a progress bar and countdown.
- **FR-022**: HOTP accounts MUST persist a counter that advances on use.
- **FR-023**: CR accounts MUST answer a 64-byte challenge with an HMAC digest matching the account's
  algorithm (20-byte HMAC-SHA1 or 32-byte HMAC-SHA256) over serial or BLE; the USB OTP HID (Yubico
  slot-2) interface returns a 20-byte HMAC-SHA1 response and accepts SHA1 accounts only. Optional
  per-entry touch confirmation applies.

### Key Entities *(include if feature involves data)*

- **TOTP Account** — OATH secret (Base32), name, issuer, algorithm (SHA1/SHA256/SHA512), digits
  (6/7/8), period; rendered as a time-based code with progress bar and countdown.
- **HOTP Account** — OATH secret (Base32), name, issuer, algorithm, digits, and a persistent counter
  that advances on use.
- **Challenge-Response (CR) Account** — HMAC-SHA1 or HMAC-SHA256 key, optional per-entry
  touch-required flag; answers a 64-byte challenge with a 20-byte (SHA1) or 32-byte (SHA256)
  response. The USB OTP HID path is SHA1-only (20-byte).
- **Account store** — all TOTP/HOTP/CR accounts share secure-element R-Memory slots 32-131 (up to
  100 accounts total). A CR account MAY be linked from a password-vault entry (owned by spec 005).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A newly added TOTP account displays a code that a standard authenticator app accepts
  for the same secret (interoperable codes), for each supported algorithm and digit length.
- **SC-002**: A HOTP counter advances by exactly one per use and survives a power cycle 100% of the
  time.
- **SC-003**: A 64-byte challenge to a CR account returns an HMAC response that matches a reference
  HMAC computation for the account's algorithm (20-byte SHA1, or 32-byte SHA256 over serial/BLE);
  the USB OTP HID path returns a 20-byte HMAC-SHA1 response.
- **SC-004**: The combined TOTP/HOTP/CR account count never exceeds the 100-account capacity, and
  adding beyond capacity fails without corrupting existing accounts.

## Assumptions

- OATH secrets and CR keys are stored in TROPIC01 R-Memory; hardware-backed acceptance is verified
  on a CDC Badge v1.0/v1.1. Host-tier tests can cover the OTP HID CRC framing and HMAC/OATH
  algorithms, not secure-element-backed storage.
- The OTP HID interface (USB feature reports) and the keyboard HID interface are mutually exclusive
  on the single keyboard HID slot; CR over USB HID assumes the OTP HID module is the active occupant.
- A standard authenticator app and a reference HMAC-SHA1/SHA256 implementation are available as
  interoperability oracles.
