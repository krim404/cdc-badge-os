---
title: Flashing the firmware
description: How to put a CDC Badge into bootloader mode and flash CDC Badge OS.
sidebar:
  order: 2
---

CDC Badge OS targets the **ESP32-S3** on the CDC Badge v1.0/v1.1. This page covers the three ways
to flash it: the browser web flasher, the Python flash tool, and the serial bootloader command.

:::danger[Pre-1.0 software]
This firmware is pre-1.0. A flash can wipe stored data, and breaking changes between versions
are expected. If an update boots into a broken state, run the **Factory Setup** flash to wipe
the plugin partition and re-install the language overlay.
:::

## Step 1: Enter bootloader mode

The badge must be in the ESP32-S3 ROM download (bootloader) mode before flashing. There are
two ways in.

### Software (badge already running CDC Badge OS)

- On the badge UI, open **Tools → Expert → Bootloader**, or
- over the USB serial console, authenticate first and then request the bootloader:

  ```text
  AUTH <pin>
  BOOTLOADER
  ```

The `BOOTLOADER` serial command requires an authenticated session, so you must send `AUTH`
with the badge PIN first. After a successful `AUTH`, `BOOTLOADER` reboots the device into USB
download mode.

### Hardware (always works)

1. Press and hold the **FLASH** button.
2. Briefly press **RESET** while still holding **FLASH**.
3. Release **FLASH**.

:::note
In bootloader mode the e-paper display does not update and the badge can look switched off.
That is expected.
:::

## Step 2: Choose a flashing method

### Option A: Browser web flasher (easiest)

The web flasher uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) and flashes
directly from the browser over Web Serial.

Requirements:

- Google Chrome or Microsoft Edge (desktop) with Web Serial support.
- A data-capable USB cable. Charge-only cables will not enumerate the device.
- The badge in bootloader mode (step 1).

The flasher offers two installs:

| Install | What it does |
|---------|--------------|
| **Firmware Update** | Replaces only the firmware. Installed plugins, language overlay, settings and PINs stay intact. It shows an "Erase device" checkbox that is unchecked by default. Use this for normal version upgrades. |
| **Factory Setup** | Wipes installed plugins and re-installs the language overlay. Use this on a fresh badge, or when an update leaves things broken. |

:::caution
The "Erase device" checkbox in **Firmware Update** does **not** re-install the language
overlay. For a fully clean device, use **Factory Setup** instead.
:::

The first boot after **Factory Setup** takes longer than usual because the firmware wipes
every used secure-element slot and re-initialises storage. Do not unplug while the display is
still updating.

### Option B: Python flash tool

`tools/flash_firmware.py` flashes the pre-built CI/release binaries with `esptool`. It can
flash from a local directory or download a GitHub release.

```bash
# Download and flash the latest GitHub release
python tools/flash_firmware.py --release latest

# Flash a specific release tag
python tools/flash_firmware.py --release v1.0.0

# Flash from a local directory of .bin files
python tools/flash_firmware.py --dir ./artifacts/

# Pick the serial port manually
python tools/flash_firmware.py --dir ./artifacts/ --port /dev/cu.usbmodem1101

# Wipe settings (erase the NVS partition) after flashing
python tools/flash_firmware.py --release latest --erase-nvs
```

What the tool does:

- Targets chip `esp32s3` at 460800 baud, flash mode `dio`, 80 MHz, 16 MB.
- Auto-detects the serial port (or use `--port`).
- Resets into the app automatically after flashing (`--after hard_reset`).
- With `--erase-nvs`, it erases the NVS region (`0x9000`, `0x5000` bytes) after flashing, which
  resets all settings.

It writes three binaries at these offsets:

| Offset | Binary |
|--------|--------|
| `0x0` | bootloader |
| `0x8000` | partitions |
| `0x50000` | firmware (app) |

:::note
The application partition lives at `0x50000`, not the ESP-IDF default of `0x10000`.
:::

### Option C: Serial bootloader command

If the badge already runs CDC Badge OS, you do not need the FLASH/RESET button dance. Use the
serial path from step 1 (`AUTH <pin>` then `BOOTLOADER`) to reboot into download mode, then
run the web flasher or the Python tool. After flashing, the device hard-resets back into the
new firmware on its own.

## Partition and flash layout

The on-device partition table:

| Partition | Type | Offset | Size |
|-----------|------|--------|------|
| `nvs` | NVS (settings) | `0x9000` | `0x47000` |
| `app0` | app (factory) | `0x50000` | `0xDA0000` |
| `plugins` | FAT (plugins + language overlay) | `0xDF0000` | `0x200000` (2 MB) |
| `coredump` | coredump | `0xFF0000` | `0x10000` |

The CI builds two install manifests for the web flasher, both targeting `chipFamily`
`ESP32-S3`:

### `manifest.json` (Firmware Update)

Sets `new_install_prompt_erase: true` and flashes three parts:

| Part | Offset (decimal) | Offset (hex) |
|------|------------------|--------------|
| `bootloader.bin` | 0 | `0x0` |
| `partitions.bin` | 32768 | `0x8000` |
| `firmware.bin` | 327680 | `0x50000` |

### `manifest_factory.json` (Factory Setup)

Flashes the same three parts plus the initial plugins/language image:

| Part | Offset (decimal) | Offset (hex) |
|------|------------------|--------------|
| `bootloader.bin` | 0 | `0x0` |
| `partitions.bin` | 32768 | `0x8000` |
| `firmware.bin` | 327680 | `0x50000` |
| `plugins_initial.bin` | 14614528 | `0xDF0000` |

The factory image's `plugins_initial.bin` lands at `0xDF0000`, the start of the `plugins`
partition, which is why Factory Setup resets the plugin/language partition while a plain
Firmware Update leaves it untouched. The seed image carries the language overlays and the
`demo.jpg` and `demo.md` files in the user file area.

## After flashing

- Plugins are optional and live in the separate 2 MB FAT partition. They can be installed and
  updated without re-flashing the firmware, via the plugin web installer.
- For a first boot, allow extra time after a Factory Setup while storage re-initialises.

:::note
Internal documentation links on this site use root-relative paths. If the site is served
under a base path, those links may need the base prefix applied.
:::
