---
title: GPG key cross-signing
description: The badge-to-badge protocol for exchanging public keys over BLE and producing RFC 4880 certification signatures, with the serial RECV_/CROSS_SIGN commands and the armored export format.
sidebar:
  order: 12
---

This page documents the cross-signing feature as implemented in
`components/mod_gpg/`: how one badge transfers its OpenPGP public key to another
over Bluetooth, how the receiving badge produces a certification signature over
that key, and how the signed key is exported for `gpg --import`.

The feature has three independent pieces:

1. A **BLE transport** that pushes a compact key record from one badge to
   another (`ble_gpg_xsig.cpp`).
2. A **receive store** that persists incoming keys in NVS (`GpgRecvStore`).
3. A **signer / armorer** that builds the RFC 4880 certification signature and
   the ASCII-armored export (`openpgp/xsig.cpp`).

## BLE transport

The receiver registers a dedicated GATT service. UUIDs are 128-bit:

| Item | UUID | Properties |
| --- | --- | --- |
| Service | `8E2F1F30-8B5D-4D7A-9A6E-4C9D6A8B1A01` | - |
| RX characteristic | `…1F31…` | Write (encryption required) |
| STATUS characteristic | `…1F32…` | Read + Notify (encryption required) |

Both characteristics require an encrypted link. The transport mirrors the vCard
exchange: the sender writes the payload to RX in chunks; the receiver reports the
result through a STATUS notification.

### Chunk framing

Each write to RX is a one-byte opcode followed by data:

| Opcode | Meaning | Layout |
| --- | --- | --- |
| `0x11` START | First chunk | opcode + 2-byte total length (big-endian) + first chunk |
| `0x12` CONT | Middle chunk | opcode + chunk |
| `0x13` END | Last chunk | opcode + chunk |

The receiver buffers chunks until it has the declared total length (or sees the
END opcode), then parses and stores the record. The STATUS notification echoes
the END opcode group: a 3-byte frame `0x93 0x01 <code>`:

| Status code | Meaning |
| --- | --- |
| `0x00` | OK (stored) |
| `0x01` | Bad / malformed payload |
| `0x02` | Store full |
| `0x03` | Internal error |

The sender's reply opcodes `0x91` / `0x92` / `0x93` mirror the write opcodes.

### Payload layout

The transferred record is the public key plus identity, not a full OpenPGP
packet stream:

| Field | Size | Notes |
| --- | --- | --- |
| curve | 1 | `0` = Ed25519, `1` = P-256 |
| pubkey_len | 1 | 32 for Ed25519, 64 for P-256 |
| pubkey | 32 or 64 | raw key bytes (P-256 is X\|\|Y, no `0x04` prefix) |
| fingerprint_v4 | 20 | sender's OpenPGP v4 (SHA-1) fingerprint |
| user_id_len | 1 | 0 to 63 |
| user_id | 0 to 63 | UTF-8 user-id |

Minimum payload is 56 bytes (Ed25519, empty user-id); maximum is 150 bytes
(P-256, 63-byte user-id). The receiver validates curve, key length and the
declared lengths before accepting the record.

The receiver computes a v5 fingerprint locally from the received key material
and records the receive timestamp; these are not part of the wire payload.

### Sender state

The send side connects to the peer address, discovers the cross-sign service,
resolves the RX characteristic handle and writes the chunks. The badge's own key
record is built from `gpg_get_status()` plus the raw public key read from the
signature ECC slot.

:::caution[Not wired up end to end]
The on-device **Send Key** menu action is a placeholder toast; the sender
function `ble_gpg_xsig_send()` exists but is not yet driven from the UI. The
**receive**, **cross-sign** and **export** paths are complete and are exercised
by the serial commands below.
:::

## Receive store

Each received key is persisted as a single NVS blob, keyed by the first 4 bytes
of its v4 fingerprint. The store is a singleton with a hard ceiling of **128**
keys. The stored record (`gpg_recv_key_t`) carries the curve, user-id, raw
public key, both fingerprints, the receive timestamp, and - once cross-signed -
the 64-byte signature, its length and a `verified` flag.

Because NVS iteration order is unspecified, callers build a sorted (oldest-first)
index snapshot and address keys by position in that snapshot.

## Certification signature

`gpgCrossSign()` builds an RFC 4880 certification over the *target* key and signs
it with **this** badge's signature key in the secure element.

The hashed input is the standard OpenPGP certification preimage:

```
SHA-256 over:
  0x99 || pk_body_len(2) || Public-Key-Packet-body
  0xB4 || uid_len(4)     || user-id bytes
  sig_data_header        (0x04, sig type, pubkey algo, hash algo,
                          hashed-subpacket length)
  hashed_subpackets      (signature creation time)
  trailer                (0x04 0xFF + 4-byte hashed-data length)
```

Details:

- Signature type is **0x10** (generic certification of a user-id).
- Hash algorithm is **SHA-256** (`0x08`).
- The public-key-packet body is reconstructed from the stored key: version
  `0x04`, the 4-byte creation time (the receive timestamp), the public-key
  algorithm (`EdDSA` = 22 or `ECDSA` = 19), the curve OID, and the MPI-encoded
  point.
- The hashed subpacket area contains only the signature creation time
  (type `0x02`).
- The result is signed by the badge's signature ECC slot via TROPIC01
  (`eddsaSign` or `ecdsaSign`), producing 64 bytes of `R || S`.

:::caution[Signature algorithm caveat]
The signing algorithm is selected from `gpg_get_status().curve`, which always
reports **Ed25519** (the real on-card curve is not exposed by that snapshot).
A badge whose own signature key is actually P-256 would therefore label the
certification as EdDSA. Verify on hardware before depending on P-256 cross-signs.
The algorithm is chosen in `components/mod_gpg/src/openpgp/xsig.cpp` from
`gpg_get_status().curve`.
:::

## Armored export

`gpgBuildSignedKeyArmored()` packs three RFC 4880 packets into one ASCII-armored
block suitable for `gpg --import`:

1. **Public-Key Packet** (Tag 6) for the target key.
2. **User ID Packet** (Tag 13).
3. **Signature Packet** (Tag 2) carrying the certification: version 4, sig type
   0x10, the hashed subpackets (creation time), the unhashed subpackets (Issuer
   Key ID = last 8 bytes of the signer's v4 fingerprint), the left-16-bits hash
   check, and the R/S MPIs.

All packets use new-format headers with the 5-byte length form. The block is
base64-encoded, wrapped at 64 characters, and finished with a CRC-24 checksum
line, between `BEGIN PGP PUBLIC KEY BLOCK` / `END PGP PUBLIC KEY BLOCK`
delimiters.

Export requires the entry to already be cross-signed (`sig_len == 64`).

## Serial commands

The cross-sign workflow is fully driveable over serial:

| Command | Action |
| --- | --- |
| `GPG RECV_LIST` | List received keys with short fingerprint and signed state |
| `GPG RECV_INFO <index>` | Show curve, full v4 / v5 fingerprints, receive time, signature |
| `GPG CROSS_SIGN <index>` | Produce and store the certification signature |
| `GPG EXPORT_SIGNED <index>` | Print the armored signed key |
| `GPG RECV_DELETE <index>` | Delete a received key |

`<index>` is the position in the sorted (oldest-first) snapshot, the same
ordering shown by `RECV_LIST`.
