---
title: Beta status & caveats
description: The pre-1.0 beta reality, the no-migration policy, work-in-progress features, and the DEBUG_MODE build flag.
sidebar:
  order: 6
---

CDC Badge OS is **pre-1.0 beta** firmware. The security model described in the
rest of this section is real and source-verified, but the project is explicit
that data on the badge is **not** authoritative and that some features are
unfinished. Read this page before you trust the badge with anything you cannot
re-create.

## Data loss on flash

Every flash can wipe all stored data on the badge: FIDO2/U2F credentials, TOTP
seeds, password-vault entries, GPG keys and the badge PIN. The project's own
guidance is blunt about it:

:::caution[Treat the badge as a working copy, not the authoritative store]
Keep an independent, off-badge backup of anything you cannot afford to lose:
FIDO2 recovery codes, a password-manager export, GPG private subkeys, TOTP
seeds. A flash, a breaking change, or a [duress wipe](/security/duress/) can
clear the device. Backup and restore is covered in the user guide.
:::

## No-migration policy

The project does **not** ship migration code. Because it is pre-1.0, every
release is allowed to be data-breaking:

- There are no version-detection or fallback paths that read an older on-device
  format.
- New on-device formats are introduced freely and old ones are deleted in the
  same change.
- When the on-device data layout changes incompatibly, the firmware **wipes and
  re-initialises** instead of converting.

This is enforced mechanically by a **build-profile byte**. The firmware stores a
profile byte in NVS and compares it to the value compiled into the running
build. On a mismatch (including first boot or a malformed value), the next boot
performs a full factory wipe (NVS partition plus every TROPIC01 ECC and
R-Memory slot) before the device comes up. The same mechanism backs the
[duress self-destruct](/security/duress/).

:::note[The build-profile byte is a software guard, not anti-rollback]
The source describes the build-profile byte as the *beta-phase software guard*.
Bypass-resistant enforcement against an active attacker (Secure Boot v2 with
anti-rollback) is on the 1.0 roadmap, not in the current build.
:::

## Work-in-progress features

No features are currently flagged as work-in-progress. Badge-to-badge transfer
(vCard and GPG public-key exchange over the
[message-transfer framework](/dev/proto/message-transfer/)) and the BLE serial
console are implemented and usable. The badge is still pre-1.0 beta, so
behaviour can change between releases.

## The `DEBUG_MODE` build flag

`DEBUG_MODE` is a compile-time flag that **defaults to on** (`1`). It primarily
controls how much the firmware logs, and in particular whether sensitive
material is logged:

- **Log verbosity.** With `DEBUG_MODE` on, the default log level is `DEBUG`;
  off, it is `WARN`.
- **Sensitive-value logging.** Debug builds log cryptographic intermediates in
  the FIDO2 PIN/ECDH path and dump binary buffers in hex. In a release build the
  hex-dump helper is compiled to swallow its input, specifically because hex
  dumps can leak key material.
- **Serial log gating.** In a release build (with Secure Serial enabled),
  INFO/DEBUG log output is held back until a serial session authenticates;
  errors and warnings still flow so boot failures stay visible. Debug builds do
  not gate logs this way.
- **Diagnostics.** Debug builds also emit extra diagnostics such as main-task
  stack low-water logging.

`DEBUG_MODE` is folded into the build-profile byte, so changing it is itself a
breaking change that triggers a factory wipe on the next boot.

:::caution[Production builds must disable DEBUG_MODE]
The default `DEBUG_MODE=1` is a development setting. It makes the firmware log
verbosely and can emit secret-bearing values (ECDH shares, key buffers) over the
serial console. Build production firmware with `DEBUG_MODE=0`.
:::

:::note[What DEBUG_MODE does not do]
In this firmware, the badge-PIN lockout/recovery logic has no `DEBUG_MODE`
bypass: the recovery timer behaves the same in debug and release builds (see
[ADR-0012](/dev/adr/0012-build-profiles-and-defaults/)). Do not assume
`DEBUG_MODE` only affects logging in every subsystem, but for the badge PIN
specifically there is no debug shortcut around the rate limit.
:::

## Summary

| Caveat | Reality |
| --- | --- |
| Maturity | Pre-1.0 beta |
| Data durability | Can be wiped by flash, breaking change, or duress |
| Migrations | None; breaking changes wipe + reinit |
| Layout guard | Build-profile byte (software, not anti-rollback) |
| WIP features | None currently flagged |
| `DEBUG_MODE` default | On (`1`); must be off for production |
