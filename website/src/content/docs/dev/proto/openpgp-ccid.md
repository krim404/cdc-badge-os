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
| `0xDB` | PUT DATA (odd) | Key import (SIG / DEC / AUT, ECC or RSA) |
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
| `0xF1` | GET VERSION | Firmware version string (vendor command) |

`PSO:ENCIPHER` (`2A 86 80`) is not implemented; GnuPG does not use it.

Command chaining (CLA bit `0x10`) is accumulated into a single synthetic APDU;
long responses are returned via response chaining (`61xx` + GET RESPONSE).

## Keys and the secure element

The three OpenPGP key roles map to fixed storage. GPG uses TROPIC01 ECC slots
**1-3** and R-Memory slots **1-3** (see `main/tropic_slot_map.h`).

| Role | Key ref | Algorithm | Storage |
| --- | --- | --- | --- |
| Signature | `0xB6` | Ed25519 (default) / P-256 ECDSA / RSA | ECC: TROPIC01 ECC slot; RSA: encrypted R-Memory |
| Decryption | `0xB8` | P-256 ECDH (default) / RSA | Software key, encrypted in R-Memory |
| Authentication | `0xA4` | Ed25519 (default) / P-256 ECDSA / RSA | ECC: TROPIC01 ECC slot; RSA: encrypted R-Memory |

ECC is the default and preferred path. Algorithm attributes advertise EdDSA
(`0x16`, OID 1.3.6.1.4.1.11591.15.1), ECDSA (`0x13`, OID 1.2.840.10045.3.1.7) and
ECDH (`0x12`, P-256 OID). The signature and authentication curves default to
Ed25519 and can be changed to P-256 via `PUT DATA C1` / `C3`; doing so wipes the
existing key in that role.

The decryption role's ECC option is **P-256 ECDH**. The TROPIC01 has no ECDH
primitive, so the DEC private key is generated in software, stored encrypted in
R-Memory (AES-256-GCM, wrapping key derived via HKDF over chip id, with AAD
binding the record to its slot), and decrypted into RAM only for the duration of
one ECDH computation, then zeroized.

### RSA (software fallback)

Each role can be switched to RSA (2048 / 3072 / 4096) with `PUT DATA C1` / `C2` /
`C3`. RSA is a software fallback: it is slower and weaker than the secure-element
ECC path because the TROPIC01 cannot perform RSA, so the private key lives as an
AES-256-GCM-encrypted blob in the dedicated `mod_gpg_rsa` R-Memory pool (two slots
per role, since an RSA-4096 blob spans two) and is loaded into RAM for each
operation. Only the two primes and the public exponent are stored; mbedTLS
reconstructs the remaining CRT parameters on load. Switching a role to RSA wipes
its previous key.

### Key generation

`GENERATE ASYMMETRIC KEY PAIR` (INS `0x47`):

- `P1 = 0x80` generates a new key for the role in the CRT (`B6` / `B8` / `A4`)
  and requires PW3. SIG / AUT use the configured curve; DEC is generated in
  software as P-256. For an RSA role the key is generated in software via mbedTLS;
  on-card RSA generation is slow (seconds for 2048, up to minutes for 4096), so
  importing a host-generated key is the faster path.
- `P1 = 0x81` reads the existing public key. An empty slot returns `0x6A88`
  (referenced data not found), which `gpg --card-status` treats as "no key yet".

For ECC the public key is returned as `7F49 { 86 <pubkey> }`: P-256 as
`0x04 || X || Y` (65 bytes), Ed25519 as 32 raw bytes. For RSA it is returned as
`7F49 { 81 <modulus> 82 <exponent> }`.

The on-device wizard uses a separate path (`gpg_generate_key`) that creates all
three keys at once and records their fingerprints; it does not go through this
APDU.

### Key import

`PUT DATA (odd, 0xDB)` accepts an OpenPGP Extended Header List (`4D` → CRT →
`7F48` template → `5F48` values) for key import into any role. ECC keys: the
32-byte private scalar is injected into the TROPIC01 ECC slot (SIG / AUT) or the
software ECDH store (DEC). RSA keys: the modulus primes and public exponent
(`e` / `p` / `q`, located via the `7F48` template) are stored as an encrypted blob
in the RSA R-Memory pool.

## Cryptographic operations

### PSO: COMPUTE DIGITAL SIGNATURE (`2A 9E 9A`)

Requires PW1. For P-256 the input must be a 32-byte SHA-256 digest, signed as
ECDSA; for Ed25519 the data is signed as EdDSA. The output is 64 bytes
(`R || S`). For an RSA signature role the input is the host-supplied DigestInfo,
padded with EMSA-PKCS1-v1.5 and signed; the output is the modulus-sized block.
Each successful signature increments the persisted digital-signature counter.

### PSO: DECIPHER (`2A 80 86`)

Requires PW1. The first data byte is the padding indicator:

- `0x02` selects symmetric **AES** decryption against the stored AES key
  (DO `0xD5`), AES-CFB128 over `IV || ciphertext`.
- `0x00` (RSA decryption role) is followed by the cryptogram; the card performs
  RSAES-PKCS1-v1.5 decryption and returns the recovered plaintext.
- Otherwise the data is parsed as a Cipher DO (`A6 / 7F49 / 86`) carrying the
  peer's 65-byte P-256 point, and the card returns the 32-byte ECDH shared
  secret.

### INTERNAL AUTHENTICATE (`0x88`)

Requires PW1 and `P1=P2=0x00`. Signs the supplied challenge with the
authentication key (ECDSA / EdDSA depending on the AUT curve, or RSA
PKCS#1-v1.5 for an RSA AUT key). This is the operation `gpg-agent` uses for SSH
client authentication.

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

#### KDF Data Object (`0xF9`)

The KDF DO lets the host pre-hash PINs with PBKDF2 (SHA-256 or SHA-512) so the
cleartext PIN never crosses the USB / PC-SC link. `GET DATA 0xF9` returns the
stored configuration (or the disabled body `81 01 00`); `PUT DATA 0xF9` (PW3)
enables or disables it. When enabled, `VERIFY`, `CHANGE REFERENCE DATA` and the
admin `RESET RETRY COUNTER` carry the PBKDF2 pre-hash instead of the PIN, and the
PW1 / PW3 references are taken from the DO's initial-hash fields at setup. Enabling
or disabling KDF resets PW1 / PW3 (disabling restores the cleartext defaults).

Enabling is compatible with GnuPG's `kdf-setup`: a 32- or 64-byte PIN field is
treated as a pre-hash on `VERIFY`, `CHANGE REFERENCE DATA` and `RESET RETRY
COUNTER`, so the host can change PW1 / PW3 to their pre-hash form (cleartext old
PIN, pre-hash new value) before `PUT DATA 0xF9` turns the mode on.

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
| `0xF9` | KDF | PBKDF2 PIN pre-hash configuration (disabled by default) |
| `0x7F21` | Cardholder Certificate | Up to 2048 bytes, stored in NVS |

PUT DATA (requires PW3) covers cardholder name / language / URL / login,
the fingerprint and generation-time DOs, CA fingerprints, the algorithm
attributes (curve or RSA switch), the Resetting Code, the AES key, the KDF DO and
the cardholder certificate. `GET DATA 0x7F21` returns the stored certificate using
response chaining; an absent certificate returns `0x6A88`.

Persistent card state (fingerprints, generation times, cardholder data, selected
curves, signature counter, PINs' Resetting-Code material) lives in a single NVS
blob that is signed with the slot-0 P-256 ECDSA attestation key; an invalid
signature causes the card to re-initialise to defaults rather than trust tampered
state.

## Caveats

- **Software keys live in RAM during use.** The decryption key is decrypted into
  RAM for each ECDH operation (the secure element cannot perform ECDH), re-encrypted
  at rest and zeroized after use. The same applies to any role configured for RSA,
  whose private key is a software blob. ECDSA / EdDSA SIG and AUT keys never leave
  the secure element.
- **RSA is a software fallback.** It is slower and weaker than the secure-element
  ECC path and is offered only when a host explicitly selects it; ECC (Ed25519 /
  P-256) is the default and preferred choice.
- **Resetting Code is cleartext-only.** The optional Resetting Code unblock path
  uses the cleartext S2K hash and is not KDF-aware; under an active KDF DO, use the
  admin `RESET RETRY COUNTER` (`P1=0x02`) to unblock PW1.
- **`PSO:ENCIPHER` is not implemented**, matching GnuPG, which never issues it.
