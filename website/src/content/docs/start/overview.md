---
title: Overview
description: What the CDC Badge OS is, the hardware it runs on, and what it can do.
sidebar:
  order: 1
---

CDC Badge OS is the firmware for the **CDC Badge v1.0/v1.1** hardware security key. It turns the
badge into a self-contained device for FIDO2/WebAuthn, SSH and GPG keys, time-based one-time
passwords, a password vault, and a sandboxed plugin runtime, all driven from an on-device
e-paper display and a 12-button keypad. No companion app is required for the core features.

## The hardware

| Part | What it is |
|------|------------|
| SoC | ESP32-S3 (`mcu: esp32s3`, 240 MHz) |
| Flash | 16 MB |
| PSRAM | Octal PSRAM, 80 MHz |
| Secure element | TROPIC01 |
| Display | 2.9" e-paper, 296 x 128 pixels, monochrome, with a frontlight |
| Input | 12-button keypad (phone-style T9 text entry) |
| Charging / power | BQ25895 charger IC |
| I/O expander | TCA9535 (reads the keypad matrix) |
| Expansion | SAO port, Grove port, and a second I2C bus on the expansion header |

The ESP32-S3 is the main processor. **Private keys never leave the TROPIC01 secure element**:
the ECC key slots live inside the secure element and the firmware references them by slot number.

:::note
PSRAM is configured as octal-mode PSRAM running at 80 MHz. The exact PSRAM capacity is not
fixed in the firmware source; the firmware simply uses whatever PSRAM the module provides.
:::

### Display rendering

The display is a low-power e-paper panel, so the screen only refreshes when something changes.
A momentary "stale" look between updates is normal for e-paper.

## What it can do

The feature set is built from self-contained modules. The capacities below are taken directly
from the on-device secure-element slot map (`main/tropic_slot_map.h`).

### Authentication and keys

- **FIDO2 / WebAuthn** passkeys and U2F second-factor, with private keys stored in TROPIC01
  ECC slots.
- **TOTP authenticator** for time-based one-time passwords. Up to **100 accounts**
  (secure-element R-Memory slots 31-130).
- **Password vault** for stored credentials. Up to **364 entries**
  (secure-element R-Memory slots 137-500).
- **GPG / OpenPGP smartcard** over USB CCID for sign, encrypt, decrypt and SSH.

### Connectivity

- **USB** CDC serial console and HID.
- **Bluetooth Low Energy** HID (acts as a Bluetooth keyboard for auto-type).
- **WiFi** for time synchronisation over NTP, controlled from the on-device
  **Tools → WiFi** menu (and also over the serial console).

### Extensibility

- **WASM plugin runtime**: third-party plugins run sandboxed inside a WebAssembly runtime
  (WAMR) instead of being compiled into the firmware. Plugins live in a separate 2 MB FAT
  partition and can be installed or removed without re-flashing the firmware. See the
  intermediate guides for installing plugins.
- **Expansion ports**: an SAO port and a Grove port are exposed for add-ons, plus a second
  I2C bus on the expansion header.

### On-device UI

- E-paper menu navigation with the 12-button keypad.
- T9-style text entry for typing on the keypad.
- English and German interface languages (more can be added as language overlay files).
- A PIN-protected [lock screen](/guide/lock-and-pin/).

## Where to go next

- **Flash the firmware**: see [Flashing the firmware](/start/first-flash/).
- **Lock screen and PIN**: see [Lock screen & PIN](/guide/lock-and-pin/).

:::note
Internal documentation links on this site use root-relative paths. If the site is served
under a base path, those links may need the base prefix applied.
:::
