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

Elliptic-curve keys are the default and preferred choice. RSA is available as a
software fallback that is slower and weaker (see below).

| Role | Curve(s) | Algorithm |
| --- | --- | --- |
| Signature | Ed25519 (default) or NIST P-256 | EdDSA / ECDSA / RSA |
| Authentication | Ed25519 (default) or NIST P-256 | EdDSA / ECDSA / RSA |
| Decryption | NIST P-256 (default) | ECDH / RSA |

The signature and authentication roles default to **Ed25519** and can be set to
**P-256**. The decryption role's ECC option is **P-256 ECDH**, because the secure
element has no native ECDH primitive and the firmware performs that one operation
in software with a separately protected key.

Any role can be switched to **RSA** (2048 / 3072 / 4096) from the host with
`gpg --card-edit` → `key-attr`. RSA keys are software keys: the secure element
cannot perform RSA, so the private key is held as an encrypted blob and used in
RAM. RSA is therefore slower and less protected than the secure-element ECC keys;
prefer Ed25519 or P-256 unless a peer specifically requires RSA.

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

- Prints an ASCII-armored OpenPGP public key (`BEGIN PGP PUBLIC KEY BLOCK`) to
  the serial console. It carries the signature primary key, the encryption
  subkey, and every certification collected under **My Certifications**.
- Shows the key as a **QR code** on the display, labelled with the user-id and a
  short word fingerprint for visual comparison.

The same export is available over serial with `GPG EXPORT`.

Import it on a computer with `gpg --import`, then run `gpg --card-status` to link
the imported key to the card. The imported key has both a signing `[S]` and an
encryption `[E]` subkey, so peers can encrypt to it and the badge decrypts. See
[GPG on your computer](/guide/gpg-getting-started/) for the full walkthrough.

## User PIN and Admin PIN

The OpenPGP card uses two PINs, matching standard smartcard semantics:

| PIN | OpenPGP name | Guards | Minimum length | Default |
| --- | --- | --- | --- | --- |
| User PIN | PW1 | Signing, decryption, SSH authentication | 6 | `123456` |
| Admin PIN | PW3 | Key generation, card data changes, PIN reset | 8 | `12345678` |

Change either PIN from **GPG ▸ Settings**, choosing **User PIN** or **Admin
PIN**. Each entry shows its remaining attempts in parentheses, for example
**User PIN (3)**, or **[Locked]** once the counter has reached zero. PINs can
also be changed from the host with `gpg --change-pin`.

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
computer. Open **GPG ▸ Send Key** to push your public key to a nearby badge over
the Bluetooth [message-transfer framework](/guide/bluetooth/) (the same beacon,
peer picker and numeric-comparison pairing as the vCard exchange). The receiving
badge stores it under **GPG ▸ Received Keys**, where each entry offers:

- **Cross-Sign** the key (optional: the badge produces an RFC 4880 certification
  signature with its own signature key, for the web of trust),
- **Export** the key as an ASCII-armored OpenPGP block to `gpg --import` on a
  computer; always available, carries the encryption subkey, and includes your
  certification once the key is cross-signed,
- **Send Signature** to push your certification back to the key's owner over
  Bluetooth (available once cross-signed),
- **Forward** the key on to another nearby badge,
- **Show QR** to display the public key as a QR code, and
- **Delete** the entry.

When a peer sends a certification back, it is collected under
**GPG ▸ My Certifications**. **GPG ▸ Export Public** (and `GPG EXPORT`) then emit
your OpenPGP public key with every collected certification attached, so a single
`gpg --import` rebuilds the web of trust.

The listing, cross-sign, export and delete actions are also available over
serial (`GPG RECV_LIST`, `GPG RECV_INFO`, `GPG RECV_CROSS_SIGN`, `GPG RECV_EXPORT`,
`GPG MYCERT_LIST`, `GPG RECV_DELETE`). Sending the cross-signature back to a peer
is a Bluetooth-only action (**GPG ▸ Received Keys ▸ Send Signature**).

For the exchange protocol and signature construction, see
[GPG key cross-signing](/dev/proto/gpg-cross-signing/).

## Serial commands

The `GPG` serial command groups the card operations:

| Command | Action |
| --- | --- |
| `GPG STATUS` | Show user-id, curve, creation time, signature count |
| `GPG GENERATE <curve> <user_id>` | Generate keys (`1` = Ed25519, `2` = P-256) |
| `GPG EXPORT` | Print the own public key (armored, with certifications) |
| `GPG RESET [token]` | Two-step destructive reset of all GPG keys |
| `GPG RECV_LIST` | List public keys received from peers (with their list index) |
| `GPG RECV_INFO <index>` | Show a received peer key's details |
| `GPG RECV_IMPORT <hex>` | Import a peer public-key wire payload (hex) |
| `GPG RECV_CROSS_SIGN <index>` | Cross-sign a received peer key with our signature key |
| `GPG RECV_EXPORT <index>` | Export a received peer key (armored, encryptable; adds our cross-signature if present) |
| `GPG MYCERT_LIST` | List third-party certifications collected on the own key |
| `GPG MYCERT_DELETE <index>` | Delete a stored certification on the own key |
| `GPG MYCERT_IMPORT <hex>` | Import a certification-return payload (hex) onto the own key |
| `GPG RECV_DELETE <index>` | Delete a received peer key |
| `GPG RSA_SELFTEST [bits]` | Run the software-RSA self-test (gen/sign/verify/decrypt), default 2048 |
