---
title: Testing
description: Host unit tests in CI and the on-device harness that verifies the shipped firmware on a real badge.
sidebar:
  order: 5
---

CDC Badge OS has two kinds of tests: host-side unit tests that run in CI, and on-device tests that
verify the shipped firmware on a real badge.

## Host unit tests (CI)

Portable, hardware-independent logic is tested on the build machine with the PlatformIO `native`
environment:

```bash
~/.platformio/penv/bin/pio test -e native
```

No badge is connected and no flash is performed. The tests live under `test/host/` (one folder per
test, Unity framework) and cover CRC, base64, CP437 conversion, CTAP2 CBOR encoding, and
message-transfer framing. CI runs this stage before building the firmware.

## On-device tests

On-device tests verify the **installed release as-is** on a real badge. The release is flashed once;
every test reaches its state at runtime (serial commands, power-cycle, factory-reset/duress, button
input). No test-only or alternate-profile firmware is flashed per test or per state.

Both categories below are **optional**: the harness is a developer/QA tool and is not part of CI.

List the catalog without a device:

```bash
~/.platformio/penv/bin/python tools/ondevice/run.py --list
```

### Automatic

Driven end-to-end over the USB-CDC serial console by `tools/ondevice/`, unattended and
self-cleaning:

```bash
~/.platformio/penv/bin/python tools/ondevice/run.py --pin 0000
~/.platformio/penv/bin/python tools/ondevice/run.py --pin 0000 --mutating --slow
```

The default run is fast and non-destructive. `--mutating` adds tests that change device state and
restore it (PIN change, GPG key generation, backup round-trip, attestation-certificate import, GPG
cross-signing); `--slow` adds the ~65 s lockout-recovery test. The automatic catalog covers system
smoke, PIN change, serial-AUTH lockout and recovery, TOTP, the password vault, vCard storage, GPG
key generate/export, encrypted-backup round-trip, i18n overlay reload, time and NVS, WiFi control,
secure-element health, FIDO2 attestation-certificate import (with a key-mismatch rejection check),
and GPG cross-signing of a received key (with a fingerprint-mismatch rejection check and OpenPGP
packet-structure verification of the exported certification).

### Standalone automated drivers

These run fully unattended without the serial catalog, over their native interface:

```bash
# USB-HID FIDO2 authenticatorLargeBlobs (0x0C) write/read round-trip
~/.platformio/penv/bin/python tools/fido2_largeblob.py --pin 0000

# Software RSA self-test (generate/sign/verify/decrypt) for 2048/3072/4096-bit keys
~/.platformio/penv/bin/python tools/gpg_selftest.py --pin 0000 --bits all

# OpenPGP smartcard (CCID) functional test through stock gpg
~/.platformio/penv/bin/python tools/gpg_card_test.py --pw1 123456 --pw3 12345678
```

`fido2_largeblob.py` obtains a `largeBlobWrite` pinUvAuth token, writes a large-blob array, reads it
back through device NVS, and asserts a byte-exact round-trip. `gpg_selftest.py` exercises the
OpenPGP RSA software keys over the `GPG RSA_SELFTEST` serial command. `gpg_card_test.py` drives the
badge as an OpenPGP card through stock GnuPG (card PINs supplied via loopback): enumeration,
public-key / SSH export, signing (PSO:CDS), decryption (PSO:DECIPHER), authentication (INTERNAL
AUTHENTICATE via the ssh-agent), and the admin cardholder-data path. It pulls the card's full public
key from the default keyring, so that key must be present there.

### Semi-automatic

These run last and need an operator: a button press / user-presence touch, a host CTAP2 or CCID
stack, a second badge, or a look at the display. Append them with `--semi`:

```bash
~/.platformio/penv/bin/python tools/ondevice/run.py --pin 0000 --semi
```

The harness drives the serial-checkable parts and prompts the operator for the rest, then asks for a
pass/fail confirmation. The semi-automatic catalog covers FIDO2 register/assert, OpenPGP
sign/decrypt over CCID, BLE vCard transfer, cross-signature return over BLE, BLE HID keyboard, USB
keyboard / Yubico OTP, e-paper refresh, the lock-screen lockout countdown, the duress wipe, keypad
/ T9 input, and the plugin sandbox.

The duress-wipe test is destructive (it erases all keys and settings) and runs only with
`--mutating` plus an explicit on-screen confirmation. The written step-by-step procedures and pass
criteria for the semi-automatic tests are under `specs/001-current-system-spec/hil/`.

### Selecting tests

Run exactly the tests you name (this ignores the `--mutating`/`--slow`/`--semi` gating):

```bash
~/.platformio/penv/bin/python tools/ondevice/run.py --pin 0000 --only A-PWD,A-2FA
```

### Requirements

The harness needs `pyserial` (see `tools/requirements.txt`). The attestation and cross-signing tests
additionally need `cryptography`; `tools/fido2_largeblob.py` needs `fido2`; `tools/gpg_card_test.py`
needs GnuPG 2.2+. The serial commands the harness uses are listed in the
[Serial command reference](/dev/proto/serial-commands/).
