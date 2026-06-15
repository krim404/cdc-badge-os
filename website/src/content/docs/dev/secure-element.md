---
title: Secure element (TROPIC01)
description: Developer reference for the TROPIC01 secure element - the ECC and R-Memory slot allocation, the ISecureElement operation set, supported curves, and the session model.
sidebar:
  order: 5
---

This is the **developer reference** for the TROPIC01 secure element (SE): the
authoritative slot allocation, the per-slot R-Memory size, the `ISecureElement`
operation set, and the supported curves. For the user-facing security model
(key non-exportability, on-chip generation, tamper-evidence), see
[Secure element & automatic key generation](/security/secure-element-keys/).

The badge stores private keys inside the TROPIC01, a chip separate from the
ESP32-S3 application processor. Two distinct storage areas are exposed: **ECC
key slots** for key pairs and **R-Memory slots** for metadata and small
secrets.

## Storage areas

| Area | Count | Slot index range | Reserved slot |
| --- | --- | --- | --- |
| ECC key slots | 32 | 0-31 | 0 |
| R-Memory slots | 512 | 0-511 | 0 |

The counts and bounds are fixed in two sources that must agree:

- `main/tropic_slot_map.h`: `ECC_SLOT_MIN = 0` / `ECC_SLOT_MAX = 31` (lines
  18-19), `RMEM_SLOT_MIN = 0` / `RMEM_SLOT_MAX = 511` (lines 22-23).
- `ISecureElement.h`: `ECC_SLOT_COUNT = 32` (line 61), `RMEM_SLOT_COUNT = 512`
  (line 62).

Slot 0 of each area is reserved: `ECC_SLOT_RESERVED = 0` (`tropic_slot_map.h`
line 20) and `RMEM_SLOT_RESERVED = 0` (line 24). The lowest allocatable
R-Memory slot is `RMEM_SLOT_MIN_ALLOC = 1` (line 25).

## R-Memory slot size

The per-slot R-Memory capacity depends on the TROPIC01 Application FW version,
so the firmware exposes both a stable minimum and a runtime query
(`ISecureElement.h`):

| Constant | Value | Meaning |
| --- | --- | --- |
| `RMEM_SLOT_SIZE` | 444 | Minimum guaranteed slot size across all supported FW versions (line 67) |
| `RMEM_SLOT_SIZE_MAX` | 475 | Stack-buffer ceiling sized for the largest known FW (line 69) |
| `getRmemSlotSize()` | runtime | Actual size reported by the running chip; always `>= RMEM_SLOT_SIZE` and `<= RMEM_SLOT_SIZE_MAX` (lines 244-249) |

Use **444 bytes** for static layouts that must remain stable across reflashes;
query `getRmemSlotSize()` for the real runtime capability. `RMEM_NAME_LEN = 16`
(line 70) is the fixed length of the R-Memory header name field.

:::caution[Doc-comment nuance]
The class-level summary comment writes "R-Memory storage (512 slots, 476 bytes
each)" (`ISecureElement.h` line 55) and `rmemWrite` documents "max 476 bytes"
(line 171). The authoritative size constants on the class itself are
`RMEM_SLOT_SIZE = 444` and `RMEM_SLOT_SIZE_MAX = 475`. Plan against the
constants, not the prose comment.
:::

## Slot allocation

The slot map is the compile-time, authoritative allocation across modules
(`main/tropic_slot_map.h`). Each module owns disjoint ECC and R-Memory ranges:

| Module | Module ID | ECC slots | R-Memory slots |
| --- | --- | --- | --- |
| System / attestation | 0 | 0 (reserved key slot) | 0 (reserved) |
| GPG | 2 | 1-3 | 1-3 |
| CA | 3 | 4 | 4 |
| FIDO2 | 4 | 5-30 | 5-31 |
| 2FA (TOTP) | 5 | - | 32-131 |
| Password vault | 6 | - | 132-500 |
| Plugins | 7 | 31 | 501-511 |

Source lines in `main/tropic_slot_map.h`:

- Module IDs: `MODULE_ID_MOD_SYSTEM 0` (line 30), `MODULE_ID_MOD_GPG 2` (line
  31), `MODULE_ID_MOD_CA 3` (line 32), `MODULE_ID_MOD_FIDO2 4` (line 33),
  `MODULE_ID_MOD_2FA 5` (line 34), `MODULE_ID_MOD_PASSWORD 6` (line 35),
  `MODULE_ID_PLUGIN_POOL 7` (line 36); `MODULE_ID_UNKNOWN 255` (line 37).
- ECC ranges: GPG 1-3 (lines 40-41), CA 4-4 (lines 42-43), FIDO2 5-30 (lines
  44-45), Plugins 31-31 (lines 47-48).
- R-Memory ranges: GPG 1-3 (lines 51-52), CA 4-4 (lines 53-54), FIDO2 5-31
  (lines 55-56), 2FA 32-131 (lines 57-58), Password 132-500 (lines 59-60),
  Plugins 501-511 (lines 64-65).

Notes from the source:

- 2FA (TOTP) and the password vault hold no ECC key pairs; they appear only in
  the R-Memory map (`TROPIC_RMEM_SLOT_MAP`, lines 74-80) and are absent from the
  ECC map (`TROPIC_ECC_SLOT_MAP`, lines 68-72).
- FIDO2 ECC ends at slot 30, so the single plugin ECC slot 31 (the last
  physical slot) does not overlap (`tropic_slot_map.h` lines 45-48).
- The plugin R-Memory range 501-511 is a pool whose named sub-slots are assigned
  dynamically at runtime, while the range bounds are validated centrally (lines
  61-65).
- Module IDs are permanent on-device identifiers and are never reassigned; new
  modules take new IDs (lines 27-29).

The reserved attestation key in ECC slot 0 and the PIN record in R-Memory slot 0
are detailed on the [secure-element security page](/security/secure-element-keys/)
and the [attestation page](/security/attestation/).

## Supported curves

`EccCurve` (`ISecureElement.h` lines 12-15) defines two curves:

| Curve | Enum | Module-level byte |
| --- | --- | --- |
| NIST P-256 (secp256r1) | `EccCurve::P256` | 1 |
| Ed25519 | `EccCurve::ED25519` | 0 |

The byte mapping is fixed by `curveByte()` (lines 22-24, returns `0` for
Ed25519, `1` otherwise) and `curveFromByte()` (lines 31-33, `0` maps to
Ed25519, otherwise P-256).

## Operation set

All operations are declared on `ISecureElement` (`cdc_hal`), a pure-virtual
interface. Results are reported via the `SeResult` enum (lines 38-47): `OK`,
`ERROR`, `SESSION_REQUIRED`, `SLOT_EMPTY`, `SLOT_OCCUPIED`, `INVALID_PARAM`,
`ALARM_MODE` (tamper detected), `NOT_SUPPORTED`.

### Session management

A secure session is required before operations (lines 74-94):

| Method | Signature | Purpose |
| --- | --- | --- |
| `sessionStart` | `bool sessionStart()` | Open a secure session (line 79) |
| `sessionEnd` | `void sessionEnd()` | Close the session (line 84) |
| `isSessionActive` | `bool isSessionActive() const` | Query session state (line 89) |
| `sleep` | `void sleep()` | Put the chip to sleep (line 94) |

When no session is available, operations return `SESSION_REQUIRED`.

### ECC key operations

| Method | Signature (lines) | Purpose |
| --- | --- | --- |
| `eccGenerate` | `SeResult eccGenerate(uint8_t slot, EccCurve curve)` (103) | Generate a key pair on-chip in the given slot and curve |
| `eccImport` | `SeResult eccImport(uint8_t slot, const uint8_t* privKey, EccCurve curve)` (111) | Import a 32-byte private key |
| `eccGetPublicKey` | `SeResult eccGetPublicKey(uint8_t slot, uint8_t* pubKey, EccCurve* curve = nullptr)` (119) | Read the public key (65 bytes P-256, 32 bytes Ed25519) |
| `eccDelete` | `SeResult eccDelete(uint8_t slot)` (124) | Delete the key in a slot |
| `eccSlotUsed` | `bool eccSlotUsed(uint8_t slot) const` (129) | Check whether a slot holds a key |

There is no private-key read or export call. The only key-material read-back is
`eccGetPublicKey`, which returns the public key.

### Signing

| Method | Signature (lines) | Notes |
| --- | --- | --- |
| `ecdsaSign` | `SeResult ecdsaSign(uint8_t slot, const uint8_t* msg, size_t msgLen, uint8_t* sig, size_t* sigLen)` (142-143) | P-256 ECDSA; the SE hashes the message internally with SHA-256, so callers MUST NOT pre-hash. Output is raw R\|\|S (64 bytes). |
| `eddsaSign` | `SeResult eddsaSign(uint8_t slot, const uint8_t* msg, size_t msgLen, uint8_t* sig)` (152-153) | Ed25519; output 64 bytes. |

### R-Memory

| Method | Signature (lines) | Purpose |
| --- | --- | --- |
| `rmemRead` | `SeResult rmemRead(uint16_t slot, uint8_t* data, uint16_t maxLen, uint16_t* actualLen)` (164-165) | Read a slot |
| `rmemWrite` | `SeResult rmemWrite(uint16_t slot, const uint8_t* data, uint16_t len)` (173) | Write a slot |
| `rmemErase` | `SeResult rmemErase(uint16_t slot)` (178) | Erase a slot |
| `rmemSlotUsed` | `bool rmemSlotUsed(uint16_t slot) const` (183) | Check whether a slot holds data |
| `rmemWriteWithHeader` | `SeResult rmemWriteWithHeader(uint16_t slot, uint8_t moduleId, const char* name, uint8_t flags, const uint8_t* payload, uint16_t payloadLen)` (199-201) | Write a slot with the common header plus payload |
| `rmemReadWithHeader` | `SeResult rmemReadWithHeader(uint16_t slot, RMemHeader* headerOut, uint8_t* payloadOut, uint16_t payloadMax, uint16_t* payloadLenOut)` (206-208) | Read header plus payload |

The packed `RMemHeader` (lines 187-194) carries `magic`, `checksum`, `moduleId`,
`flags`, a `char name[RMEM_NAME_LEN]` (16 bytes), and `payloadLen`.

### Random number generation

| Method | Signature (lines) | Behaviour |
| --- | --- | --- |
| `getRandom` | `bool getRandom(uint8_t* buffer, uint16_t size)` (219) | Hardware TRNG, with an ESP32 TRNG fallback when no SE session is available (a WARN is logged on fallback) |
| `getRandomStrict` | `bool getRandomStrict(uint8_t* buffer, uint16_t size)` (229) | Hardware TRNG only; returns false and leaves the buffer untouched if the SE TRNG is unreachable. Use for keys and seeds. |

### Diagnostics

| Method | Signature (lines) | Purpose |
| --- | --- | --- |
| `getChipId` | `bool getChipId(uint8_t* serialNum, uint8_t size)` (236) | Read the chip serial number |
| `getFwVersion` | `bool getFwVersion(uint8_t riscvVer[4], uint8_t spectVer[4])` (242) | Read RISC-V and SPECT FW versions (index 3 major, 2 minor, 1 patch, 0 build) |
| `getRmemSlotSize` | `uint16_t getRmemSlotSize() const` (249) | Runtime R-Memory slot size |

The instance is obtained via the factory `getSecureElementInstance()`
(`ISecureElement.h` line 253).

## Source files

| Concern | File |
| --- | --- |
| Slot allocation (authoritative) | `main/tropic_slot_map.h` |
| Operation interface, curves, sizes | `components/cdc_hal/include/cdc_hal/ISecureElement.h` |
| User-facing security model | [/security/secure-element-keys/](/security/secure-element-keys/) |
