---
title: GPG key cross-signing
description: The badge-to-badge protocol for exchanging public keys and certifications over BLE, producing RFC 4880 certification signatures, collecting third-party certifications on the own key, and the serial commands and armored export format.
sidebar:
  order: 12
---

This page documents the cross-signing feature as implemented in
`components/mod_gpg/`: how two badges exchange OpenPGP public keys over Bluetooth,
how each badge certifies the other's key, how a certification travels back to the
key's owner, and how the own public key is exported with the collected
certifications for `gpg --import`.

The feature has these pieces:

1. A **transport** carrying compact records over the generic Bluetooth
   message-transfer framework (`cdc_msg`): a public-key record and a
   certification-return record, (de)serialised by `GpgKeyPayload.cpp` and
   `GpgCertPayload.cpp`.
2. A **received-key store** that persists incoming peer keys in NVS
   (`GpgRecvStore`).
3. A **signer / armorer** that builds the RFC 4880 certification signature and
   the ASCII-armored exports (`openpgp/xsig.cpp`).
4. A **self-cert store** that persists third-party certifications on the own key
   in NVS (`GpgSelfCertStore`), re-emitted on every own-key export.

## Transport

Transfers ride on the shared badge-to-badge message-transfer framework
(`cdc_msg`): the same beacon, peer picker, numeric-comparison pairing and
encrypted link used by the vCard exchange. GPG registers two MIME-typed handlers:

| MIME type | Direction | Payload |
| --- | --- | --- |
| `application/pgp-keys` | public key | a peer's public key record |
| `application/pgp-signature` | certification return | a certification over the recipient's own key |

The framework owns chunking, consent, encryption and progress; the GPG layer only
(de)serialises the records below.

### Public-key payload

The transferred record is the public key plus identity, not a full OpenPGP
packet stream:

| Field | Size | Notes |
| --- | --- | --- |
| curve | 1 | `0` = Ed25519, `1` = P-256 |
| pubkey_len | 1 | 32 for Ed25519, 64 for P-256 |
| pubkey | 32 or 64 | raw key bytes (P-256 is X\|\|Y, no `0x04` prefix) |
| created_at | 4 | key creation time (big-endian), reproduces the fingerprint |
| fingerprint_v4 | 20 | sender's OpenPGP v4 (SHA-1) fingerprint |
| user_id_len | 1 | 0 to 63 |
| user_id | 0 to 63 | UTF-8 user-id |

Maximum payload is 154 bytes (P-256, 63-byte user-id). On receipt the badge
recomputes the v4 fingerprint from `(curve, pubkey, created_at)` and rejects the
record unless it matches the transmitted `fingerprint_v4`, so a later
certification binds to the peer's real OpenPGP key. The receiver also computes a
v5 fingerprint locally and records the receive timestamp.

**Send Key** serialises `gpg_get_status()` plus the raw public key read from the
signature ECC slot; **Forward** serialises a stored received key.

### Certification-return payload

After cross-signing a received key, **Send Signature** transmits the
certification back to that key's owner over `application/pgp-signature`:

| Field | Size | Notes |
| --- | --- | --- |
| target_fp_v4 | 20 | v4 fingerprint of the certified key (the recipient's own key) |
| issuer_fp_v4 | 20 | signer's v4 fingerprint |
| issuer_uid_len | 1 | 0 to 63 |
| issuer_uid | 0 to 63 | signer's user-id (display only) |
| sig_pkt_len | 2 | signature-packet length (big-endian) |
| sig_pkt | variable | the verbatim OpenPGP Signature Packet body (Tag 2) |

The receiver rejects the record unless `target_fp_v4` equals its own signature
key's fingerprint, then stores it in the self-cert store.

## Received-key store

Each received key is persisted as a single NVS blob, keyed by the first 4 bytes
of its v4 fingerprint. The store is a singleton with a hard ceiling of **128**
keys. The stored record (`gpg_recv_key_t`) carries the curve, user-id, raw
public key, key creation time, both fingerprints, the receive timestamp, and -
once cross-signed - the 64-byte signature, its length, the signature creation
time and a `verified` flag.

Because NVS iteration order is unspecified, callers build a sorted (oldest-first)
index snapshot and address keys by position in that snapshot.

## Self-cert store

Third-party certifications received over `application/pgp-signature` are
persisted as single NVS blobs in `GpgSelfCertStore`, keyed by the issuer's v4
fingerprint, with a hard ceiling of **16** certifications. Each record holds the
issuer fingerprint and user-id, the receive timestamp, and the verbatim
Signature Packet body. The badge does not verify these signatures; they are
re-emitted unchanged when the own public key is exported so a GPG keyring builds
the web of trust.

## Certification signature

`gpgCrossSign()` builds an RFC 4880 certification over the *target* key and signs
it with **this** badge's signature key in the secure element. The signature
creation time passed in is stored alongside the signature and reused at export so
the signed bytes match the exported packet.

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
  `0x04`, the 4-byte key creation time, the public-key algorithm (`EdDSA` = 22 or
  `ECDSA` = 19) selected from the on-card curve, the curve OID, and the
  MPI-encoded point.
- The hashed subpacket area contains only the signature creation time
  (type `0x02`).
- The result is signed by the badge's signature ECC slot via TROPIC01
  (`eddsaSign` or `ecdsaSign`), producing 64 bytes of `R || S`.

`buildCertSigPacket()` assembles the Signature Packet body and is shared between
the armored export and the certification-return payload so both carry identical
bytes.

## Armored exports

All armored builders pack RFC 4880 packets into one ASCII-armored block, base64
encoded, wrapped at 64 characters, finished with a CRC-24 checksum line between
`BEGIN PGP PUBLIC KEY BLOCK` / `END PGP PUBLIC KEY BLOCK` delimiters, using
new-format headers with the 5-byte length form.

- `gpgBuildOwnSignedKeyArmored()` builds the **own** public key: Public-Key
  Packet (Tag 6) + User ID Packet (Tag 13) + one Signature Packet (Tag 2) per
  stored self-cert. **Export Public** (QR and serial) and `GPG EXPORT` use it, so
  importing the own key merges every collected third-party certification.
- `gpgBuildSignedKeyArmored()` builds a **received** key with this badge's
  certification (Tag 6 + Tag 13 + Tag 2). Requires the entry to be cross-signed
  (`sig_len == 64`). The **Export** action and `GPG EXPORT_SIGNED` use it.
- `gpgBuildPublicKeyArmored()` builds a received key with only the Public-Key and
  User ID packets (no signature). The **Show QR** action uses it.

## Menus

The GPG menu offers **Export Public**, **Send Key**, **Received Keys** and
**My Certifications**. A received key's detail view offers **Cross-Sign**,
**Export**, **Send Signature**, **Forward**, **Show QR** and **Delete**;
**Export** and **Send Signature** are disabled until the key is cross-signed.
**My Certifications** lists the third-party certifications on the own key with a
**Delete** action.

## Serial commands

The full workflow is driveable over serial:

| Command | Action |
| --- | --- |
| `GPG EXPORT` | Print the own OpenPGP public key (armored, with certifications) |
| `GPG RECV_LIST` | List received keys with short fingerprint and signed state |
| `GPG RECV_INFO <index>` | Show curve, full v4 / v5 fingerprints, receive time, signature |
| `GPG RECV_IMPORT <hex>` | Import a peer public-key wire payload (hex) into the received store |
| `GPG CROSS_SIGN <index>` | Produce and store the certification signature |
| `GPG EXPORT_SIGNED <index>` | Print the armored signed received key |
| `GPG SEND_SIG <index>` | Send a cross-signature back to the peer over BLE |
| `GPG CERT_LIST` | List third-party certifications on the own key |
| `GPG CERT_DELETE <index>` | Delete a stored certification on the own key |
| `GPG CERT_IMPORT <hex>` | Import a certification-return payload (hex) onto the own key |
| `GPG RECV_DELETE <index>` | Delete a received key |

`<index>` is the position in the sorted (oldest-first) snapshot, the same
ordering shown by `RECV_LIST` / `CERT_LIST`.
