---
title: Production hardening & lockdown
description: The tools/provision.py workflow for rotating the TROPIC01 sync key and locking down the ESP32-S3 with flash encryption, secure boot v2, and download-mode fuses - and how to update a locked badge afterwards.
sidebar:
  order: 9
---

This page documents how to take a badge from the beta state into a hardened
production state using `tools/provision.py`, and how to keep updating it once it
is locked.

:::danger[Untested and destructive]
Every operation on this page is **irreversible in part** and has **never been
run against real hardware** - it is a destructive, one-way procedure. A mistake
can permanently brick the secure element or freeze the badge so it can never be
reflashed. Read the whole page, keep a badge you can afford to lose, and do a
`--dry-run` first.
:::

## Why this exists

Two facts drive the whole workflow:

- A stock badge authenticates to the TROPIC01 with the **public libtropic
  production pairing key** (`lt_sh0priv_prod0`, compiled into the firmware).
  Anyone who has that key can open a secure session to the chip. Rotating it to
  a unique per-deployment key closes that door.
- The ESP32-S3 ships with **no flash encryption, no secure boot, and an open
  ROM download mode**. Flash can be dumped and modified. Enabling the hardware
  security features fixes that.

The firmware has a **single factory app partition and no OTA slots**
(`partitions.csv`), so the only update path is serial reflashing. That single
fact shapes the entire lockdown design: whatever you burn, keep a serial update
route open unless you deliberately want to freeze the badge forever.

## The disarmed safety latch

`tools/provision.py` ships **disarmed**. Running any subcommand refuses
immediately:

```
REFUSED: provision.py is DISARMED.
Open tools/provision.py and set  ARMED = True  to enable it.
```

To use the tool you must edit the source and set `ARMED = True` **and** export
`CDC_PROVISION_EXPERIMENTAL=1` (the tool has never been validated on real
hardware; the variable is the explicit experimental opt-in). On top of the
latch, every destructive operation also requires the
`--i-understand-this-is-irreversible` flag and a typed, operation-specific
confirmation phrase, and every eFuse mutation first reads back the chip's
actual eFuse state (`espefuse summary`).

Before anything runs, the tool verifies that `esptool`/`espefuse`/`espsecure`
actually execute and are **v5.x** (hyphenated CLI). The ESP-IDF-bundled v4.x is
rejected; every external command is checked for a zero exit code, a failure
aborts immediately.

## Per-chip state and manifest

All secrets and progress live in per-chip directories under `tools/secrets/`
(git-ignored, mode 0700):

- `esp-<mac>/` - `fe_key.bin`, `sb_key.pem`, `sb_digest.bin`, `manifest.json`
- `tr01-<chipid>/` - `sync_key.json`, `manifest.json`

`manifest.json` binds the chip identity (ESP MAC / TROPIC01 chip id), SHA-256
hashes of the key files, eFuse snapshots and completed steps together. Key
files that do not match their manifest hash abort the run, so two badges can
never be mixed up and a wrong/tampered key is never flashed or burned.
Existing key files are never overwritten.

## Subcommand overview

| Subcommand | What it does | Reversible? |
|---|---|---|
| `rotate-key` | Rotate the TROPIC01 SH0 sync key to a unique key | soft phase yes, invalidate no |
| `lock-esp` | Flash encryption + secure boot v2 + JTAG/download hardening | **no** |
| `reflash` | Sign + pre-encrypt + serial-flash a new image onto a locked badge | n/a |
| `disable-download` | Final download-mode lock (security or permanent) | **no** |

Mandatory order: `rotate-key` (soft) → `rotate-key --verify` →
`rotate-key --invalidate-old-slot` → `lock-esp` (state machine, step by step)
→ optional `disable-download`. rotate-key comes FIRST: the custom pairing key
is compiled into the firmware and stays readable from a plaintext flash dump
until flash encryption is on.

## 1. Rotate the TROPIC01 sync key

The pairing key lives in the chip's I-Memory (slots 0-3, one-shot writes,
permanent invalidation). The write and invalidate run **through the firmware**,
which owns the SPI secure session, via two serial commands that only exist in a
build compiled with `-DFEATURE_PROVISIONING=1`.

:::caution[Provisioning firmware only]
`TR01 PAIR_WRITE` and `TR01 PAIR_INVALIDATE` are gated behind the
`FEATURE_PROVISIONING` compile flag (default **0**). A normal build does not
contain them, so they cannot be reached over serial by accident. Build a
dedicated provisioning firmware, rotate the key, then flash the normal release.
:::

The rotation is deliberately **three-staged**:

**Soft phase** (reversible - the old slot stays valid as a fallback):

```bash
# 1. build + flash a provisioning firmware
PLATFORMIO_BUILD_FLAGS='-DFEATURE_PROVISIONING=1' pio run -e cdc_badge_usb -t upload

# 2. generate a new X25519 keypair, write it into free slot 1, emit the header
python tools/provision.py rotate-key --pin 0000 \
    --i-understand-this-is-irreversible
```

This generates the keypair, saves the private key to
`tools/secrets/tr01-<chipid>/sync_key.json` (git-ignored, bound to the TROPIC01
chip id in `manifest.json`), writes
`components/cdc_hal/include/cdc_hal/pairing_key_custom.h`, and sends
`TR01 PAIR_WRITE 1 <pubkey>`. Rebuild the firmware against the new key and
flash it:

```bash
PLATFORMIO_BUILD_FLAGS='-DCDC_PAIRING_KEY=CDC_PAIRING_KEY_CUSTOM' \
    pio run -e cdc_badge_usb -t upload
```

**Verify phase** (mandatory before anything irreversible):

```bash
python tools/provision.py rotate-key --verify --pin 0000
```

This proves the badge really authenticates via the NEW slot: the `VERSION`
`Profile:` line must report `pairing_slot=1`, `TR01 SESSION` must open a secure
session, and the `TR01 INFO` chip id must match the manifest. Only a recorded
successful verification unlocks the next phase.

**Invalidate phase** (irreversible - refused until `--verify` succeeded):

```bash
python tools/provision.py rotate-key --invalidate-old-slot 0 --pin 0000 \
    --i-understand-this-is-irreversible
```

Three independent guards protect this: the manifest must contain
`pairing.verified=true`, the target slot must differ from the live
`pairing_slot` the badge reports, and the firmware driver itself refuses to
write or invalidate the slot it is currently authenticating with.

## 2. The release build (opt-in, guarded)

`lock-esp` flashes images from the dedicated lockdown env:

```bash
CDC_PROVISION_EXPERIMENTAL=1 pio run -e cdc_badge_release
```

The env layers `sdkconfig.release` on top of the normal config:
`CONFIG_SECURE_BOOT=y` (v2, RSA-3072, binaries signed **externally** by
provision.py), `CONFIG_SECURE_FLASH_ENC_ENABLED=y` (release mode),
`CONFIG_SECURE_SERIAL=y`, `-DDEBUG_MODE=0`, and NVS encryption
(`CONFIG_NVS_ENCRYPTION=y`, flash-encryption-based scheme).

Under flash encryption the `nvs` partition would otherwise stay **plaintext** -
the badge keeps app data there (GPG stores, FIDO2 metadata, browser bookmarks,
plugin NVS). This env therefore uses a dedicated partition table
`partitions.release.csv` that shrinks `nvs` by 4 KB and adds an encrypted
`nvs_keys` partition; the XTS keys are generated per device by `nvs_flash_init()`
on first boot, consume no eFuse key block, and need no app code change. The dev
env `cdc_badge_usb` keeps `partitions.csv` and stays unencrypted.

:::danger[Never build or upload this by accident]
A bootloader built from `sdkconfig.release` **self-burns secure-boot and
flash-encryption eFuses on the first boot of an unlocked chip**. Two guards
enforce this (`tools/pio_release_guard.py`):

- building refuses without `CDC_PROVISION_EXPERIMENTAL=1`
- `pio run -e cdc_badge_release -t upload` is **always** refused - the only
  flash path is provision.py, which writes the image only after the eFuses
  were already burned externally

The default `pio run` (env `cdc_badge_usb`) is completely unaffected and stays
unsigned/unencrypted.
:::

## 3. Lock the ESP32-S3 (state machine)

`lock-esp` is a **strict state machine**. `lock-esp` alone reads the chip's
actual eFuse state plus the per-chip manifest and prints which steps are done
and which one is next; `lock-esp --continue` executes exactly that next step.
Skipping, repeating, or reordering is impossible, and a chip state that
contradicts the sequence (a later step already done) aborts with a manual-
inspection error.

```bash
python tools/provision.py lock-esp --dry-run              # plan only
python tools/provision.py lock-esp --port /dev/ttyACM0    # live status
python tools/provision.py lock-esp --port /dev/ttyACM0 --continue \
    --dir .pio/build/cdc_badge_release --pin 0000 \
    --i-understand-this-is-irreversible                   # next step
```

The sequence (the badge stays recoverable at every boundary; the plaintext
firmware keeps booting until step 5):

1. `gen_fe_key` - host-generate the flash-encryption key
2. `gen_sb_key` - host-generate the RSA-3072 SB key + public-key digest
3. `burn_fe_key` - burn the FE key into a **free** key block (chosen from the
   live eFuse summary, not hardcoded), read back the purpose
4. `burn_sb_digest` - burn the SB digest (`SECURE_BOOT_DIGEST0`), read back
5. `flash_release` - validate the release artifacts (unambiguous filenames,
   signature verified with `espsecure verify-signature`, sizes checked against
   the partition regions), sign bootloader + app, encrypt everything, write all
   segments in **one** `write-flash` call
6. `enable_lockdown` - burn `SPI_BOOT_CRYPT_CNT=7` + `SECURE_BOOT_EN=1` in one
   call, then **verify the boot over serial**: `VERSION` must report
   `flash_enc=1 secure_boot=1 debug=0 secure_serial=1`; the result is recorded
   in the manifest
7. `harden` - `DIS_DOWNLOAD_MANUAL_ENCRYPT`, `DIS_DOWNLOAD_ICACHE/DCACHE`,
   `HARD_DIS_JTAG`, `DIS_USB_JTAG`, `DIS_DIRECT_BOOT`, read back

Every eFuse step demands the danger-gate confirmation phrase; key generation
refuses to overwrite existing files; every burn is read back and the step only
counts as done when the chip actually reports the new state. USB-OTG download
fuses are intentionally never burned - that would kill the badge's TinyUSB.
`DIS_USB_JTAG` only disables the USB-Serial-JTAG debug bridge; TinyUSB
(USB-OTG) is unaffected.

:::danger[Keep the keys]
`tools/secrets/esp-<mac>/fe_key.bin` and `sb_key.pem` are the **only** way to
reflash the badge over serial afterwards. Back them up securely and never
commit them (`tools/secrets/` is git-ignored).
:::

## 4. Reflash a locked badge

With flash encryption in release mode, the UART/USB bootloader can no longer
encrypt on the fly. Instead you pre-encrypt host-side and write the ciphertext:

```bash
python tools/provision.py reflash --dir .pio/build/cdc_badge_release --dry-run
```

The badge is identified by its eFuse MAC; the keys are taken from
`tools/secrets/esp-<mac>/` and must match the SHA-256 hashes in the manifest.
For bootloader (`0x0`), partitions (`0x8000`), firmware (`0x50000`): sign with
the SB key (bootloader and app only - SBv2 does not sign the partition table,
and an appended signature block at `0x8000` would overwrite NVS at `0x9000`),
verify the signatures, size-check every image against its partition region,
`espsecure encrypt-flash-data --aes-xts --address <offset>`, then write all
segments in one `esptool --no-stub write-flash` call. Put the badge into
download mode first (`AUTH <pin>` then `BOOTLOADER`).

## 5. Disable the download mode (final)

Refused until every `lock-esp` step is complete **and** the manifest records a
verified boot of the final signed/encrypted firmware. The last lock has two
levels. Choose deliberately - they trade updatability for security.

**Security download mode (default, still updatable):**

```bash
python tools/provision.py disable-download --mode security \
    --i-understand-this-is-irreversible
```

Burns `ENABLE_SECURITY_DOWNLOAD`. The ROM bootloader stays, but only encrypted
writes are allowed and reads/dumps are blocked. Serial reflashing (section 4)
keeps working. Note that `espefuse` itself can no longer burn further fuses
after this.

**Permanent lock (frozen forever):**

```bash
python tools/provision.py disable-download --mode permanent \
    --i-understand-this-is-irreversible
```

Burns `DIS_DOWNLOAD_MODE`. The ROM download mode is destroyed. Because this
firmware has **no OTA path**, the badge can then **never be reflashed, recovered,
or updated again**. The tool requires a second typed confirmation
(`PERMANENTLY-DISABLE-ALL-FLASHING`) for this level.

## eFuse reference (ESP32-S3)

Names verified against the ESP-IDF `esp_efuse_table.csv`. Bits only flip 0→1.

| eFuse | Effect |
|---|---|
| `SPI_BOOT_CRYPT_CNT` | Flash encryption enable; value `7` = final |
| `SECURE_BOOT_EN` | Enable secure boot v2 (RSA-3072 on S3) |
| `SECURE_BOOT_DIGEST0` | SB public-key digest slot (stays readable) |
| `DIS_DOWNLOAD_MANUAL_ENCRYPT` | Block bootloader on-the-fly encryption |
| `DIS_DOWNLOAD_ICACHE` / `DIS_DOWNLOAD_DCACHE` | Block cache access in download mode |
| `HARD_DIS_JTAG` | Permanently disable pad JTAG |
| `DIS_USB_JTAG` | Disable the USB-Serial-JTAG debug bridge (TinyUSB unaffected) |
| `DIS_DIRECT_BOOT` | Disable legacy/direct boot |
| `ENABLE_SECURITY_DOWNLOAD` | Secure download mode (encrypted writes only) |
| `DIS_DOWNLOAD_MODE` | Kill ROM download mode entirely (no recovery) |

:::note[esptool syntax]
`provision.py` targets standalone esptool v5 (hyphenated commands, `burn-efuse`;
pinned as `esptool>=5,<6` in `tools/requirements.txt`). Before anything runs it
executes `<tool> version` and refuses anything that is not v5.x - the esptool
bundled with ESP-IDF 5.5 is v4.x (underscore commands) and is rejected.
:::

## Tests

`pytest tools/tests/` runs the hardware-free test suite: state-machine
transitions (fresh chip, partial state, occupied key blocks, contradictions),
failure injection for external commands, artifact validation (ambiguous names,
oversized images), the rotate-key verify/invalidate gating, and the tool
preflight. No test burns anything; hardware acceptance is a separate,
explicitly authorized step on an expendable badge.

## Source files

- `tools/provision.py` - the workflow tool (disarmed by default)
- `tools/pio_release_guard.py` - build/upload guard for `cdc_badge_release`
- `sdkconfig.release` - lockdown sdkconfig overlay (opt-in env only)
- `partitions.release.csv` - lockdown partition table with encrypted `nvs_keys`
- `tools/tests/test_provision.py` - hardware-free test suite
- `components/cdc_hal/include/cdc_hal/pairing_key_config.h` - build-time key select
- `components/cdc_hal/src/Tropic01Element.cpp` - `pairingKeyWrite` / `pairingKeyInvalidate` / `activePairingSlot`
- `components/serial_cmd/src/SerialCmd.cpp` - `TR01 PAIR_WRITE` / `PAIR_INVALIDATE` (gated by `FEATURE_PROVISIONING`), extended `VERSION` Profile line
- `components/cdc_core/include/cdc_core/feature_flags.h` - `FEATURE_PROVISIONING`
