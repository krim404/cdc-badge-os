# T-HIL05 — PIN lockout: budget exhausted (1 at boot) → 60 s countdown → recovery, no brick; shared serial AUTH

**Status: non-blocking (hardware verification)**

**FRs covered**: FR-002, FR-003

Verifies the badge-PIN brute-force protection. The retry budget is RAM-resident: the device grants
only **1 attempt immediately after a cold boot** (0 if it was locked) and starts a 60-second recovery
timer at boot; on timer expiry the budget refills to 3. Exhausting the budget on a wrong entry
triggers a 60-second lockout with a visible countdown, after which the correct PIN is accepted with
no permanent brick. The recovery behaves identically in debug and release builds (no bypass), and the
serial `AUTH` gate shares the same lockout state as the lock screen. Maps to Success Criterion SC-002.

## Prerequisites

### Hardware
- One provisioned CDC Badge v1.0/v1.1.
- USB-C cable to the host.

### Host tools
- Serial terminal at 115200 baud (`/dev/cu.usbmodem*`, `BadgeV1`).
- A stopwatch (or the badge's own countdown) to time the 60-second window.

### Build profile
- Runs against the installed release as-is; no reflash.
- Badge PIN known (dev: `0000`). Have a deliberately **wrong** PIN ready (e.g. `9999`).
- Serial-AUTH portion requires the secure-serial gate active (`FEATURE_SECURE_SERIAL=1`); if the
  flashed image has it off, the serial-AUTH steps are not applicable — record as not-exercised.

## Procedure

1. **Baseline unlock (SC-001)**: power-cycle the badge; at the lock screen enter the **correct** PIN.
   Confirm the main menu opens. Re-lock (rescue chord N+Y, or wait for auto-lock).
2. **Cold-boot single attempt (FR-002)**: power-cycle the badge so it is freshly booted, then enter
   the **wrong** PIN **once**. Confirm a single wrong entry immediately engages a **60-second
   lockout** with a visible countdown — at cold boot the budget is 1, not 3.
3. **Refusal during lockout (FR-002)**: while the countdown is running, attempt the **correct** PIN.
   Confirm entry is refused until the countdown reaches zero.
4. **Recovery refills the budget to 3 (FR-002/FR-003 / SC-002)**: let the countdown expire WITHOUT
   unlocking. Confirm the budget is restored to 3 — now enter the **wrong** PIN and observe up to 3
   attempts before the next 60-second lockout engages (the recovery-window budget is 3). Then, after
   the next expiry, enter the **correct** PIN and confirm the main menu opens.
5. **No permanent brick (SC-002)**: confirm at no point a firmware reflash was required to recover —
   the device self-recovered after each 60 s window.
6. **RAM-resident retry counter (spec Edge Cases / NFR-004)**: from a recovery-window state (budget
   3), enter one wrong attempt (below the threshold), then power-cycle the badge before locking.
   Confirm after reboot the device is at a clean lock screen, the counter did not corrupt, and the
   correct PIN works (a fresh cold boot again grants 1 attempt).
7. **Shared serial AUTH lockout (FR-002 + FR-081)** *(only if secure-serial gate active)*:
   - Confirm wrong serial `AUTH` attempts draw from the **same** RAM budget as the lock screen: a
     wrong `AUTH` after the cold-boot budget is already exhausted is refused with a lockout response.
   - Cross-check sharing: with the budget exhausted at the lock screen, confirm serial `AUTH` is
     simultaneously locked out (the counters are shared, not independent).
   - After the 60 s expires, confirm `AUTH <correct-pin>` succeeds over serial.

## Pass criteria

- Step 1: correct PIN opens the menu in a single attempt (SC-001).
- Step 2: a single wrong PIN at cold boot engages a 60-second lockout with a visible on-screen
  countdown — confirming the boot budget is 1 (FR-002).
- Step 3: during lockout, even the correct PIN is refused until the timer expires.
- Step 4: after expiry the budget refills to 3; the recovery-window allows up to 3 wrong attempts
  before re-locking, and the correct PIN unlocks after expiry (FR-002).
- Step 5: recovery required **no reflash** — 0% permanent brick (SC-002 / FR-003).
- Step 6: a power-cycle mid-attempt leaves a clean lock screen with an uncorrupted retry counter
  (correct PIN works; a cold boot again grants 1 attempt) (NFR-004).
- Step 7 (if exercised): serial `AUTH` shares the lockout — wrong serial attempts draw from the same
  budget as lock-screen attempts and the 60 s lockout blocks both; the gate clears together after
  expiry (FR-081).

## Notes

- **No flashing**: the whole plan runs against the installed release and is fully self-recovering by
  design. The `A-LOCK` automatic test in `tools/ondevice/` covers the serial-AUTH lockout and the
  60 s recovery unattended.
- **Not destructive**: no secrets are erased. This is the safe counterpart to T-HIL04; do NOT confuse
  a wrong **badge** PIN (recoverable lockout) with a **duress** PIN (full wipe).
- The lockout timer is 60 s; budget roughly 2 minutes per full lock/recover cycle. The cold-boot
  budget of 1 (vs. 3 in a recovery window) is an intentional anti-power-cycle throttle documented in
  `PinManager.h`.
