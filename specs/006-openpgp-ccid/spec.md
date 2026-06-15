# Feature Specification: OpenPGP CCID Smartcard (GPG / SSH)

**Feature Branch**: `006-openpgp-ccid`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the OpenPGP
CCID smartcard capability over USB: on-chip key generation, signing, decryption, SSH authentication,
public-key export, and the PW1/PW3 PIN model.

> **Source of truth**: This spec lifts requirements FR-040..043 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); FR numbers are preserved as cross-references. Code is
> the ground truth for current behaviour. The owning components are `mod_gpg` and `openpgp`. The GPG
> cross-signing send path (FR-044) is a separate, Provisional/WIP capability owned by spec
> `007-gpg-cross-signing` and is out of scope here.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Generate OpenPGP keys on-chip (Priority: P1)

The holder generates an OpenPGP key triple (SIG/DEC/AUT) entirely on the device; private keys are
created on-chip and never exported.

**Why this priority**: Without on-chip keys the card has nothing to sign, decrypt, or authenticate
with; on-chip generation is the security premise of the device.

**Independent Test**: Run key generation on-device with a name and curve, then confirm the SIG/AUT
keys use the chosen curve and the DEC key is fixed to P-256, and that no private key is exported.

**Acceptance Scenarios**:

1. **Given** the GPG menu, **When** the holder runs key generation (name, optional email, curve
   Ed25519 or P-256), **Then** SIG, DEC and AUT keys are created on-chip (DEC fixed to P-256) and
   never exported.
2. **Given** generated keys, **When** the holder exports the public key, **Then** the public key is
   emitted as PEM to serial and a QR code on the display, and no private-key material leaves the
   device.

---

### User Story 2 - Use the badge as an OpenPGP smartcard (Priority: P1)

The holder plugs the badge into a host and uses it as an OpenPGP CCID smartcard for signing,
decryption and SSH authentication, gated by the OpenPGP PINs.

**Why this priority**: Acting as a recognized smartcard for gpg/ssh is the primary external use of
the OpenPGP capability.

**Independent Test**: Connect the badge over USB to a host with gpg-agent and confirm
`gpg --card-status` enumerates the card; then perform a sign/decrypt/SSH-auth gated by PW1.

**Acceptance Scenarios**:

1. **Given** a host with gpg-agent, **When** the badge is connected over USB, **Then** it is
   enumerated as an OpenPGP CCID smartcard and can sign/decrypt/authenticate gated by the OpenPGP
   User PIN (PW1) / Admin PIN (PW3).
2. **Given** the card enumerated, **When** the host issues OpenPGP card APDUs (SELECT, GET/PUT DATA,
   VERIFY, PSO SIGN/DECIPHER, INTERNAL AUTHENTICATE, GENERATE ASYMMETRIC KEY PAIR, GET CHALLENGE,
   etc.), **Then** the card responds per the OpenPGP card APDU set over T=1.

---

### User Story 3 - Card PIN model and recovery (PW1 / PW3) (Priority: P2)

Card operations are gated by PW1 (user) and card administration by PW3 (admin), each with 3
attempts; PW1 is admin-resettable while PW3 is terminal.

**Why this priority**: The PIN model is the access-control core of the smartcard; the PW3 terminal
lockout is a deliberate, non-recoverable-without-wipe behaviour that the holder must understand.

**Independent Test**: Exhaust PW1 and confirm it can be reset by PW3; separately, exhaust PW3 on a
test/wipe-acceptable device and confirm card management is terminally blocked until a full device
wipe.

**Acceptance Scenarios**:

1. **Given** the card with default PINs, **When** card operations and administration run, **Then**
   PW1 (user, default `123456`, min 6) gates operations and PW3 (admin, default `12345678`, min 8)
   gates administration, each allowing 3 attempts.
2. **Given** PW1 is blocked by three wrong entries, **When** the admin uses PW3 to reset reference
   data, **Then** PW1 is restored (PW1 is admin-resettable).
3. **Given** PW3 entered wrong three times, **When** card management is attempted, **Then** it is
   terminally blocked with no host-side reset; recovery is only via a full device wipe.

---

### Edge Cases

- **OpenPGP PW3 exhaustion**: three wrong Admin-PIN entries terminally block card management until a
  full device wipe (no host-side reset) — a non-recoverable state by design.
- **DEC key at rest**: the DEC key is a software P-256 key encrypted at rest in R-Memory with
  AES-256-GCM and is the documented exception to "keys never leave the secure element" (decrypted to
  RAM per ECDH op, zeroized after use).
- **Curve switch**: SIG and AUT default to Ed25519 and may be switched to P-256; DEC is fixed to
  P-256 ECDH.
- **PIN hashing**: PW1/PW3 use Iterated+Salted S2K over SHA-256 (the card advertises a KDF-DO and the
  host transmits the S2K hash) — distinct from the badge/FIDO2 PIN hash (owned by spec 002, FR-007).

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-040**: The device MUST present an OpenPGP CCID smartcard (T=1) over USB and implement the
  OpenPGP card APDU set (SELECT, GET/PUT DATA, VERIFY, CHANGE/RESET reference data, PSO
  SIGN/DECIPHER, INTERNAL AUTHENTICATE, GENERATE ASYMMETRIC KEY PAIR, GET CHALLENGE, etc.).
- **FR-041**: It MUST hold three key roles — SIG (ECC slot 1), AUT (ECC slot 3), both Ed25519 by
  default and switchable to P-256, and DEC (fixed P-256 ECDH, software key encrypted at rest in
  R-Memory with AES-256-GCM).
- **FR-042**: Card operations MUST be gated by PW1 (user, default `123456`, min 6) and PW3 (admin,
  default `12345678`, min 8); each allows 3 attempts; PW1 is admin-resettable, PW3 is terminal
  (wipe-only recovery).
- **FR-043**: The device MUST support generating keys on-chip and exporting only the public key (PEM
  to serial + QR on display); private keys MUST never be exported.

### Key Entities *(include if feature involves data)*

- **OpenPGP Key Set** — three key roles: SIG (ECC slot 1, Ed25519 default / P-256), AUT (ECC slot 3,
  Ed25519 default / P-256), and DEC (fixed P-256 ECDH, software key encrypted at rest in R-Memory
  with AES-256-GCM); plus v4 fingerprints, cardholder data, and a signature counter. ECC slots 1-3
  and R-Memory slots 1-3.
- **OpenPGP PINs (PW1 / PW3)** — PW1 (user, default `123456`, min 6) gates card operations and is
  admin-resettable; PW3 (admin, default `12345678`, min 8) gates administration and is terminal
  (three wrong entries block card management until a full device wipe). Both use Iterated+Salted S2K
  over SHA-256.
- **DEC key at rest** — the documented exception to on-chip-only secrecy: decrypted to RAM per ECDH
  operation and zeroized after use.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The badge is recognised as an OpenPGP smartcard by a standard host GPG toolchain
  without custom drivers (`gpg --card-status` enumerates the card).
- **SC-002**: A sign, a decrypt, and an SSH authentication each succeed when gated by a correct PW1,
  and are refused when PW1 is wrong or blocked.
- **SC-003**: PW1 exhaustion (3 wrong entries) is recoverable via PW3 reset; PW3 exhaustion (3 wrong
  entries) is recoverable only by a full device wipe — no host-side reset succeeds.
- **SC-004**: No private-key material (SIG/AUT in the secure element, DEC plaintext) ever leaves the
  device; only the public key is exported (PEM + QR).

## Assumptions

- SIG/AUT private keys live in TROPIC01 ECC slots and never leave the secure element; the DEC key is
  the documented software-key exception, decrypted to RAM per ECDH op and zeroized after use.
- Hardware-backed acceptance (CCID enumeration, sign/decrypt/SSH, PIN semantics) is verified on a CDC
  Badge v1.0/v1.1 over USB with a standard GPG toolchain; host-tier tests can cover only S2K hashing
  and APDU/CRC framing, not secure-element-bound signing.
- The OpenPGP CCID interface and the keyboard/OTP HID modules are mutually exclusive on the single
  keyboard HID slot.
- PW1/PW3 use S2K hashing distinct from the badge/FIDO2 PIN hash; this two-KDF design is
  protocol-driven (owned by spec 002, FR-007) and not an inconsistency.
- The PW3 terminal lockout means full device wipe is the only recovery; holders are expected to keep
  off-badge backups of the public key and any data they cannot regenerate.
