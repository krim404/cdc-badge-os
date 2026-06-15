---
title: ADR-0004 — Two-KDF PIN hashing is protocol-driven
description: Badge/FIDO2 PIN and OpenPGP PINs use different KDFs by protocol necessity.
sidebar:
  order: 4
---

**Status**: accepted
**Source**: reverse-spec Discrepancy D2; FR-007, FR-017; `components/cdc_core/src/PinManager.cpp`; `components/mod_fido2/src/ctap2.cpp`

## Context

The badge stores three PIN secrets: the badge PIN (which also serves as the FIDO2 CTAP2
ClientPIN), the OpenPGP user/admin PINs (PW1/PW3), and the optional duress PIN. Their hash
representations are fixed by the external protocols that consume them, not by an internal
preference:

- **FIDO2 CTAP2 ClientPIN**: the platform transmits exactly `LEFT(SHA-256(PIN), 16)` (the first
  16 bytes of the SHA-256 of the PIN) at verification time. The authenticator never receives the
  plaintext PIN, so it cannot recompute any salted/iterated hash; it can only compare the
  16-byte value the platform sent. In `ctap2.cpp` the verify path decrypts the transmitted hash
  to a 16-byte buffer (`memcpy(decrypted_pin_hash, decrypted, 16)`) and compares it via
  `pin_storage_verify_fido2_hash` (ClientPIN compare at ~lines 2648 and 2884).
- **OpenPGP PW1/PW3**: the card advertises a KDF-DO, so the host transmits an Iterated+Salted S2K
  (RFC 4880) hash over SHA-256. The card must store and compare the S2K form.

Because the badge PIN and the FIDO2 ClientPIN are the same secret (shared via
`pin_storage_{get,verify}_fido2_hash`), the badge PIN representation is bound to the CTAP2 form.

## Decision

PIN hashing is intentionally not uniform; it is determined by protocol:

- The badge PIN (and thus the FIDO2 ClientPIN) is stored as `LEFT(SHA-256(PIN), 16)` —
  `PinManager::computeBadgeHash`.
- OpenPGP PW1/PW3 are stored as Iterated+Salted S2K over SHA-256 with a per-PIN random salt —
  `PinManager::computeKdfHash`.
- The duress PIN (internal only, free choice) also uses S2K via `computeKdfHash`.

Unifying these KDFs is rejected: forcing the badge PIN to S2K would break FIDO2 ClientPIN
verification, and forcing OpenPGP PINs to truncated SHA-256 would break the OpenPGP card KDF-DO.

## Consequences

- Enables: one shared PIN secret usable as both the device unlock and the FIDO2 ClientPIN, plus
  spec-conformant OpenPGP card behaviour.
- Must hold: `computeBadgeHash` must remain `LEFT(SHA-256(PIN), 16)` (unsalted, 16 bytes) for as
  long as it doubles as the ClientPIN hash; the S2K path must keep its per-PIN salt and the
  iteration count stored in the PIN record.
- Cost: the badge PIN hash is unsalted and truncated by protocol constraint; brute-force
  resistance for the badge PIN relies on the on-device retry lockout (ADR-0011), not on KDF cost.
