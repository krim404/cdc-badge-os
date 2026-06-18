---
title: Secure element & automatic key generation
description: The TROPIC01 secure element, its ECC slot and R-Memory model, the slot allocation, what operations it performs, and which keys are generated on-chip.
sidebar:
  order: 3
---

The badge stores its private keys in a **TROPIC01 secure element (SE)**, a
separate chip from the ESP32-S3 application processor. Private keys live inside
the SE and the application firmware talks to it over a defined operation set: it
can ask the SE to generate a key, read back a *public* key, or sign a message,
but the operation interface exposes no way to read a private key out.

This page describes the storage model, the slot allocation, the operations the
SE performs, and which keys are created automatically, all from the firmware
source.

## Two kinds of storage: ECC slots and R-Memory

The SE provides two distinct storage areas:

| Area | Count | Slot 0 | Per-slot size | Holds |
| --- | --- | --- | --- | --- |
| **ECC key slots** | 32 (0-31) | reserved | one key pair | private keys (signing) |
| **R-Memory slots** | 512 (0-511) | reserved | >= 444 bytes | metadata / small secrets |

The counts are fixed in both the slot map and the hardware interface
(`ECC_SLOT_MIN`/`ECC_SLOT_MAX` and `RMEM_SLOT_MIN`/`RMEM_SLOT_MAX` in
`main/tropic_slot_map.h`; `ECC_SLOT_COUNT = 32`, `RMEM_SLOT_COUNT = 512` in
`ISecureElement.h`). Slot 0 of each area is reserved.

R-Memory slot capacity depends on the SE's running application firmware: the
firmware treats **444 bytes** as the minimum guaranteed per-slot size and
queries the chip at runtime for the actual size (always between 444 and 475
bytes). Use 444 bytes as the safe planning figure.

## Slot allocation

The slot map is the authoritative, compile-time allocation of ECC and R-Memory
slots across modules (`main/tropic_slot_map.h`):

| Module | ECC slots | R-Memory slots |
| --- | --- | --- |
| System / attestation | 0 (attestation key) | 0 (reserved) |
| GPG | 1-3 | 1-3 |
| CA | 4 | 4 |
| FIDO2 | 5-30 | 5-30 |
| TOTP (2FA) | - | 31-130 |
| GPG RSA keys | - | 131-136 |
| Password vault | - | 137-500 |
| Plugins | 31 | 501-511 |

TOTP and the password vault use only R-Memory (they store secrets, not SE key
pairs). FIDO2 ends at ECC slot 30 so the single plugin ECC slot (31) does not
overlap. The reserved attestation key in ECC slot 0 is covered on the dedicated
[attestation page](/security/attestation/).

## What the secure element does

The operation interface (`ISecureElement.h`) exposes:

- **Key generation** (`eccGenerate`): create a new key pair directly in an ECC
  slot, on either the **P-256 (secp256r1)** or **Ed25519** curve.
- **Public-key read-back** (`eccGetPublicKey`): retrieve the public key of a
  slot. There is no matching "read private key" call.
- **Key import** (`eccImport`) and **delete** (`eccDelete`).
- **Signing**: `ecdsaSign` (P-256 ECDSA; the SE hashes the message internally
  with SHA-256, so callers must not pre-hash) and `eddsaSign` (Ed25519).
- **R-Memory** read/write/erase, with a small header helper for module-tagged
  payloads.
- **Hardware TRNG**: `getRandom` (with an ESP32 RNG fallback when no SE session
  is available, logged as a warning) and `getRandomStrict` (returns bytes only
  when they came from the SE TRNG, for keys and seeds).

## How non-exportable the keys are

State this precisely, because it is the heart of the badge's security model:

- The SE **operation interface used by the firmware has no private-key read or
  export call**. The only way to retrieve key material is `eccGetPublicKey`,
  which returns the public key. Signing is performed by handing the message to
  the SE (`ecdsaSign` / `eddsaSign`); the private key never enters application
  memory.
- Keys can be **created on-chip** (`eccGenerate`), so for those keys no private
  value ever existed outside the SE.

:::caution[What this does and does not prove]
The firmware *cannot* extract a private key through the operation interface, and
on-chip generated keys never leave the SE. This is the protection the source
demonstrates.

It is **not** a claim about physical tamper resistance or laboratory key
extraction. The interface also exposes `eccImport`, so a key that was imported
(rather than generated on-chip) existed in firmware memory at import time. And
the SE reports an `ALARM_MODE` result, indicating it has a tamper/alarm state,
but the strength of that protection against a determined physical attacker is a
property of the TROPIC01 hardware, not something this firmware can guarantee.
:::

## Automatic, on-chip key generation

Several keys are generated **inside the SE**, automatically, rather than being
imported from outside:

- **Attestation / device identity key (ECC slot 0).** Generated on-chip on
  first boot if the slot is empty, on the P-256 curve. The firmware reads back
  only the public key, hashes it, and stores that hash to detect later
  tampering or regeneration. The private key is never exported. Full lifecycle
  on the [attestation page](/security/attestation/).
- **FIDO2 credential keys.** Created on-chip via `eccGenerate` when a new
  credential is registered (`mod_fido2/src/fido2_storage.cpp`).
- **GPG / OpenPGP keys.** The signature and authentication ECC keys are
  generated on-chip via `eccGenerate` in the GPG module and OpenPGP card path
  (`mod_gpg/src/gpg.cpp`, `mod_gpg/src/openpgp/openpgp.cpp`). The decryption key
  (P-256 ECDH) and any role configured for RSA are **software keys**: the secure
  element cannot perform ECDH or RSA, so those private keys are held as
  AES-256-GCM-encrypted blobs in R-Memory and decrypted into RAM only for the
  operation. ECC is the default; RSA is an opt-in fallback.

Because these keys are generated locally, a freshly flashed or wiped badge
produces brand-new key material. There is no factory-installed key shared
across devices.

## Tamper-evident PIN storage

The PIN record (R-Memory slot 0) is signed with the chip-bound attestation key
in ECC slot 0. If slot 0 is regenerated or the stored payload is altered, the
signature no longer verifies and the firmware re-initialises the PIN store to
defaults instead of trusting the tampered record
(`PinManager::loadFromStorage`). This binds the PIN data to the specific SE.

The record holds two different PIN-hash forms, which is **protocol-driven, not
an inconsistency**: the badge PIN (which doubles as the FIDO2 CTAP2 ClientPIN)
is stored as `LEFT(SHA-256(PIN), 16)`, while the OpenPGP PW1/PW3 PINs use
Iterated+Salted S2K because the OpenPGP card KDF-DO requires it. The duress PIN
also uses S2K. See [ADR-0004](/dev/adr/0004-two-kdf-pin-hashing/) and the
[PINs & lockout](/security/pin-lockout/) page.

## What is verifiable here

| Property | Status |
| --- | --- |
| ECC slots | 32, slot 0 reserved |
| R-Memory slots | 512, slot 0 reserved, >= 444 bytes each |
| Curves | P-256 (secp256r1), Ed25519 |
| Signing | ECDSA-SHA256 (P-256), EdDSA (Ed25519) |
| Private-key export call | None in the operation interface |
| On-chip generation | Attestation, FIDO2, GPG keys |
| Key import path exists | Yes (`eccImport`) |
| Hardware TRNG | Yes (`getRandom`, `getRandomStrict`) |
| Tamper/alarm state | Reported by SE (`ALARM_MODE`) |
