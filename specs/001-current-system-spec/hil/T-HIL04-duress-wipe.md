# T-HIL04 — Duress PIN triggers full self-destruct wipe; crash-safe re-run

**Status: non-blocking (hardware verification)**

**FRs covered**: FR-004, FR-070

Verifies that entering the configured duress PIN at the lock screen is indistinguishable from a
failed unlock (no UI/log/timing tell) yet initiates a full factory wipe on the next boot — NVS
reinit, all TROPIC01 ECC slots 0–31 deleted, all R-Memory slots 0–511 erased, attestation key
regenerated — and that an interrupted wipe re-runs on the next boot (never left half-wiped).
Maps to Success Criteria SC-009 and the crash-safety NFR-004.

## ⚠️ DESTRUCTIVE TEST — READ FIRST

This plan **irreversibly erases all on-device secrets**: FIDO2 credentials, GPG/SSH keys,
TOTP/HOTP/CR accounts, the password vault, vCards, OS/WiFi settings, and the attestation
identity. There is **no on-device recovery**. Only run this on a **scratch/dev badge** with no
material you want to keep, or after exporting an off-badge backup (note: backups never contain
secure-element private keys — those are gone permanently).

- Recovery: the firmware image survives the wipe. After the wipe completes the badge re-provisions
  to defaults (default badge PIN). If anything is left inconsistent, recover by reflashing via the
  serial `AUTH <pin>` then `BOOTLOADER` path.

## Prerequisites

### Hardware
- One **scratch** CDC Badge v1.0/v1.1 with a working TROPIC01.
- USB-C cable for serial inspection.

### Host tools
- Serial terminal at 115200 baud (`/dev/cu.usbmodem*`).
- `tools/coredump.py` available (only if a crash needs analysis — not expected here).

### Build profile
- Any beta build. The duress PIN is **default-off**; it must be configured first.
- Badge PIN known (dev: `0000`). Choose a distinct duress PIN (must differ from the badge PIN —
  bidirectionally enforced per FR-004).

## Procedure

1. **Seed recognisable secrets**: on the scratch badge, create at least one of each so the wipe
   is observable: one FIDO2 credential, one TOTP account, one password entry, one vCard. Note an
   on-device fingerprint of each (e.g. account name, credential RP).
2. **Configure duress PIN (FR-004)**: enable and set a duress PIN distinct from the badge PIN.
   - Attempt to set the duress PIN **equal** to the badge PIN; confirm it is rejected
     (bidirectional difference enforced).
3. **Indistinguishability check (FR-004 / SC-009)**: lock the badge. Enter the **duress PIN** at
   the lock screen. Observe carefully:
   - The on-screen response must look exactly like a normal **wrong** PIN (failed-attempt UI),
     with no special message, icon, or visibly different timing.
   - On the serial log, confirm there is no log line revealing wipe intent before reboot.
4. **Confirm boot marker erased**: the duress trigger erases the NVS boot marker and reboots.
   Observe the badge reboot.
5. **Wipe runs on next boot (FR-070)**: on the boot after the duress entry, confirm via serial
   that a full factory wipe runs: NVS reinit, ECC slots 0–31 deleted, R-Memory slots 0–511
   erased, attestation key regenerated.
6. **Verify secrets gone (SC-009)**: after the wipe completes and the badge re-provisions, unlock
   with the **default** badge PIN and confirm every secret from step 1 is gone (no FIDO2
   credential, no TOTP account, no password entry, no vCard).
7. **Crash-safe re-run (NFR-004 / spec Edge Cases)**: repeat steps 1–4 to set up a fresh duress
   trigger. During the **next** boot, while the wipe is running (or immediately after the boot
   marker is erased but before the wipe completes), **cut power** (unplug / power-cycle).
   - On the subsequent boot, confirm the wipe **re-runs** (boot marker still absent) and
     completes.
   - Confirm the device is never left in a half-wiped state (no partially surviving secrets).

## Pass criteria

- Step 2: a duress PIN equal to the badge PIN is rejected; a distinct one is accepted.
- Step 3: duress entry presents the identical failed-unlock UI as a wrong PIN, with no distinct
  message/icon/timing, and no pre-reboot serial line reveals wipe intent (SC-009).
- Step 5: the post-duress boot performs a full wipe — NVS reinit + ECC 0–31 deleted + R-Mem
  0–511 erased + attestation key regenerated (confirmed via serial log) (FR-070).
- Step 6: after wipe, none of the step-1 secrets are recoverable on-device; the badge accepts the
  default badge PIN (re-provisioned to defaults).
- Step 7: a power loss mid-wipe causes the wipe to re-run and complete on the next boot; the
  device is never left half-wiped (NFR-004).

## Notes

- **DESTRUCTIVE**: see the warning block above. Scratch badge only. The attestation identity is
  regenerated, so any externally registered FIDO2 credentials / exported GPG public keys tied to
  the old identity become invalid.
- **Flash conservation**: the duress trigger reboots the badge automatically — this is a soft
  reboot, **not** a flash. The whole plan requires **no flashing**. Only reflash (serial `AUTH`
  then `BOOTLOADER`) if step 7's power-loss test leaves the device wedged.
- **Timing the power cut (step 7)**: the wipe window is short. Watch the serial log for the
  wipe-start line and cut power immediately; repeat if the wipe completed before the cut. Record
  how far the log progressed before each cut.
