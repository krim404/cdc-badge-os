---
title: Security overview
description: How the badge protects your secrets, what runs automatically in the background, and where to read the details.
sidebar:
  order: 1
---

This section explains, in plain language, how the CDC Badge protects its
secrets, and it is honest about the limits. Every detailed page is verified
against the firmware source.

## The short version

- Your private keys live in a **separate secure-element chip (TROPIC01)**, not
  in the main processor's flash. The firmware can ask it to sign and to hand
  back *public* keys, but the operation interface has no call to read a private
  key out.
- The keys for FIDO2, GPG, and the device's own identity are **generated inside
  the secure element**, so for those keys no private value ever existed off-chip.
- The device is gated by a **badge PIN**. Too many wrong tries trigger a short,
  self-recovering lockout, not a permanent brick.
- As an OpenPGP smartcard the badge also has the standard **PW1/PW3 card PINs**,
  which follow smartcard rules: their counters are persistent and can lock the
  card.
- An optional **duress PIN** can wipe the device when entered.
- The PIN record is **signed by a chip-bound key**, so tampering with it forces
  a reset to defaults rather than trusting the altered data.

:::caution[Beta firmware]
This is pre-1.0 beta. Data on the badge can be wiped by a flash, a breaking
change, or a duress entry, and some features are unfinished. Keep off-badge
backups and read [Beta status & caveats](/security/caveats/).
:::

## What runs automatically in the background

A few security-relevant things happen without you asking:

- **Device identity key on first boot.** An attestation/identity key is
  generated on-chip in secure-element slot 0 the first time it is missing, and
  its public-key hash is recorded to detect later tampering. Details on the
  [attestation page](/security/attestation/).
- **On-chip key generation on demand.** When you register a FIDO2 credential or
  create a GPG key, the key pair is generated inside the secure element rather
  than imported.
- **Tamper check at unlock.** When PIN data is loaded, its signature is verified
  against the chip-bound slot-0 key; a failed check silently re-initialises to
  defaults.
- **Layout guard at boot.** A build-profile byte is compared at boot; a mismatch
  triggers a full factory wipe before the device comes up. This is the same
  mechanism the duress self-destruct uses. See
  [Beta status & caveats](/security/caveats/).
- **Self-recovering PIN lockout.** A blocked badge PIN restores its attempts
  after a short recovery window, automatically.

## Read next

| Page | What it covers |
| --- | --- |
| [Secure element & automatic key generation](/security/secure-element-keys/) | The TROPIC01 chip, slot model, slot allocation, what the chip does, and which keys are generated on-chip |
| [FIDO2 attestation key & AAGUID](/security/attestation/) | The per-device, self-signed FIDO2 attestation identity in slot 0 |
| [PINs & lockout](/security/pin-lockout/) | The badge PIN's recovery timer versus the OpenPGP PW1/PW3 terminal lockout |
| [Duress PIN / self-destruct](/security/duress/) | The optional wipe-on-entry PIN and exactly what it erases |
| [Beta status & caveats](/security/caveats/) | Data-loss reality, no-migration policy, WIP features, and the `DEBUG_MODE` flag |

## What we do and do not claim

The pages in this section state only what the source supports. In particular:

- We say the firmware **cannot extract a private key through its operation
  interface** and that on-chip generated keys never leave the chip. We do **not**
  claim physical tamper resistance or that lab extraction is impossible; that is
  a property of the TROPIC01 hardware, not of this firmware.
- We give exact lockout counts and durations, slot ranges, and wipe scopes,
  each backed by a source line in the evidence ledger.
