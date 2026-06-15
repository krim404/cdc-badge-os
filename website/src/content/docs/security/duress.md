---
title: Duress PIN / self-destruct
description: The optional duress PIN that wipes the badge when entered, how to set it up, and exactly what the self-destruct erases.
sidebar:
  order: 5
---

The badge supports an **optional duress PIN**. When armed, entering it at the
unlock screen does not unlock the device: it triggers a **full wipe** and
reboots. To an observer (or a coercer) the entry looks exactly like any other
unlock attempt; there is no on-screen, log, or timing tell.

The duress PIN is **off by default**. Nothing happens until you deliberately set
one.

## How it behaves at unlock

On every unlock attempt the firmware checks the entered PIN against the duress
PIN **before** checking it against the real badge PIN. Because the two are
forced to be different (see below), there is no ambiguity:

- Entered value matches the **duress PIN** -> self-destruct, then reboot.
- Entered value matches the **badge PIN** -> normal unlock.
- Anything else -> failed attempt (counts against the badge-PIN
  [rate limit](/security/pin-lockout/)).

The self-destruct call does not return; from the user's point of view the badge
simply reboots as if it had been reset.

## Setting up a duress PIN

The duress PIN is configured from the Expert menu:

**Tools** -> **Expert** -> **Set duress PIN**

The setup reuses the PIN-change wizard:

1. **Re-authenticate** with your current badge PIN.
2. **Enter** the new duress PIN.
3. **Confirm** it.

Rules enforced by the firmware:

| Rule | Detail |
| --- | --- |
| Length | 4-8 digits, digits only (same as the badge PIN) |
| Must differ | The duress PIN must not equal the current badge PIN |
| Symmetric check | You also cannot later set a *badge* PIN equal to the duress PIN |
| Tamper-bound | The duress state is covered by the slot-0 attestation signature on the PIN record |

To disarm it, clear the duress PIN through the same PIN machinery
(`clearDuressPin`), which removes the stored duress hash and salt.

:::caution[Test it deliberately, not by accident]
A correct duress-PIN entry wipes the badge irrecoverably. Pick a value you will
not type by mistake, and remember that it is intentionally indistinguishable
from a normal unlock when entered.
:::

## What the self-destruct actually wipes

The wipe is **complete**, but it happens in two stages, so be precise about the
mechanism:

1. **Trigger (immediate).** `selfDestruct()` erases the NVS *build-profile
   marker* and reboots. It does **not** itself delete keys or secrets; it
   removes the marker that tells the boot path the device is provisioned.
2. **Wipe (on next boot).** Finding the marker absent, the boot path runs a full
   factory wipe before the device comes up:
   - **NVS partition** is erased and re-initialised (`wipeNvs`).
   - **Every TROPIC01 ECC slot (0-31)** is deleted.
   - **Every TROPIC01 R-Memory slot (0-511)** is erased (`wipeTropic`).

After the wipe completes, the build-profile marker is re-seeded so the device
boots fresh with defaults.

In scope of the wipe (everything that lives in NVS or the secure element):

| Wiped | Where it lives |
| --- | --- |
| FIDO2 / U2F credential keys | TROPIC01 ECC + R-Memory |
| GPG / OpenPGP keys | TROPIC01 ECC + R-Memory |
| TOTP seeds | TROPIC01 R-Memory |
| Password-vault entries | TROPIC01 R-Memory |
| Badge PIN, OpenPGP PINs, duress PIN | TROPIC01 R-Memory slot 0 |
| Attestation key (ECC slot 0) | TROPIC01 ECC |
| OS settings and module state in NVS | NVS partition |

:::note[Interrupted wipe is safe]
The build-profile marker is re-seeded only after the secure-element wipe
completes. If power is lost mid-wipe, the marker is still absent on the next
boot, so the wipe simply re-runs. It cannot leave a half-wiped device that
looks provisioned.
:::

:::caution[Scope limits]
The self-destruct erases NVS and the secure element. It does not reflash or
erase the firmware application image itself, and anything you exported off the
badge (backups, recovery codes) is naturally untouched.
:::

## What is verifiable here

| Property | Status |
| --- | --- |
| Duress PIN default | Off (opt-in) |
| Length | 4-8 digits |
| Must differ from badge PIN | Enforced both directions |
| Check order | Duress checked before badge verify |
| On match | `selfDestruct()`, no return, reboot |
| Wipe scope | Full NVS + all TROPIC01 ECC (0-31) and R-Memory (0-511) |
| Wipe timing | Trigger clears marker; wipe runs on next boot |
| Crash-safety | Marker re-seeded only after wipe completes |
