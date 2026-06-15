# T-HIL02 — OpenPGP CCID: card-status, sign/decrypt/SSH, PW1/PW3 semantics

**Status: non-blocking (hardware verification)**

**FRs covered**: FR-040, FR-041, FR-042, FR-043

Verifies the badge as an OpenPGP CCID smartcard over USB: a standard GPG toolchain enumerates
the card without custom drivers, on-chip key generation works, sign/decrypt/SSH-auth operations
succeed gated by PW1, and the PW1/PW3 PIN semantics hold — including the **terminal PW3 lockout**
(wipe-only recovery). Maps to Success Criterion SC-006.

## Prerequisites

### Hardware
- One provisioned CDC Badge v1.0/v1.1 with a working TROPIC01 secure element.
- USB-C cable to the host.

### Host tools
- GnuPG 2.2+ (`gpg`, `gpg-agent`, `gpg-connect-agent`) and `scdaemon`.
- `pcscd` running (or GnuPG's internal CCID driver). No custom badge driver is permitted —
  recognition must work with the stock toolchain (FR-040 / SC-006).
- For SSH auth: OpenSSH client with `gpg-agent --enable-ssh-support`.
- Serial terminal at 115200 baud (optional) for badge-side logs.

### Build profile
- The GPG/CCID module (`mod_gpg`) must be enabled. Note: the keyboard/OTP HID modules
  (`mod_usbhid`, `mod_otphid`) are mutually exclusive with GPG CCID over the single keyboard HID
  slot; ensure those are disabled for this run.
- Badge PIN known (dev: `0000`). OpenPGP defaults: PW1 = `123456` (min 6), PW3 = `12345678`
  (min 8).

## Procedure

1. **Generate keys on-device (FR-041/FR-043)**: on the badge, open the GPG menu and run key
   generation. Enter a name, optional email, and select curve Ed25519 (default) or P-256.
   Confirm SIG, DEC and AUT keys are created. (DEC is fixed to P-256 ECDH regardless of the
   chosen SIG/AUT curve.)
2. **Export public key (FR-043)**: trigger public-key export on the badge. Confirm a PEM block
   is emitted on the serial console and a QR code is shown on the display. Confirm no private key
   material is ever printed/exported.
3. **Enumerate the card (FR-040)**: connect the badge over USB and run:
   ```bash
   gpg --card-status
   ```
   Confirm the card is enumerated as an OpenPGP card and the three key fingerprints (SIG/DEC/AUT)
   appear, matching the on-device key set.
4. **Sign (FR-040/FR-042)**: sign a small file gated by PW1.
   ```bash
   echo "hil-test" | gpg --local-user <SIG-fpr> --sign --armor > /tmp/hil.sig
   ```
   Enter PW1 when prompted. Confirm a valid signature is produced and verifies:
   ```bash
   gpg --verify /tmp/hil.sig
   ```
5. **Decrypt (FR-040/FR-041)**: encrypt to the DEC key, then decrypt with the card.
   ```bash
   echo "hil-secret" | gpg --encrypt --recipient <DEC-fpr> --armor > /tmp/hil.enc
   gpg --decrypt /tmp/hil.enc
   ```
   Enter PW1 if prompted. Confirm the plaintext `hil-secret` is recovered.
6. **SSH authentication (FR-040/FR-041)**: with `gpg-agent` SSH support enabled, export the AUT
   key as an SSH public key and authenticate.
   ```bash
   gpg-connect-agent updatestartuptty /bye
   ssh-add -L            # AUT key appears as an SSH identity
   ```
   Add the printed key to a test host's `authorized_keys` and confirm an SSH login succeeds,
   entering PW1 when prompted.
7. **PW1 attempt budget + admin reset (FR-042)**: enter a wrong PW1 three times during a sign
   operation. Confirm the card blocks PW1. Then use PW3 (Admin PIN) to reset PW1
   (`gpg --card-edit` → `admin` → `passwd` → reset/unblock). Confirm PW1 is usable again.
8. **PW3 terminal lockout (FR-042 — DESTRUCTIVE to card management)**: enter a wrong PW3 three
   times via `gpg --card-edit` → `admin` → `passwd` (admin PIN). After the third wrong entry,
   confirm the card refuses all further administration. Confirm there is **no host-side reset**
   that restores PW3 (e.g. `factory-reset` in `gpg --card-edit` is rejected / does not recover
   it). Recovery is wipe-only (see Notes).

## Pass criteria

- Step 1: SIG, AUT and DEC keys are reported created on-device; DEC is P-256 even when SIG/AUT
  are Ed25519.
- Step 2: a PEM public key appears on serial and a QR appears on the display; no private key is
  output anywhere.
- Step 3: `gpg --card-status` succeeds with the **stock** GnuPG toolchain (no custom driver) and
  shows three fingerprints matching the on-device set.
- Step 4: signing succeeds after PW1 entry; `gpg --verify` reports a good signature.
- Step 5: decryption recovers the exact plaintext.
- Step 6: the AUT key is offered as an SSH identity and an SSH login succeeds gated by PW1.
- Step 7: three wrong PW1 entries block PW1; a PW3-driven reset restores PW1 use (PW1 is
  admin-resettable).
- Step 8: three wrong PW3 entries terminally block card administration; no host-side command
  restores it. This non-recoverable state is **by design** (spec Edge Cases / FR-042).

## Notes

- **DESTRUCTIVE (PW3, step 8)**: exhausting PW3 permanently disables card administration for the
  current key set. The **only** recovery is a full device wipe (Expert → factory reset, or a
  duress wipe) which deletes the GPG ECC slots and re-provisions. Run step 8 last, and only when
  you are prepared to wipe the GPG keys. Skip step 8 if the badge holds keys you want to keep.
- **DEC key caveat (NFR-006 / RF-03)**: the DEC private key is software-stored (AES-256-GCM in
  R-Memory) and decrypted to RAM per ECDH op, then zeroized. This is the documented exception to
  on-chip-only key custody; SIG/AUT keys live in TROPIC01 ECC slots and never leave.
- **Flash conservation**: no flashing is required for steps 1–8. If a reflash is needed
  afterward (e.g. to recover from step 8), use the serial `AUTH <pin>` then `BOOTLOADER` path.
- Allow re-enumeration to settle (a few seconds) after connecting before running `gpg`.
