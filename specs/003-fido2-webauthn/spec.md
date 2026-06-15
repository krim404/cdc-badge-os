# Feature Specification: FIDO2 / WebAuthn Authenticator

**Feature Branch**: `003-fido2-webauthn`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the
FIDO2/WebAuthn (CTAP2/U2F) authenticator capability over USB HID.

> **Source of truth**: This spec lifts requirements FR-010..017 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); FR numbers are preserved as cross-references. Code is
> the ground truth for current behaviour. The owning module is `mod_fido2`. Open item B2
> (`credProtect` parsed/stored but not enforced) is kept as a clarification; B1 (FIDO2 ClientPIN ==
> badge PIN) is recorded as RESOLVED.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Register a passkey from a relying party (Priority: P1)

A host computer registers a new WebAuthn credential against the badge over USB; the badge generates
the key pair on-chip and requires on-device user presence.

**Why this priority**: Passkey registration is the entry point for WebAuthn; without it no later
sign-in is possible. FIDO2/WebAuthn is the primary reason the device exists as a security key.

**Independent Test**: From a WebAuthn relying party, initiate registration, confirm the on-device
prompt, and confirm a credential is returned to the host.

**Acceptance Scenarios**:

1. **Given** a host initiating WebAuthn registration, **When** the badge shows the "Register Key"
   prompt with the relying-party name and the holder presses confirm within the prompt window,
   **Then** an on-chip key pair is generated and a credential is returned to the host.
2. **Given** an existing credential for the same relying party, **When** a new registration is
   requested, **Then** the badge warns about overwriting before proceeding.
3. **Given** a registration request, **When** the holder does not confirm within the prompt window,
   **Then** no credential is created.

---

### User Story 2 - Authenticate (sign in) with a passkey (Priority: P1)

A host requests an assertion against a previously registered credential; the badge confirms user
presence and returns a signed assertion with an incrementing counter.

**Why this priority**: Sign-in is the everyday use of the authenticator; it must produce valid,
counter-protected assertions.

**Independent Test**: Register a credential, then request an assertion from the relying party,
confirm presence on the badge, and confirm the assertion is accepted and the counter increments.

**Acceptance Scenarios**:

1. **Given** a registered credential, **When** the host requests an assertion and the holder
   confirms presence, **Then** the badge returns a signed assertion and the per-credential signature
   counter increments.
2. **Given** a relying party that requires user verification, **When** the WebAuthn ClientPIN flow
   runs, **Then** the badge enforces PIN verification according to CTAP2 ClientPIN v2 using the same
   secret as the badge PIN.
3. **Given** a device-selection probe (CTAP `selection`), **When** the host issues it, **Then** the
   badge requires user presence only (no PIN).

---

### User Story 3 - Manage stored credentials (Priority: P2)

The host enumerates and manages resident credentials via CTAP2 credentialManagement, within the
device's slot capacity.

**Why this priority**: Credential management lets the holder see and remove resident keys; it is
secondary to register/authenticate but required for a complete authenticator.

**Independent Test**: Register one or more resident credentials, then enumerate them via
credentialManagement and confirm the metadata matches.

**Acceptance Scenarios**:

1. **Given** resident credentials on the device, **When** the host runs credentialManagement
   enumerate, **Then** the badge returns the stored credentials with their relying-party and user
   metadata.
2. **Given** the device near its credential capacity, **When** more than 26 credentials are
   requested, **Then** the device does not exceed its ECC slot capacity (slots 5-30).
3. **Given** a host querying authenticator info, **When** getInfo is issued, **Then** the badge
   advertises FIDO 2.0, FIDO 2.1 and U2F V2 with `rk`, `up`, `clientPin`, `credMgmt`,
   `pinUvAuthToken`, and reports `uv` as not provided.

---

### Edge Cases

- **Unsupported commands**: largeBlobs, authenticatorConfig and bioEnrollment report unsupported
  rather than failing opaquely.
- **credProtect**: levels 1-3 are parsed/stored/reported but NOT enforced at assertion time
  (documented gap — see FR-016).
- **Secure-element failure**: if the FIDO2 ECC slot is in error, the module fails to start.
- **Counter monotonicity**: the per-credential signature counter must never regress across
  assertions, even after power cycles.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-010**: The device MUST act as a CTAP2 authenticator over USB HID, advertising FIDO 2.0,
  FIDO 2.1 and U2F V2, with capabilities including resident keys (`rk`), user presence (`up`),
  client PIN (`clientPin`), credential management (`credMgmt`) and `pinUvAuthToken`; built-in user
  verification (`uv`) is not provided.
- **FR-011**: The device MUST implement the CTAP2 commands makeCredential, getAssertion, getInfo,
  clientPIN, reset, getNextAssertion, credentialManagement and selection; largeBlobs,
  authenticatorConfig and bioEnrollment MUST report unsupported.
- **FR-012**: Credential private keys MUST be generated on-chip (ES256/P-256 or EdDSA/Ed25519) and
  stored in TROPIC01 ECC slots; the device MUST support up to 26 credentials with 64-byte credential
  IDs. The effective capacity is the 26 ECC slots 5-30 assigned by the module registry; the
  `FIDO2_MAX_CREDENTIALS` constant (32) is an upper bound that the slot range, not the constant,
  caps at 26.
- **FR-013**: Each FIDO2 operation MUST require on-device user-presence confirmation; a
  device-selection probe (CTAP `selection`) MUST require user presence only (no PIN).
- **FR-014**: Per-credential monotonic signature counters MUST persist in the secure element and
  increment on each assertion.
- **FR-015**: Attestation MUST use the `packed`, self-signed, per-device format with the chip-bound
  ECC slot 0 key and a fixed AAGUID (`CDCBAD6E39C30001BAD6E00100000001`).
- **FR-016**: [NEEDS CLARIFICATION: `credProtect` levels 1-3 are parsed/stored/reported but NOT
  enforced at assertion time (documented gap). Is this the intended end state or a known defect?
  (baseline B2)]
- **FR-017**: The FIDO2 ClientPIN and the badge PIN MUST be the **same secret**: the FIDO2 ClientPIN
  hash is the badge PIN hash `LEFT(SHA-256(PIN), 16)`, shared via the
  `pin_storage_{get,verify}_fido2_hash` bridge. *(Code-confirmed; resolves former open question B1.
  This sharing is also why the badge PIN hashing is fixed by CTAP2 — see FR-007 in spec 002.)*

### Key Entities *(include if feature involves data)*

- **FIDO2 Credential** — on-chip key pair (ES256/P-256 or EdDSA/Ed25519), credential ID (64 bytes),
  relying-party identity, user handle, resident flag, and a per-credential monotonic signature
  counter; private key in TROPIC01 ECC slots 5-30 (up to 26), metadata in R-Memory slots 5-31.
- **Attestation / Device Identity Key** — chip-bound P-256 key in ECC slot 0 used for the `packed`
  self-signed attestation; fixed AAGUID `CDCBAD6E39C30001BAD6E00100000001` (key lifecycle owned by
  spec 013).
- **ClientPIN secret** — the badge PIN, transmitted by the platform as `LEFT(SHA-256(PIN), 16)`;
  the authenticator never sees the plaintext (record/hashing owned by spec 002, FR-006/FR-007).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A WebAuthn registration and a subsequent sign-in each complete with a single on-device
  confirmation within the displayed prompt window, and the signature counter increases on every
  successful sign-in.
- **SC-002**: The badge is enumerated and used as a standard FIDO2/WebAuthn authenticator by a
  standard relying party without custom drivers.
- **SC-003**: The per-credential signature counter is strictly monotonic across assertions and
  power cycles (never regresses).
- **SC-004**: The device never stores more credentials than its ECC slot capacity (≤ 26), and
  credential management enumeration returns metadata consistent with what was registered.

## Assumptions

- ClientPIN verification reuses the badge PIN secret; the badge PIN record and its hashing are owned
  by spec `002-lock-pin-duress` (FR-006/FR-007) and referenced here, not re-specified.
- Hardware-backed acceptance (on-chip keygen, counter persistence, attestation) is verified on a CDC
  Badge v1.0/v1.1 with TROPIC01 over USB; host-tier tests can cover only CBOR encoders and protocol
  framing, not secure-element-bound keygen/signing.
- `credProtect` non-enforcement (FR-016) is recorded as a known documented gap pending a decision on
  whether to enforce it at assertion time.
