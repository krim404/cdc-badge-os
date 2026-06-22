---
title: GPG on your computer
description: Step by step from a fresh badge to signing and encrypting files on your computer with GnuPG, plus the badge-to-badge key exchange and cross-signing flow.
sidebar:
  order: 9
---

This walkthrough takes you from a fresh badge to a working OpenPGP key on your
computer: generate the keys on the badge, get the public key into GnuPG, then
sign, encrypt and decrypt real files. The private keys never leave the badge.

For the feature reference (key roles, curves, PIN semantics, every serial
command) see [GPG / OpenPGP & SSH](/guide/gpg-ssh/).

## What you need

- A badge with GPG keys (this page generates them in step 1).
- **GnuPG** on your computer:
  - **macOS:** `brew install gnupg pinentry-mac`
  - **Linux (Debian/Ubuntu):** `sudo apt install gnupg scdaemon`
  - **Windows:** install **Gpg4win** (`winget install GnuPG.Gpg4win`).
- A serial terminal at **115200 baud** to read the badge console (any terminal
  works; the firmware repo ships `tools/` helpers).

Check GnuPG is installed:

```sh
$ gpg --version
```

## 1. Generate keys on the badge

From the badge main menu open **GPG ▸ Generate Keys** and follow the wizard:

1. Enter a **Name**.
2. Enter an optional **Email** (combined into a `Name <email>` user-id).
3. Choose the **Curve**: <kbd>Ed25519</kbd> (recommended) or <kbd>P-256</kbd>.

One run creates all three keys: a **signature** key, a **decryption** key and an
**authentication** key. The signature and authentication keys are the chosen
curve; the decryption key is always P-256.

:::caution
Change the default PINs before relying on the card (**GPG ▸ Settings**). The
defaults are public: User PIN (PW1) `123456`, Admin PIN (PW3) `12345678`.
:::

## 2. Plug in and find the card

Plug the badge into USB. The badge presents an OpenPGP smartcard, so GnuPG sees
it like a YubiKey:

```sh
$ gpg --card-status
Reader ...........: CDC BadgeV1
Application type .: OpenPGP
Serial number ....: BA755010
Signature key ....: A88F A2BB B450 CB1E 3A20  A170 ABC0 15BA 827C 44E0
Encryption key....: E487 CB0B B44C 6708 857A  2CCF 38F1 4AB4 9DCE F583
Authentication key: 080F E598 AB94 1D27 9B80  478B 0F93 0164 13AA 70D2
```

The `Reader` line names the badge and `Application type` is `OpenPGP`. If the
reader is not found, restart the smartcard daemon with `gpgconf --kill scdaemon`
and try again. Only one program can hold the card at a time.

## 3. Import your key and link it to the badge

The badge generated the keys itself, so GnuPG does not have the public key yet:
`gpg --card-status` only shows the fingerprints. Export the public key over the
serial console and import it.

:::note[Opening the serial console]
- **macOS:** find the port with `ls /dev/cu.usbmodem*`, then `screen /dev/cu.usbmodem... 115200`.
- **Linux:** the port is usually `/dev/ttyACM0`; run `screen /dev/ttyACM0 115200` (add yourself to the `dialout` group if access is denied).
- **Windows:** open **Device Manager ▸ Ports (COM & LPT)** to find the badge's `COMx`, then connect with [PuTTY](https://www.putty.org/) (connection type **Serial**, speed **115200**) or another terminal.
:::

In your serial terminal (115200 baud), type:

```text
GPG EXPORT
```

The badge prints an ASCII-armored OpenPGP block:

```text
-----BEGIN PGP PUBLIC KEY BLOCK-----
...
-----END PGP PUBLIC KEY BLOCK-----
```

Copy the whole block (including the `BEGIN`/`END` lines and the blank line after
the header) into a file `mykey.asc`, then import and link it to the card:

```sh
$ gpg --import mykey.asc
$ gpg --card-status
```

Confirm the key is there with both a signing primary key `[SCA]` and an
encryption `[E]` subkey:

```sh
$ gpg --list-keys
pub   ed25519 ... [SCA]
      A88FA2BBB450CB1E3A20A170ABC015BA827C44E0
uid           Your Name <you@example.com>
sub   nistp256 ... [E]
```

That `[E]` subkey is what lets other people encrypt to you and what the badge
decrypts in step 6.

The matching **private keys never leave the badge**. The `gpg --card-status` you
just ran also recorded them as on-card stubs, which you can see with:

```sh
$ gpg --list-secret-keys
sec>  ed25519 ... [SCA]
      A88FA2BBB450CB1E3A20A170ABC015BA827C44E0
      Card serial no. = 4344 BA755010
uid           Your Name <you@example.com>
ssb>  nistp256 ... [E]
      E487CB0BB44C6708857A2CCF38F14AB49DCEF583
      Card serial no. = 4344 BA755010
```

The `>` after `sec` / `ssb` and the `Card serial no.` line mean GnuPG holds only
a reference: the actual secret keys stay on the badge. Every signature (step 5)
and decryption (step 7) runs on the badge and asks for the **User PIN (PW1)**.

:::note
`gpg --card-status` is what links the imported key to the badge; no
`gpg --edit-card` ▸ `fetch` step is needed. (`fetch` reads a public-key URL
stored on the card, which the badge leaves empty.)
:::

## 4. Share and import public keys

Your own `mykey.asc` is safe to hand out: give it to anyone who wants to send you
encrypted files or verify your signatures, or publish it wherever you like. It
contains only public material.

To **encrypt a file to someone** (step 6), you first need *their* public key in
GnuPG. There are two ways to get it:

- **From a file** they gave you:

  ```sh
  $ gpg --import their-key.asc
  ```

- **From your badge**, if they sent you their key over Bluetooth: it now lives
  under **GPG ▸ Received Keys**. Pull it onto the computer over the serial console
  as shown in
  [Getting an exchanged key onto a computer](#getting-an-exchanged-key-onto-a-computer)
  below, then `gpg --import` the saved block.

Either way, `gpg --list-keys` then lists their key with an `[E]` subkey, ready to
encrypt to.

## 5. Sign a file with the badge

Signing runs on the badge: GnuPG hands the data to the card, the badge signs it
with the on-card signature key, and asks for the **User PIN (PW1)**. The private
key never leaves the device.

```sh
$ gpg --detach-sign -u <YOUR_FINGERPRINT> report.pdf
$ gpg --verify report.pdf.sig report.pdf
```

Use `--clearsign` for text you want to keep readable.

## 6. Encrypt a file for someone else

Encrypting *to a recipient* uses **their** public key, not the badge, so this
step needs no PIN:

```sh
$ gpg --encrypt --recipient friend@example.com document.txt
```

To **sign with the badge and encrypt** in one step, combine both. The signature
runs on the badge and asks for the **User PIN (PW1)**; the encryption uses the
recipient's public key:

```sh
$ gpg --sign --encrypt -r friend@example.com -u <YOUR_FINGERPRINT> document.txt
```

## 7. Decrypt a file sent to you

When someone encrypts a file to your key, the badge does the decryption with its
decryption key, so it asks for the **User PIN (PW1)**:

```sh
$ gpg --decrypt secret.gpg
```

GnuPG talks to the badge over the smartcard interface; the private key never
leaves the device.

## Badge-to-badge key exchange and cross-signing

Two badges can exchange and cross-sign each other's keys over Bluetooth, without
a computer. The full round trip:

1. **Send your key.** On the first badge open **GPG ▸ Send Key**, pick the other
   badge, and confirm the matching number on both. The other badge stores it
   under **GPG ▸ Received Keys**.
2. **Cross-sign.** On the receiving badge open **GPG ▸ Received Keys**, select
   the entry and choose **Cross-Sign**. The badge certifies that key with its
   own signature key.
3. **Send the signature back.** Choose **Send Signature** to return the
   certification to the key's owner over Bluetooth.
4. **Collect it.** On the original badge the returned certification appears under
   **GPG ▸ My Certifications**.

A certification only lands under **My Certifications** if it actually targets
that badge's own key; one sent to the wrong badge is discarded.

### Getting an exchanged key onto a computer

A key you received over Bluetooth lives in **Received Keys**. To use it in GnuPG,
read it over the serial console (115200 baud) by its list index:

```text
GPG RECV_LIST
OK: 1 received keys
[0] Alice Example <alice@example.com>
    FP: F832D01E...
    Signed: No

GPG RECV_EXPORT 0
-----BEGIN PGP PUBLIC KEY BLOCK-----
...
-----END PGP PUBLIC KEY BLOCK-----
```

`GPG RECV_EXPORT` prints the peer's key with its encryption subkey. You do **not**
need to cross-sign it first; cross-signing only adds your own certification (the
web-of-trust step above). Save the block to `their-key.asc` and import it:

```sh
$ gpg --import their-key.asc
$ gpg --list-keys alice@example.com
pub   ed25519 ... [SCA]
      F832D01E763213413BA9ABFEFCC74F9458C137BE
uid           Alice Example <alice@example.com>
sub   nistp256 ... [E]
```

The `[E]` subkey means you can now encrypt to that peer (step 6). If you have
cross-signed the key, `GPG RECV_EXPORT` additionally carries your certification.

When you export **your own** key (**GPG ▸ Export Public** or `GPG EXPORT`), every
certification collected under **My Certifications** is attached automatically, so
a single `gpg --import` rebuilds your web of trust.

## Stuck?

- **Card not found:** `gpgconf --kill scdaemon`, then `gpg --card-status` again.
  Only one program can hold the smartcard at a time.
- **`gpg` hangs on a PIN prompt in scripts:** add
  `--pinentry-mode loopback --passphrase <PW1>`.
- **`No encryption subkey`:** re-import with `GPG EXPORT` (step 3); the imported
  key must show the `[E]` subkey.
- **PIN attempts:** the User PIN allows 3 tries, then it blocks. Reset a blocked
  User PIN from the host with the Admin PIN (`gpg --admin` ▸ `unblock`). Never
  guess PINs.
