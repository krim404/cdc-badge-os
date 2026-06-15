---
title: OpenPGP smartcard (CCID)
description: The verifiable OpenPGP card surface of the badge - the USB CCID interface, supported APDU operations, PW1/PW3 semantics, data objects, fingerprints, key storage in the secure element, and what is not implemented.
sidebar:
  order: 13
---

This page documents the OpenPGP smart-card application the badge implements, as
found in `components/mod_gpg/`. It is descriptive of the firmware, not of the
OpenPGP specification: where a data object or command is absent or stubbed, it is
called out as a **GAP**.

The implementation targets the OpenPGP Smart Card Application **3.4.1** and is
adapted from pico-openpgp for the ESP32-S3 and the TROPIC01 secure element.

For the generated code reference, see the [Code reference](/api/).

## USB / CCID transport

The card is exposed as a USB CCID (Chip Card Interface Device) interface,
registered by the GPG module under the name `OpenPGP SmartCard`.

| Property | Value |
| --- | --- |
| USB class | CCID, `0x0B` |
| VID / PID | `0x08E6` / `0x4433` (Gemalto GemPC433, for libccid whitelist) |
| Endpoint size | 64 bytes (in and out) |
| CCID version (bcdCCID) | 1.10 |
| Protocol | T=1 only |
| dwMaxCCIDMessageLength | 2048 |
| Slots | 1 (index 0) |

The CCID layer handles `ICC_POWER_ON` (returns the ATR), `ICC_POWER_OFF`,
`GET_SLOT_STATUS`, `XFR_BLOCK` (carries an APDU to the OpenPGP applet),
`GET_PARAMETERS` and `RESET_PARAMETERS`. The ATR is a T=1 layout with 10
OpenPGP historical bytes.

## APDU dispatch

Each `XFR_BLOCK` payload is parsed as an ISO 7816 APDU and dispatched by INS. The
class byte allows only the base class and the chaining bit; secure messaging and
logical channels > 0 are rejected with `0x6E00`.

| INS | Command | Notes |
| --- | --- | --- |
| `0xA4` | SELECT | By AID; always allowed, even when terminated |
| `0xCA` | GET DATA | Data objects (see below) |
| `0xDA` | PUT DATA | Requires PW3 |
| `0xDB` | PUT DATA (odd) | Key import - DEC only |
| `0x20` | VERIFY | PW1 / PW3 |
| `0x24` | CHANGE REFERENCE DATA | PIN change |
| `0x2C` | RESET RETRY COUNTER | Unblock PW1 |
| `0x2A` | PSO | Sign (`9E 9A`) / Decipher (`80 86`) |
| `0x88` | INTERNAL AUTHENTICATE | AUT key (SSH) |
| `0x22` | MANAGE SECURITY ENVIRONMENT | Role check, no-op |
| `0x47` | GENERATE ASYMMETRIC KEY PAIR | Generate / read public key |
| `0x84` | GET CHALLENGE | Random bytes |
| `0xC0` | GET RESPONSE | Drain a chained response |
| `0xE6` | TERMINATE DF | Enter terminated state |
| `0x44` | ACTIVATE FILE | Factory reset when terminated |

Command chaining (CLA bit `0x10`) is accumulated into a single synthetic APDU;
long responses are returned via response chaining (`61xx` + GET RESPONSE).

## Keys and the secure element

The three OpenPGP key roles map to fixed storage. GPG uses TROPIC01 ECC slots
**1-3** and R-Memory slots **1-3** (see `main/tropic_slot_map.h`).

| Role | Key ref | Curve(s) | Storage |
| --- | --- | --- | --- |
| Signature | `0xB6` | Ed25519 (default) / P-256 | TROPIC01 ECC slot |
| Decryption | `0xB8` | P-256 (fixed) | Software key, encrypted in R-Memory |
| Authentication | `0xA4` | Ed25519 (default) / P-256 | TROPIC01 ECC slot |

RSA is not supported. Algorithm attributes advertise EdDSA (`0x16`, OID
1.3.6.1.4.1.11591.15.1), ECDSA (`0x13`, OID 1.2.840.10045.3.1.7) and ECDH
(`0x12`, P-256 OID). The signature and authentication curves default to Ed25519
and can be changed to P-256 via `PUT DATA C1` / `C3`; doing so wipes the existing
key in that slot so the next generation matches the new attributes.

The decryption role is fixed to **P-256 ECDH**. The TROPIC01 has no ECDH
primitive, so the DEC private key is generated in software, stored encrypted in
R-Memory (AES-256-GCM, wrapping key derived via HKDF over chip id and PIN hash,
with AAD binding the record to its slot), and decrypted into RAM only for the
duration of one ECDH computation, then zeroized.

### Key generation

`GENERATE ASYMMETRIC KEY PAIR` (INS `0x47`):

- `P1 = 0x80` generates a new key for the role in the CRT (`B6` / `B8` / `A4`)
  and requires PW3. SIG / AUT use the configured curve; DEC is generated in
  software as P-256.
- `P1 = 0x81` reads the existing public key. An empty slot returns `0x6A88`
  (referenced data not found), which `gpg --card-status` treats as "no key yet".

The public key is returned as `7F49 { 86 <pubkey> }`: P-256 as `0x04 || X || Y`
(65 bytes), Ed25519 as 32 raw bytes.

The on-device wizard uses a separate path (`gpg_generate_key`) that creates all
three keys at once and records their fingerprints; it does not go through this
APDU.

### Key import

`PUT DATA (odd, 0xDB)` accepts an OpenPGP Extended Header List for key import,
but **only the decryption key** (CRT `B8`) is supported - it is the one role the
secure element cannot hold natively. There is no import path for the signature or
authentication keys.

## Cryptographic operations

### PSO: COMPUTE DIGITAL SIGNATURE (`2A 9E 9A`)

Requires PW1. For P-256 the input must be a 32-byte SHA-256 digest, signed as
ECDSA; for Ed25519 the data is signed as EdDSA. The output is 64 bytes
(`R || S`). Each successful signature increments the persisted digital-signature
counter.

### PSO: DECIPHER (`2A 80 86`)

Requires PW1. The first data byte is the padding indicator:

- `0x02` selects symmetric **AES** decryption against the stored AES key
  (DO `0xD5`), AES-CFB128 over `IV || ciphertext`.
- Otherwise the data is parsed as a Cipher DO (`A6 / 7F49 / 86`) carrying the
  peer's 65-byte P-256 point, and the card returns the 32-byte ECDH shared
  secret.

### INTERNAL AUTHENTICATE (`0x88`)

Requires PW1 and `P1=P2=0x00`. Signs the supplied challenge with the
authentication key (ECDSA or EdDSA depending on the AUT curve). This is the
operation `gpg-agent` uses for SSH client authentication.

### MANAGE SECURITY ENVIRONMENT (`0x22`)

Accepted defensively: it validates `P1=0x41`, a `83 01 <ref>` template and that
the reference agrees with the role in P2, then returns success. Key selection is
fixed by role, so MSE is effectively a no-op.

## PW1 / PW3 semantics

| PIN | Reference(s) | Guards | Min length | Default |
| --- | --- | --- | --- | --- |
| PW1 (User) | `0x81` sign, `0x82` other | PSO, INTERNAL AUTHENTICATE | 6 | `123456` |
| PW3 (Admin) | `0x83` | PUT DATA, key generation, PIN reset | 8 | `12345678` |

The OpenPGP layer caps PIN length at 32 bytes; the badge PIN backend
(`PinManager`) enforces a 16-character maximum.

PIN behaviour follows smartcard semantics:

- Each PIN has **3 retries**. A failed attempt is pre-decremented and persisted
  before the comparison, so a power cycle cannot reset the counter.
- `VERIFY` with `Lc = 0` queries the remaining retries (`0x63Cx`); a blocked PIN
  returns `0x6983`; a wrong PIN returns `0x63Cx` with the remaining count.
- PINs are stored as iterated-salted SHA-256 (OpenPGP S2K) in the
  secure-element-backed PIN store. The iteration count is read from that store at
  verification time; the firmware default is 100000.

### Unblocking and reset

| Command | Effect |
| --- | --- |
| `CHANGE REFERENCE DATA` (`0x24`) | Change PW1 / PW3 given the old PIN |
| `RESET RETRY COUNTER` (`0x2C`), `P1=0x02` | Reset PW1, requires PW3 |
| `RESET RETRY COUNTER` (`0x2C`), `P1=0x00` | Reset PW1 using the Resetting Code |
| `TERMINATE DF` (`0xE6`) | Terminate; needs PW3, or both PINs blocked |
| `ACTIVATE FILE` (`0x44`) | When terminated, factory-resets the card |

The **Resetting Code** (DO `0xD3`) is optional. It is set or cleared with
`PUT DATA` (length 0 clears it) and stored as a salted SHA-256 hash computed with
OpenPGP S2K over 100000 total bytes (a hashed byte count, not an iteration count),
with its own 3-attempt counter; it lets the host reset PW1 without PW3.

While the card is in the terminated lifecycle state, every command except SELECT
and ACTIVATE FILE returns `0x6285`.

## Data objects

GET DATA returns the standard OpenPGP DOs. Selected highlights:

| Tag | Object | Notes |
| --- | --- | --- |
| `0x4F` | AID | `D2 76 00 01 24 01`, version 3.4, manufacturer `CD`, serial from MAC |
| `0x6E` | Application Related Data | Nested: ext caps, algo attrs, PW status, fingerprints, gen dates |
| `0x65` | Cardholder Related Data | Name, language, sex |
| `0xC0` | Extended Capabilities | SM none; cardholder cert max 2048; special DO max 256 |
| `0xC1`-`0xC3` | Algorithm Attributes | SIG / DEC / AUT |
| `0xC4` | PW Status Bytes | Max lengths and live retry counters |
| `0xC7`-`0xC9` | Fingerprints | SIG / DEC / AUT, v4 (SHA-1, 20 bytes) |
| `0xCA`-`0xCC` | CA Fingerprints | 3 slots |
| `0xCE`-`0xD0` | Generation times | 4-byte big-endian, per role |
| `0x93` | Digital signature counter | 3 bytes |
| `0x5F50` | URL | Public-key retrieval URL |

PUT DATA (requires PW3) covers cardholder name / language / URL / login,
the fingerprint and generation-time DOs, CA fingerprints, the algorithm
attributes (curve switch), the Resetting Code and the AES key.

Persistent card state (fingerprints, generation times, cardholder data, selected
curves, signature counter, PINs' Resetting-Code material) lives in a single NVS
blob that is signed with the slot-0 P-256 ECDSA attestation key; an invalid
signature causes the card to re-initialise to defaults rather than trust tampered
state.

## Gaps and unimplemented features

- **KDF Data Object (`0xF9`)**: GET DATA returns an empty object, signalling
  "no KDF configured", and there is no PUT DATA handler for it. A KDF-DO byte
  codec (`kdf.h`) exists in the tree but is not wired into the card processing,
  so host-managed PBKDF2 PIN pre-hashing is **not** supported. (The internal PIN
  hashing described above is unrelated to the host-facing KDF DO.) **GAP.**
- **Cardholder Certificate (`0x7F21`)**: GET DATA returns `0x6A88`; the
  certificate is not stored. **GAP.**
- **RSA**: not supported; algorithm-attribute validation rejects RSA. **GAP.**
- **Key import for SIG / AUT**: no path exists; only the DEC key can be
  imported. **GAP.**
- **DEC private key in RAM**: the decryption key is necessarily decrypted into
  RAM for each ECDH operation (the secure element cannot perform ECDH), unlike
  the SIG / AUT keys which never leave the element. It is re-encrypted at rest
  and zeroized after use. **CAVEAT.**
