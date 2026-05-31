# Security Hardening Guide for CDC Badge

This document describes the current security posture of the CDC Badge firmware
and the hardening steps required for a production-grade deployment. Most
hardware-level hardening is **not yet enabled** in the beta firmware; this
document collects the residual work and tracks which items are scheduled for
the 1.0 release.

> **Status:** beta firmware. The application-layer protections listed below
> are in place, but the hardware-level countermeasures (Secure Boot v2, Flash
> Encryption, NVS Encryption, JTAG disable, anti-rollback) are intentionally
> deferred during active development. They will be enabled with 1.0.

---

## Implementation Status

| Feature | State | Notes |
|---------|-------|-------|
| Badge PIN brute-force protection | Done | RAM-only counter, persisted locked flag, 60s recovery, brick-resistant |
| OpenPGP PW1/PW3 smartcard semantics | Done | Pre-decrement persist, terminal at 0, no software recovery |
| TROPIC01 key storage | Done | Private keys stay in TROPIC01 ECC slots and never leave the chip |
| PIN hash storage | Done | R-Memory slot 0, ECDSA-P256 signed by chip-bound attestation key |
| Random PIN salts | Done | Generated from TROPIC01 RNG at first boot |
| Build-profile factory reset | Done | NVS + TROPIC01 wipe on `DEBUG_MODE` / `FEATURE_SECURE_SERIAL` change |
| Serial command authentication | Done | `CONFIG_SECURE_SERIAL=y`, PIN-gated privileged commands |
| Log key-material redaction | Done | `log_hex` compiled out in release builds, log auth gate active |
| Secure Boot v2 | TODO | Bootloader signing not enabled in sdkconfig |
| Flash Encryption | TODO | Required to make the on-chip flash content non-extractable |
| NVS Encryption | TODO | Requires Flash Encryption |
| App anti-rollback (`secure_version`) | TODO | Required to make the build-profile guard tamper-resistant |
| JTAG disable (eFuse) | TODO | Burn `JTAG_DISABLE` before shipping |
| Custom TROPIC01 pairing certificate | TODO | Engineering-sample certificate still in use |

## Threat Model

The badge is designed as a personal hardware security key. The relevant
attacker classes are:

1. **Lost / stolen device:** attacker has physical possession but no special
   tooling. PIN brute-force resistance and on-chip key isolation are the
   relevant defences. Both are in place today.
2. **Coerced flash:** attacker has physical possession and can connect a host
   that flashes arbitrary firmware. Without Secure Boot v2 + Flash Encryption,
   the attacker can read encrypted-at-rest material out of flash and load
   firmware that bypasses application-level checks. The 1.0 hardening line is
   what closes this. The beta firmware does **not** defend against this
   attacker.
3. **Glitching / decapping / side-channel:** out of scope. TROPIC01 handles
   the operations on sensitive material and is the relevant countermeasure.

---

## Build-Profile Factory Reset

The firmware writes a single byte (`BUILD_PROFILE_BYTE`, see
`components/cdc_core/include/cdc_core/feature_flags.h`) into NVS namespace
`boot_profile`. On every boot the firmware compares the stored byte with the
compiled-in value. If the byte is missing, malformed, or different, the boot
sequence wipes the NVS partition, re-initialises it blank, and wipes every
TROPIC01 R-Memory slot (0-511) and ECC slot (0-31) once the secure-element
session is available. Module storage and PIN material are then re-initialised
to defaults on next access.

This catches two cases:

- **Honest user reflashes with different `DEBUG_MODE` / `FEATURE_SECURE_SERIAL`:**
  the device returns to defaults instead of presenting a confused mix of
  state from the previous build profile.
- **Fresh device, missing profile byte, or malformed value:** previous
  firmware state is wiped before the new profile starts persisting data.

A forgotten PIN is recovered via the web-flasher's reset action, not by
hand-flipping build flags.

A structural NVS error during the check (partition unreadable for reasons
unrelated to the profile byte's presence or format) is treated as a hardware
defect: the device enters lockdown (`LockdownReason::NVS_UNREADABLE`) rather
than performing a destructive wipe that might compound the fault.

**This is a software-only guard.** An attacker who can flash arbitrary
firmware can simply flash a firmware that does not perform the check, in
which case stored data survives. Real enforcement of the same property
requires Secure Boot v2 + anti-rollback (see Roadmap).

---

## PIN Security

### Badge PIN

- 4-8 numeric digits, default `123456`
- LEFT(SHA-256(PIN), 16) stored in R-Memory slot 0
- Payload is signed with the chip-bound attestation key (ECC slot 0); a
  tampered slot or regenerated attestation key triggers re-init to defaults
- Retry counter lives in RAM only
- R-Memory persists only a binary `locked` flag
- Boot grants 1 attempt (0 if locked) and arms the 60-second recovery timer;
  on timer expiry the counter is restored to MAX_RETRIES and the locked flag
  is cleared
- Crash mid-verify cannot brick the badge: the counter is RAM-only and the
  locked flag is only persisted when the RAM counter genuinely transitions
  to zero

### OpenPGP PW1 / PW3

- Salted iterated SHA-256 (OpenPGP S2K, 100000 iterations) in R-Memory slot 0
- Random salts generated at first boot, persisted along with the hashes
- Smartcard semantics: the retry counter is **decremented and persisted
  synchronously before** the verify, so a power-cycle between hash and persist
  cannot resurrect attempts. Reaching zero is terminal until an admin reset.

---

## TROPIC01 Hardening (TODO)

### Custom Pairing Certificate

The firmware currently uses the engineering-sample pairing material that
ships with the libtropic submodule. For a production deployment each device
should be provisioned with a unique pairing key.

Two viable provisioning models:

1. **Per-device key in flash-encrypted storage.** Provision at manufacturing
   and rely on Flash Encryption (per-device key burned into eFuse) so the
   pairing private key cannot be read from a flash dump. Requires Flash
   Encryption to be enabled first.
2. **Password-derived key.** Encrypt the pairing private key with a user
   passphrase that is entered at boot. Trades operational convenience for
   not needing per-device provisioning. Decryption is in-RAM and the
   plaintext is zeroised after `lt_session_start`.

Once a TROPIC01 chip is paired with a key, the pairing slot used cannot be
overwritten with a different key without invalidating the slot, so this is
a one-shot setup step.

### Slot Map

The authoritative module-to-slot mapping is the code in
`main/tropic_slot_map.h`; a prose mirror is in the
[Module Development Guide](MODULE_DEVELOPMENT.md). SYSTEM owns ECC slot 0
(attestation) and R-Memory slot 0 (PIN material).

---

## ESP32-S3 Hardening (TODO)

These items are deliberately disabled during beta development to keep the
flash workflow fast (frequent reflashing, debug builds, occasional rollback
to an older build). They are required before a release build can be
considered production-grade. The exact ESP-IDF options live in `sdkconfig`;
configure them via `pio run -t menuconfig` rather than hand-editing.

### Flash Encryption

Release mode is **irreversible** once the eFuse is burned. The chip then only
accepts firmware images encrypted with the per-device key, so on-flash material
is not extractable from a dump. Develop with it disabled, enable it for
manufactured units.

### Secure Boot v2

The sdkconfig today only sets the Secure Boot v2 *preferred* capability flag,
which is not the same as enabling Secure Boot. Enabling requires generating a
signing key with `espsecure.py`, signing the bootloader and app, and burning
the public-key digest into eFuse.

### App Anti-Rollback

Combined with Secure Boot v2, this makes the build-profile guard enforceable:
release builds (e.g. `DEBUG_MODE=0`, `SECURE_SERIAL=1`) are assigned a higher
secure version than debug builds. Once a release build has booted, the
bootloader refuses any image with a lower secure version, and the rollback bit
cannot be cleared.

### NVS Encryption

Requires Flash Encryption. Encrypts the NVS partition so configuration data
extracted from a flash dump is not readable.

### JTAG Disable

Permanently disables JTAG by burning the relevant eFuses with `espefuse.py`.
Irreversible.

---

## Hardening the Beta Yourself

If you want to harden your own beta unit before 1.0 lands, the minimum
useful set is:

1. Set `DEBUG_MODE=0` and `FEATURE_SECURE_SERIAL=1` in your build flags.
2. Enable Flash Encryption in release mode in your local sdkconfig and
   re-flash. From this point the chip is locked to your encrypted images.
3. Enable Secure Boot v2 with your own signing key. Keep the signing key
   offline.
4. Enable `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` and pick a `secure_version`
   for your hardened builds that is higher than any debug build you might
   want to flash later. You cannot go back.
5. Burn `JTAG_DISABLE`.
6. Replace the engineering-sample pairing certificate with one you generated
   yourself before establishing the first TROPIC01 session on this device.

Each of these steps is irreversible. The maintainers do not provide a
recovery path for a beta unit that has been bricked by an incorrect signing
configuration.

---

## Operational Recovery

The supported recovery path during beta development is:

- **Forgotten Badge PIN:** trigger a full factory reset from the web-flasher.
  NVS and every used TROPIC01 R-Memory / ECC slot are wiped and the default
  PIN is restored.
- **Locked PW1 / PW3:** PW1 can be reset by PW3; PW3 has no software reset
  path. If both are exhausted the only recovery is the same web-flasher
  factory wipe.
- **Lost/forgotten passphrase entirely:** factory wipe as above.

Once Secure Boot + anti-rollback are in place (1.0), these recovery paths
will no longer be available — that is the intent. A production unit with a
forgotten PIN will be unrecoverable by design.

---

## References

- [ESP-IDF Security Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/index.html)
- [ESP-IDF Anti-Rollback](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/ota.html#anti-rollback)
- [TROPIC01 / libtropic](https://github.com/tropicsquare/libtropic)
- [FIDO2 Security Considerations](https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-20210615.html#security-considerations)
