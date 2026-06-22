# Feature Specification: GPG Cross-Signing (Provisional / WIP)

**Feature Branch**: `007-gpg-cross-signing`

**Created**: 2026-06-14

**Status**: Draft (derived from baseline `001-current-system-spec`)

**Input**: Per-capability decomposition of the CDC Badge OS baseline. This spec owns the GPG
cross-signing capability: exchanging OpenPGP public keys between badges over BLE, producing an
RFC 4880 certification signature with the on-chip SIG key, returning that certification to the
key's owner over BLE, collecting third-party certifications on the own key, and exporting the
armored own public-key block (with collected certifications) for `gpg --import`.

> **⚠ PROVISIONAL (NOT YET HARDWARE-VERIFIED)**: This capability is normative but **Provisional**.
> The cross-sign flow is bidirectional and fully UI- and serial-driven: **Send Key** transfers the
> own public key, **Cross-Sign** certifies a received key, **Send Signature** returns the
> certification to its owner, **My Certifications** lists certifications collected on the own key,
> and **Export Public** emits the own key with those certifications attached. The signing curve is
> read from the on-card SIG key. Statements below describe the design as built; the end-to-end
> acceptance (SC-002; HIL plan T-HIL03) is **not yet verified on hardware** and against a standard
> GPG toolchain, and this capability MUST stay flagged Provisional until that verification runs.

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

1. **Given** a stored peer key, **When** the holder runs `GPG RECV_CROSS_SIGN <i>`, **Then** the device
   builds an RFC 4880 certification preimage and signs it with the on-chip SIG key (ECC slot 1),
   recording the certification signature and setting the verified flag.
2. **Given** a peer key on a P-256 curve, **When** it is cross-signed, **Then** the certification
   signature reflects the actual curve. The signing algorithm (EdDSA or ECDSA) is selected from the
   on-card SIG key's curve, read from the secure element via `gpg_get_status()`.

---

### User Story 3 - Return the certification and export the own key (Priority: P2)

The holder returns a produced certification to the key's owner over BLE, and exports the own public
key (with all collected certifications) so the web of trust can be rebuilt on a host.

**Why this priority**: Without a return path and an own-key export the certification stays trapped
on the signer; returning it and re-exporting is what makes the cross-signature usable off-badge.

**Independent Test (Provisional)**: After cross-signing a peer key, run `GPG RECV_EXPORT <i>` and
confirm a well-formed armored block is emitted; use the on-badge **Send Signature** action to return
it over BLE; import a returned certification and confirm `GPG EXPORT` emits the own key carrying it.

**Acceptance Scenarios**:

1. **Given** a cross-signed peer key, **When** the holder runs `GPG RECV_EXPORT <i>`, **Then** the
   device emits an armored OpenPGP public-key block containing the certification signature over
   serial. (`RECV_EXPORT` also works on a not-yet-cross-signed key, emitting the encryptable key
   without the cross-signature.)
2. **Given** a cross-signed peer key, **When** the holder invokes the on-badge **Send Signature**
   action, **Then** the certification is transferred to the key's owner over BLE; the
   owner accepts it only if it targets the owner's own key and stores it under My Certifications.
3. **Given** one or more collected certifications on the own key, **When** the holder runs
   `GPG EXPORT` (or **Export Public**), **Then** the device emits the armored own public key with
   every collected certification packet attached.

---

### Edge Cases

- **Fingerprint mismatch**: a received public-key payload is rejected unless its v4 fingerprint
  recomputes from the transmitted curve, public key and creation time, so a certification cannot
  bind to a key the badge cannot reproduce.
- **Wrong target**: a returned certification is rejected unless it targets the receiver's own SIG
  key fingerprint.
- **Store capacity**: the received-peer-key store holds at most 128 keys; the self-cert store holds
  at most 16 certifications (both NVS-backed).
- **No SIG key present**: cross-signing requires an on-chip SIG key (owned by spec 006); without a
  generated OpenPGP key set there is nothing to certify with.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-044** *(Provisional)*: The device MUST support exchanging a peer badge's public key over BLE
  (with a fingerprint-validating wire record), producing an RFC 4880 certification signature
  (cross-signing) with the on-card SIG key, returning that certification to the key's owner over
  BLE, collecting third-party certifications on the own key, and exporting the armored own public-key
  block with the collected certifications attached. The signing algorithm is selected from the
  on-card SIG key's curve.

### Key Entities *(include if feature involves data)*

- **Received Peer Key (cross-signing)** — a record of another badge's OpenPGP public key: peer curve,
  public key, key creation time, v4/v5 fingerprints, user-id, receive timestamp, optional
  certification signature, signature creation time, and a verified flag. NVS-backed, ≤ 128 keys.
- **Certification Signature** — an RFC 4880 certification (cross-)signature produced over the peer
  key's certification preimage using the on-chip SIG key (ECC slot 1, owned by spec 006).
- **Certification Return Record** — a transport record carrying a produced certification back to the
  certified key's owner: target fingerprint, issuer fingerprint, issuer user-id, and the verbatim
  signature packet. MIME `application/pgp-signature`.
- **Self-Certification** — a third-party certification on the own key, persisted (issuer
  fingerprint, issuer user-id, receive timestamp, signature packet). NVS-backed, ≤ 16.
- **Armored Public-Key Block** — the OpenPGP-armored export of either a certified peer key
  (`GPG RECV_EXPORT <i>`) or the own key with its collected certifications (`GPG EXPORT`).

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
