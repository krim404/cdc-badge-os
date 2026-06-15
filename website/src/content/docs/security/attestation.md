---
title: FIDO2 attestation key & AAGUID
description: What the badge attests, its AAGUID, the self-signed per-device attestation certificate, and where the attestation key lives.
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

The certificate in `x5c` is **self-signed**: its issuer equals its subject, and
it is signed by the very same attestation key whose public key it carries. There
is **no CA chain and no batch certificate** embedded in the firmware.

:::caution[Not CA-backed, and per-device]
Because each badge generates its own attestation key on first use, the
attestation certificate is unique per device and self-signed. This is **not**
privacy-preserving batch attestation (where many devices share one key and
certificate to avoid being individually trackable). A relying party that records
the attestation certificate can distinguish one badge from another. Relying
parties that demand a known vendor CA root will not be able to chain-validate
this certificate.
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
| Certificate trust | Self-signed, per-device |
| CA / batch chain | None |
| Key storage | TROPIC01 ECC slot 0, generated on-chip, non-exportable |
| Key curve | P-256 |
| Signature | ECDSA-SHA256 over authData \|\| clientDataHash |
