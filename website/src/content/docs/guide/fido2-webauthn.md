---
title: Passkeys / WebAuthn (FIDO2)
description: Registering passkeys, signing in, on-device confirmation, and managing FIDO2 credentials on the badge.
sidebar:
  order: 7
---

The badge is a FIDO2 / WebAuthn security key. It registers passkeys for websites
and apps, signs you in, and confirms every operation with a button press on the
device. Private keys are generated inside the TROPIC01 secure element and never
leave the chip.

## How the badge connects

The badge speaks FIDO over USB. It presents itself as a CTAPHID HID device using
the FIDO Alliance usage page, the same transport a USB security key uses, so
browsers and operating systems recognise it without a driver.

- Transport: USB HID (CTAPHID), 64-byte reports.
- Reported versions: `FIDO_2_0`, `FIDO_2_1`, and legacy `U2F_V2`.

There is no BLE or NFC FIDO transport: the authenticator reports only `usb`.

## Registering a passkey

When a site offers to create a passkey (or "register a security key"), pick the
badge as the authenticator. The badge then asks you to confirm:

1. The website triggers registration.
2. The badge wakes its display and shows a **Register Key** prompt with the
   site (relying party) name.
3. Approve with <kbd>Y</kbd> or deny with <kbd>N</kbd>.
4. On approval the badge generates a new key pair in the secure element and
   returns the new credential.

If a passkey for the same site and user account already exists, the prompt
changes to **OVERWRITE KEY!** and asks you to confirm replacing it before the
old key is discarded.

You have 30 seconds to respond. If you do not, the request times out and the
site reports a failure.

### Key types

The badge supports two credential algorithms and offers whichever the site
requests:

| Algorithm | Curve |
| --- | --- |
| ES256 (ECDSA) | NIST P-256 |
| EdDSA | Ed25519 |

Most websites use ES256 / P-256.

## Signing in

When a site asks you to sign in with your passkey:

1. The site triggers authentication.
2. The badge shows a **Sign In** prompt with the site name.
3. Approve with <kbd>Y</kbd> or deny with <kbd>N</kbd>.

The badge signs the challenge with the matching key and returns the assertion.
Each credential carries its own signature counter that increases on every
sign-in, which lets servers detect a cloned key.

## User presence and the PIN

Every register and sign-in needs **user presence**: a physical <kbd>Y</kbd>
press on the badge. The badge cannot be used silently.

User verification (proving it is *you*, not just that the device is present) is
done with a PIN. The badge uses the **WebAuthn ClientPIN protocol (version 2)**:

- The PIN is the badge PIN. You set and change it on the device, not through
  the browser. WebAuthn's "set a PIN for your security key" dialog is not used;
  the badge rejects PIN-set/change requests sent over USB.
- When a site requires user verification, the platform asks for the PIN, runs
  the ClientPIN exchange, and the badge checks it against its stored PIN hash.
- After too many wrong PIN attempts the PIN becomes blocked for the FIDO
  channel.

See [Lock screen & PIN](/guide/lock-and-pin/) for how the badge PIN
is set.

## Passkeys vs non-discoverable credentials

WebAuthn distinguishes two credential kinds, and the badge supports both:

| Kind | What it means |
| --- | --- |
| Resident / discoverable (passkey) | The credential is stored on the badge and can be listed and used without the site providing a credential ID first. These appear in the on-device list. |
| Non-resident (server-side) | The credential is tied to an ID the server stores; the server presents that ID at sign-in. |

On this badge both kinds occupy one secure-element key slot per credential,
because every credential's private key is generated and kept inside the secure
element. There is no "unlimited server-side keys" mode.

## Managing credentials on the badge

Open the **WebAuthn** entry in the main menu to see your stored passkeys.

- The list shows each stored credential.
- Press <kbd>3</kbd> on a selected entry to open its menu:
  - **Details** shows the site, whether it is a resident key, and the curve.
  - **Delete** removes the credential. The key slot is wiped in the secure
    element.

Some platforms can also enumerate and delete credentials over USB
(credential management). The badge requires a verified PIN before it will list
or delete credentials that way.

### Resetting all credentials

A platform "reset authenticator" request erases **all** FIDO2 credentials. The
badge requires you to confirm this on the device with <kbd>Y</kbd> first.

## Capacity

Credential storage is bounded by the secure element's key slots assigned to
FIDO2.

| Limit | Value |
| --- | --- |
| FIDO2 secure-element key slots | 26 (slots 5 through 30) |
| Maximum stored credentials | 26 (one key slot each) |
| Credential ID length | 64 bytes |

Once all FIDO2 slots are in use, new registrations fail until you delete a
credential. The exact slot allocation is defined in `main/tropic_slot_map.h`.

:::note
The maximum is the number of FIDO2 key slots in the secure element. Resident and
non-resident credentials both consume one slot, so the limit applies to all
credentials together.
:::
