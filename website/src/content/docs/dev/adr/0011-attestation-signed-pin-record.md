---
title: ADR-0011 — Attestation-signed PIN record in R-Memory slot 0
description: The PIN record is bound to the device by a slot-0 attestation signature.
sidebar:
  order: 11
---

**Status**: accepted
**Source**: spec 002 (PIN storage); FR-006; `components/cdc_core/src/PinManager.cpp`

## Context

The combined PIN record (badge hash, OpenPGP PW1/PW3 salts/hashes, KDF iteration count, retry
flags, and the optional duress PIN material) is stored in TROPIC01 R-Memory slot 0. ECC slot 0
holds the chip-bound P-256 attestation key. The badge-PIN retry counter lives in RAM; only the
locked flag and the PW1/PW3 counters are persisted, so a power cycle mid-verify cannot resurrect
the badge counter.

## Decision

The PIN record is bound to the device by an attestation signature. On save, `PinManager`
appends a P-256 ECDSA signature over the payload bytes `[0..PAYLOAD_SIZE)` produced by the
chip-bound key in ECC slot 0 (`se->ecdsaSign(ATTESTATION_ECC_SLOT, …)`). On load, only the
signed format is accepted: wrong magic, wrong length, or an invalid signature
(`verify_payload_signature`) causes a silent reinit to defaults, which re-signs the slot freshly.

- Signature algorithm: ECDSA over P-256 (SECP256R1), 64-byte raw `r || s`, over `SHA-256` of the
  payload.
- A regenerated or tampered slot-0 key fails verification and triggers reinit to defaults; this
  is not migration (per ADR-0005).
- Random-`k` ECDSA means re-saving the same payload yields a different signature; only
  verification matters.

## Consequences

- Enables: tamper detection of the PIN record and binding to the specific secure element; an
  attacker who rewrites slot 0 (e.g. via the pairing key) invalidates the record rather than
  silently substituting one.
- Must hold: the attestation key stays in ECC slot 0; the signed format is the only accepted
  format; load failures reinit to defaults rather than attempting recovery.
- Cost: each PIN-record save performs an on-chip ECDSA sign; a slot-0 key change wipes PIN state.
