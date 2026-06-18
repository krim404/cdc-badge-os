---
title: FIDO2 attestation key & AAGUID
description: What the badge attests, its AAGUID, the self-signed per-device attestation certificate, importing a CA-signed certificate, and where the attestation key lives.
sidebar:
  order: 2
---

When the badge registers a new FIDO2 credential it includes an **attestation
statement**: a signature over the new credential and the relying party's
challenge, plus a certificate, that lets a verifier reason about what kind of
authenticator produced the key. This page describes exactly what the badge
attests, based on the firmware source.

## AAGUID

The AAGUID (Authenticator Attestation GUID) identifies the authenticator model.
The badge reports a fixed 16-byte AAGUID, both in `authenticatorGetInfo` and in
the attested credential data of every registration:

```
CD CB AD 6E 39 C3 00 01 BA D6 E0 01 00 00 00 01
```

The same constant is used everywhere; there is no per-device randomisation of
the AAGUID.

## Attestation format: packed, basic, self-signed

The badge produces **packed attestation** with a certificate (`x5c`):

- `fmt` is `packed`.
- `attStmt` contains `alg` = ES256 and `sig`, plus an `x5c` array holding one
  certificate.
- The signature is ECDSA over the P-256 curve, computed over
  `authenticatorData || clientDataHash`.

The certificate in `x5c` is **self-signed by default**: its issuer equals its
subject, and it is signed by the very same attestation key whose public key it
carries. No CA chain or batch certificate is embedded in the firmware.

A **CA-signed certificate can be imported per device** (see below). When present
it replaces the self-signed certificate in `x5c`, so the attestation chains to
your own CA while the attestation key stays in the secure element.

:::caution[Self-signed and per-device by default]
Unless a CA-signed certificate is imported, each badge generates its own
attestation key on first use and the certificate is unique per device and
self-signed. This is **not** privacy-preserving batch attestation (where many
devices share one key and certificate to avoid being individually trackable). A
relying party that records the attestation certificate can distinguish one badge
from another, and a party that demands a known vendor CA root cannot
chain-validate a self-signed certificate.
:::

### Certificate contents

The self-signed certificate is built in firmware with these fixed fields:

| Field | Value |
| --- | --- |
| Subject / Issuer | `C=DE, O=CDC, OU=Authenticator Attestation, CN=CDC Badge FIDO2` |
| Public key | The attestation key (P-256, uncompressed point) |
| Signature algorithm | ecdsa-with-SHA256 |
| basicConstraints | critical, `CA:FALSE` |
| keyUsage | digitalSignature |
| Validity | 2024-01-01 to 2049-12-31 |
| Serial number | Random (8 bytes from the secure element TRNG) |

## Importing a CA-signed certificate

The attestation key stays in the secure element; to make attestation CA-backed
you sign the device's attestation public key with your own CA and import the
resulting certificate. The PIN-gated `ATTEST` serial command group drives this:

| Command | Action |
| --- | --- |
| `ATTEST EXPORT` | Print the attestation public key (P-256, uncompressed, hex) so your CA can issue a certificate for it. |
| `ATTEST IMPORT` | Paste the CA-signed certificate as hex (DER), ending with `---` on a new line (or `ABORT`). |
| `ATTEST CLEAR` | Remove the imported certificate and revert to the self-signed one. |

On import the badge parses the DER certificate and verifies that its public key
matches the attestation key in slot 0; a mismatch is rejected. The validated
certificate is stored in NVS (namespace `attest`) and used in `x5c` for every
later registration. If slot 0 is later regenerated the stored certificate no
longer matches and the badge falls back to the self-signed certificate.

## Where the attestation key lives

The attestation private key is a **chip-bound key in TROPIC01 ECC slot 0**. It
is generated inside the secure element and is never exported: signing is done by
asking the secure element to sign, so the private key never appears in firmware
memory.

Key lifecycle, as implemented by the attestation key service:

1. On boot the service checks ECC slot 0.
2. If the slot is empty, it generates a fresh **P-256** key on-chip
   (`eccGenerate`). This is what makes the key per-device.
3. It reads back the public key, hashes it, and stores that hash in NVS
   (namespace `attest`).
4. On later boots it re-reads the slot and compares the public-key hash against
   the stored value. A mismatch, an empty slot, or a wrong curve triggers
   regeneration.

Because the key is generated locally and lazily, a freshly flashed or wiped
badge produces a brand-new attestation identity. There is no factory-installed
key or vendor attestation root.

:::note[Reset / re-flash implications]
Wiping the secure element or regenerating slot 0 produces a new attestation key
and therefore a new attestation certificate. Existing credentials are
independent of the attestation key, but any prior record a relying party kept of
the old attestation certificate no longer matches.
:::

## Legacy U2F

For CTAP1 / U2F (`U2F_V2`) registrations the badge returns the **same** slot-0
attestation certificate and an ES256 P-256 attestation signature, so U2F
registrations carry the same self-signed, per-device attestation identity.

## What is verifiable here

| Property | Status |
| --- | --- |
| AAGUID | Fixed, value quoted above (from source) |
| Attestation format | `packed` with `x5c` (basic attestation) |
| Certificate trust | Self-signed per-device, or CA-signed when a certificate is imported |
| CA / batch chain | None by default; per-device CA certificate import supported |
| Key storage | TROPIC01 ECC slot 0, generated on-chip, non-exportable |
| Key curve | P-256 |
| Signature | ECDSA-SHA256 over authData \|\| clientDataHash |
