# Feature Specification: GPG Cross-Signing (Provisional / WIP)

**Feature Branch**: `007-gpg-cross-signing`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the GPG
cross-signing capability: receiving a peer badge's OpenPGP public key over BLE, producing an
RFC 4880 certification signature with the on-chip SIG key, and exporting the resulting armored
public-key block over serial.

> **⚠ PROVISIONAL (WIP, NOT HARDWARE-VERIFIED)**: This capability is normative but **Provisional**.
> Per the baseline Clarifications (Session 2026-06-14), the GPG cross-sign send path (FR-044) is
> documented as work-in-progress: the on-device "Send Key" action is a placeholder, the BLE send
> path exists but is not UI-driven, and the behaviour is **not yet verified on hardware**. Hardware
> acceptance (SC-002; HIL plan T-HIL03) is **non-blocking** until verified on hardware, and this
> capability MUST stay flagged WIP until then. Statements below describe the design as built; treat
> them as provisional until a hardware run confirms them.

> **Source of truth**: This spec lifts requirement FR-044 faithfully from the baseline
> (`specs/001-current-system-spec/spec.md`); the FR number is preserved as a cross-reference. Code
> is the ground truth for current behaviour. The owning component is `mod_gpg`. The OpenPGP CCID
> smartcard capability (FR-040..043) is owned by spec `006-openpgp-ccid` and is out of scope here;
> the BLE controller invariant (FR-090) is owned by spec `011-ble-controller-hid`.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Receive a peer badge's public key over BLE (Priority: P1)

The holder receives another badge holder's OpenPGP public key over BLE so it can later be
certified. The received key is stored on the device with the peer's identity for review.

**Why this priority**: Cross-signing cannot begin without first holding the peer's public key;
receipt is the entry point of the whole flow.

**Independent Test (Provisional)**: With two badges, transfer a peer public key over the dedicated
BLE service and confirm the receiver stores the peer curve, public key, v4 fingerprint and user-id.

**Acceptance Scenarios**:

1. **Given** a peer badge offering its OpenPGP public key over BLE, **When** the transfer completes
   over the encryption-required characteristics, **Then** the receiver stores the peer's curve,
   public key, v4 fingerprint, user-id and receive timestamp as a received-peer-key record.
2. **Given** the received-peer-key store at capacity (≤ 128 keys), **When** a further key is
   received, **Then** the device does not exceed the store limit.

---

### User Story 2 - Certify a received peer key (cross-sign) (Priority: P1)

The holder selects a stored peer key and produces an RFC 4880 certification signature over it using
the on-chip SIG key, marking the peer key as verified/certified.

**Why this priority**: Producing the certification signature is the core value of cross-signing —
it is the act of attesting to a peer's key.

**Independent Test (Provisional)**: Select a received peer key, run cross-sign, and confirm a
certification signature is produced by the SIG ECC slot and attached to the stored record.

**Acceptance Scenarios**:

1. **Given** a stored peer key, **When** the holder runs `GPG CROSS_SIGN <i>`, **Then** the device
   builds an RFC 4880 certification preimage and signs it with the on-chip SIG key (ECC slot 1),
   recording the certification signature and setting the verified flag.
2. **Given** a peer key on a P-256 curve, **When** it is cross-signed, **Then** the certification
   signature reflects the actual curve. [NEEDS CLARIFICATION: cross-sign signatures always label the
   curve as EdDSA from the status snapshot, so P-256 signatures may be mislabeled and are unverified
   on hardware. (baseline B13)]

---

### User Story 3 - Export the certified public-key block (Priority: P2)

The holder exports the certified peer key as an armored OpenPGP public-key block over serial so it
can be published or re-imported on a host.

**Why this priority**: Without an export path the certification stays trapped on the device; export
is what makes the cross-signature usable off-badge.

**Independent Test (Provisional)**: After cross-signing a peer key, run the export command and
confirm a well-formed armored public-key block is emitted over serial.

**Acceptance Scenarios**:

1. **Given** a cross-signed peer key, **When** the holder runs `GPG EXPORT_SIGNED <i>`, **Then** the
   device emits an armored OpenPGP public-key block containing the certification signature over
   serial.
2. **Given** the on-device "Send Key" action, **When** the holder invokes it, **Then** [NEEDS
   CLARIFICATION: the on-device "Send Key" action is a placeholder; the BLE send path exists but is
   not UI-driven. Confirm intended scope. (baseline B14, FR-044)]

---

### Edge Cases

- **Curve mislabeling (B13)**: cross-sign signatures always label the curve as EdDSA from the status
  snapshot, so P-256 signatures may be mislabeled; this is unverified on hardware.
- **Send path WIP (B14)**: the on-device "Send Key" action is a placeholder; the BLE send path is
  not UI-driven and the end-to-end flow is not hardware-verified.
- **Store capacity**: the received-peer-key store holds at most 128 keys (NVS-backed).
- **No SIG key present**: cross-signing requires an on-chip SIG key (owned by spec 006); without a
  generated OpenPGP key set there is nothing to certify with.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-044** *(Provisional / WIP)*: The device MUST support receiving a peer badge's public key over
  BLE and producing an RFC 4880 certification signature (cross-signing), exportable as an armored
  public-key block over serial. [NEEDS CLARIFICATION: the on-device "Send Key" action is a
  placeholder; the BLE send path exists but is not UI-driven. Confirm intended scope. (baseline B14)]
  [NEEDS CLARIFICATION: cross-sign signatures always label the curve as EdDSA from the status
  snapshot, so P-256 signatures may be mislabeled and are unverified on hardware. (baseline B13)]

### Key Entities *(include if feature involves data)*

- **Received Peer Key (cross-signing)** — a record of another badge's OpenPGP public key: peer curve,
  public key, v4 fingerprint, user-id, receive timestamp, optional certification signature, and a
  verified flag. NVS-backed, ≤ 128 keys.
- **Certification Signature** — an RFC 4880 certification (cross-)signature produced over the peer
  key's certification preimage using the on-chip SIG key (ECC slot 1, owned by spec 006).
- **Armored Public-Key Block** — the OpenPGP-armored export of a certified peer key emitted over the
  serial console (`GPG EXPORT_SIGNED <i>`).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A peer OpenPGP public key received over BLE is stored with its curve, public key, v4
  fingerprint and user-id, and the store never exceeds 128 keys.
- **SC-002** *(Provisional, HIL non-blocking)*: On hardware, two badges complete a cross-sign round
  trip — receive peer key, certify it with the on-chip SIG key, and export an armored public-key
  block that a standard GPG toolchain can import. Verified by HIL plan T-HIL03; non-blocking until
  the WIP send path and curve-labeling (B13/B14) are resolved on hardware.
- **SC-003**: The exported armored block carries the certification signature and no private-key
  material; the on-chip SIG private key never leaves the secure element.

## Assumptions

- This capability is **Provisional (WIP, not hardware-verified)**; its hardware acceptance is
  non-blocking per the baseline Clarifications (Session 2026-06-14) until verified on hardware.
- The on-chip SIG key used for certification is owned and provisioned by spec `006-openpgp-ccid`
  (ECC slot 1); cross-signing reuses it and does not generate its own key material.
- The BLE transport for receiving the peer key is the dedicated GPG cross-signing GATT service with
  encryption-required characteristics; the single-controller invariant is owned by spec
  `011-ble-controller-hid`.
- The curve-labeling defect (B13) and the placeholder "Send Key" action (B14) are open items to be
  resolved by reading the code and/or a hardware run, then documented; they are recorded here rather
  than guessed.
- Host-tier tests can cover RFC 4880 certification framing and CRC-24 armor checks (T-H02), but not
  the secure-element-bound signature or the BLE round trip.
