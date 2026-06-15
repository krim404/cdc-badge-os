---
title: GPG / OpenPGP & SSH
description: Use the badge as an OpenPGP smartcard over USB for signing, decryption and SSH authentication, generate keys on-device, and cross-sign other badges' keys.
sidebar:
  order: 9
---

The badge presents itself to a host computer as an **OpenPGP smartcard** over
USB. GnuPG and `gpg-agent` talk to it the same way they talk to a YubiKey or an
OpenPGP card: signing, decryption and authentication keys never leave the
device. The authentication key doubles as an SSH key.

The GPG module is reached from the badge main menu under **GPG**.

## What the badge is

When plugged in, the GPG module registers a USB CCID (chip-card) interface named
`OpenPGP SmartCard`. The card application follows the OpenPGP Smart Card
Application 3.4.1 specification. On the host, `gpg --card-status` enumerates it
like any other OpenPGP card.

Private keys are held in the TROPIC01 secure element (signing and authentication
keys) or in encrypted secure-element R-Memory (the decryption key). They cannot
be read out over the interface.

For the wire-level protocol, see
[OpenPGP smartcard (CCID)](/dev/proto/openpgp-ccid/).

## Key roles

An OpenPGP card carries three keys, each with a fixed job:

| Role | Purpose | Where the private key lives |
| --- | --- | --- |
| Signature (SIG) | Sign data / certify keys | TROPIC01 ECC slot |
| Decryption (DEC) | Decrypt messages (ECDH) | Encrypted in R-Memory |
| Authentication (AUT) | Client authentication, including **SSH** | TROPIC01 ECC slot |

The authentication key is exposed for SSH: `gpg-agent` (with SSH support
enabled) uses the badge's AUT key to answer SSH authentication challenges. The
host signs each challenge through the card's INTERNAL AUTHENTICATE operation,
which requires the User PIN.

## Curves and algorithms

The badge implements elliptic-curve keys only. RSA is not supported.

| Role | Curve(s) | Algorithm |
| --- | --- | --- |
| Signature | Ed25519 (default) or NIST P-256 | EdDSA / ECDSA |
| Authentication | Ed25519 (default) or NIST P-256 | EdDSA / ECDSA |
| Decryption | NIST P-256 (fixed) | ECDH |

The signature and authentication roles default to **Ed25519** and can be set to
**P-256**. The decryption role is always **P-256 ECDH**, because the secure
element has no native ECDH primitive and the firmware performs that one operation
in software with a separately protected key.

Public-key fingerprints follow the OpenPGP v4 format (SHA-1, 20 bytes).

## Generating keys on the device

The fastest path is the on-device wizard. From **GPG**, choose
**Generate Keys**. The wizard asks for:

1. A **Name**.
2. An optional **Email** (combined into a `Name <email>` user-id).
3. The **Curve** (<kbd>Ed25519</kbd> or <kbd>P-256</kbd>).

One run generates all three keys (SIG, DEC, AUT) and records their
fingerprints, generation time and cardholder name on the card. The signature and
authentication keys are created with the chosen curve; the decryption key is
always P-256.

:::note
Keys can also be generated from the host with `gpg --card-edit` then `generate`.
That path goes through the card interface and requires the Admin PIN. The
on-device wizard does not.
:::

## Exporting the public key

From **GPG**, choose **Export Public** once keys exist. The badge:

- Prints the signature public key as a `BEGIN PUBLIC KEY` PEM block (DER
  SubjectPublicKeyInfo) to the serial console.
- Shows the key as a **QR code** on the display, labelled with the user-id and a
  short word fingerprint for visual comparison.

The same export is available over serial with `GPG EXPORT`.

To import a full OpenPGP key into GnuPG, use `gpg --card-status` /
`gpg --card-edit`, which reads the keys directly from the card.

## User PIN and Admin PIN

The OpenPGP card uses two PINs, matching standard smartcard semantics:

| PIN | OpenPGP name | Guards | Minimum length | Default |
| --- | --- | --- | --- | --- |
| User PIN | PW1 | Signing, decryption, SSH authentication | 6 | `123456` |
| Admin PIN | PW3 | Key generation, card data changes, PIN reset | 8 | `12345678` |

Change either PIN from **GPG ▸ Settings**, choosing **User PIN** or **Admin
PIN**. PINs can also be changed from the host with `gpg --change-pin`.

:::caution
Change the default PINs before relying on the card. The defaults are public.
:::

### Lockout behaviour

Each PIN allows **3 attempts**. A failed attempt is counted and persisted
*before* the check runs, so a power cycle cannot reset the counter. When the
counter reaches zero the PIN is **blocked**, and the card returns the
"authentication method blocked" status to the host. There is no timed recovery
for the OpenPGP PINs.

Recovery from a blocked PIN:

- A blocked **User PIN** can be reset by the host using the **Admin PIN**
  (`gpg --admin` then `unblock`), or with a configured **Resetting Code**.
- If both PINs are blocked, the card can be terminated and re-activated from the
  host, which factory-resets it (`TERMINATE` then `ACTIVATE FILE`).

You can also wipe all GPG keys and PINs from the device with **GPG ▸ Reset**, or
over serial with the two-step `GPG RESET` command.

## Badge-to-badge cross-signing

Two badges can exchange and cross-sign each other's keys directly, without a
computer. One badge sends its public key to another over Bluetooth; the
receiving badge stores it under **GPG ▸ Received Keys**, where you can:

- **Cross-Sign** the key (the badge produces an RFC 4880 certification signature
  with its own signature key), and
- **Export** the resulting signed key as an ASCII-armored OpenPGP block that you
  later `gpg --import` on a computer.

:::caution
The "Send Key" menu entry that initiates the Bluetooth push is a work in
progress and currently shows a placeholder. Receiving, cross-signing and
exporting a received key are implemented; the same actions are available over
serial (`GPG RECV_LIST`, `GPG CROSS_SIGN`, `GPG EXPORT_SIGNED`).
:::

For the exchange protocol and signature construction, see
[GPG key cross-signing](/dev/proto/gpg-cross-signing/).

## Serial commands

The `GPG` serial command groups the card operations:

| Command | Action |
| --- | --- |
| `GPG STATUS` | Show user-id, curve, creation time, signature count |
| `GPG GENERATE <curve> <user_id>` | Generate keys (`1` = Ed25519, `2` = P-256) |
| `GPG EXPORT` | Print the public key as PEM |
| `GPG RESET [token]` | Two-step destructive reset of all GPG keys |
| `GPG RECV_LIST` | List received cross-sign keys |
| `GPG RECV_INFO <index>` | Show a received key's details |
| `GPG CROSS_SIGN <index>` | Cross-sign a received key |
| `GPG EXPORT_SIGNED <index>` | Export a signed key as an armored block |
| `GPG RECV_DELETE <index>` | Delete a received key |
