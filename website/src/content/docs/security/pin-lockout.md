---
title: PINs & lockout
description: The badge PIN with its rate-limiting recovery timer, versus the OpenPGP card PINs (PW1 user / PW3 admin) with smartcard-style terminal lockout.
sidebar:
  order: 4
---

The badge has two independent PIN systems with **different lockout behaviour**.
Confusing them is the most common source of "did I just brick my badge?"
worries, so this page keeps them strictly apart.

- The **badge PIN** unlocks the device UI and the serial console. Its lockout
  is a temporary **rate limit** that recovers automatically.
- The **OpenPGP card PINs** (PW1 / PW3) follow **smartcard semantics**: their
  retry counters are persistent and reaching zero is terminal until reset.

All PINs are managed by `PinManager` and stored, signed, in secure-element
R-Memory slot 0 (see [Secure element & key generation](/security/secure-element-keys/)).

:::note[The two PIN-hash forms are protocol-driven, not an inconsistency]
The badge PIN (which doubles as the FIDO2 CTAP2 ClientPIN) is stored as
`LEFT(SHA-256(PIN), 16)`, while the OpenPGP PW1/PW3 PINs use Iterated+Salted S2K.
This is dictated by the external protocols: CTAP2 ClientPIN transmits exactly the
first 16 bytes of `SHA-256(PIN)`, so the authenticator can only compare that
value, whereas the OpenPGP card advertises a KDF-DO and receives an S2K hash. The
duress PIN is internal-only and also uses S2K. See
[ADR-0004](/dev/adr/0004-two-kdf-pin-hashing/).
:::

## Badge PIN

| Property | Value | Source |
| --- | --- | --- |
| Length | 4-8 digits, digits only | `PinManager.h` `BADGE_PIN_MIN`/`MAX`; `PinManager.cpp` |
| Default | `123456` | `PinManager.h` `DEFAULT_BADGE_PIN` |
| Stored as | `LEFT(SHA256(PIN), 16)` | `PinManager.cpp` `computeBadgeHash` |
| Max attempts | 3 (`MAX_RETRIES`) | `PinManager.h` |
| Comparison | constant-time-style | `PinManager.cpp` `compareHash` |

### How the badge PIN lockout actually works

The badge PIN does **not** use a persistent attempt counter on the chip. Only a
binary "locked" flag is stored in R-Memory; the retry count lives in RAM. The
behaviour is a self-recovering rate limit:

1. On boot the firmware grants **one** attempt (or zero if the locked flag was
   set) and starts a recovery timer.
2. A correct PIN restores the counter to 3 and clears any lock.
3. A wrong PIN decrements the counter. When it hits zero, the locked flag is
   set and the recovery timer (re)starts.
4. After the **60-second** recovery window expires, the counter is restored to
   3 and the locked flag is cleared automatically.

:::note[The badge PIN cannot be permanently bricked]
Reaching zero attempts blocks entry only until the 60-second recovery timer
expires; then attempts are restored. There is no persistent counter that
exhausts forever. The verify path has no debug bypass either; the recovery is
unconditional in source.
:::

The serial console reuses the same badge PIN and the same lockout state: an
exhausted badge PIN blocks `AUTH` over serial for the recovery window, and the
console reports the remaining seconds.

### Changing the badge PIN

Set or change it from the badge's PIN settings. A new badge PIN must satisfy the
4-8 digit rule and, if a [duress PIN](/security/duress/) is armed, must differ
from it.

## OpenPGP card PINs (PW1 / PW3)

When the badge is used as an OpenPGP smartcard over USB CCID, it presents the
two standard OpenPGP PINs. These are **separate** from the badge PIN, with their
own values and their own counters.

| PIN | Role | Min length | Max length | Default | Max attempts |
| --- | --- | --- | --- | --- | --- |
| **PW1** | User PIN (sign/decrypt/authenticate) | 6 | 16 | `123456` | 3 |
| **PW3** | Admin PIN (card management) | 8 | 16 | `12345678` | 3 |

Both are hashed with OpenPGP **iterated + salted S2K** over SHA-256 (default
100000 iterations) with a per-PIN random salt, as required by the OpenPGP card
KDF-DO (see [ADR-0004](/dev/adr/0004-two-kdf-pin-hashing/)).

### Terminal lockout

PW1 and PW3 use **smartcard semantics**: the retry counter is decremented and
persisted **synchronously before** the comparison, so a power-cycle in the
middle of a verify cannot resurrect an attempt. When a counter reaches zero,
that PIN is **blocked**.

- A blocked **PW1** can be unblocked by the **admin (PW3)** via the OpenPGP
  *RESET RETRY COUNTER* command, or by the *Resetting Code* path.
- A blocked **PW3** is **terminal**: there is no host command that resets the
  admin PIN's counter on the card. Recovering an admin-locked card means wiping
  the device and re-initialising.

:::caution[An admin-locked OpenPGP card is not host-recoverable]
The *RESET RETRY COUNTER* APDU on this firmware unblocks **PW1** (with admin
authorisation or the Resetting Code). It does not reset **PW3**. Treat the admin
PIN with care; three wrong PW3 entries lock card management until a full device
wipe.
:::

The OpenPGP card protocol itself (CCID transport, APDUs, status words) is
covered in the developer protocol docs.

## At a glance

| | Badge PIN | OpenPGP PW1 | OpenPGP PW3 |
| --- | --- | --- | --- |
| Scope | device UI + serial | OpenPGP card user | OpenPGP card admin |
| Length | 4-8 | 6-16 | 8-16 |
| Counter | RAM, self-recovering | persistent, terminal | persistent, terminal |
| After max attempts | 60 s recovery, then restored | blocked until PW3/RC reset | blocked, wipe to recover |
