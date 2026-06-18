---
title: Companion tools
description: The host-side Python scripts in tools/ - flashing, plugin and file upload, serial and BLE consoles, backups, core-dump analysis, language-image building and the PlatformIO build helpers.
sidebar:
  order: 9
---

The `tools/` directory holds host-side Python scripts that talk to the badge or
support the build. Most run over the USB CDC serial port and accept `--pin` for
AUTH when secure serial is enabled; the BLE console talks over Bluetooth instead.

## Overview

| Tool | Purpose | Primary usage |
| --- | --- | --- |
| `flash_firmware.py` | Flash pre-built firmware (bootloader + partitions + app) from a local directory or a GitHub release. | `--dir <path>` or `--release <tag\|latest>`; `--port`, `--erase-nvs`. |
| `upload.py` | Unified upload over serial: install/manage plugins, write a language overlay, or stream an arbitrary file onto the plugins partition. | `--wasm <f> --meta <f>` (plus `--list`/`--info`/`--delete`/`--start`/`--stop`); `--lang-overlay <lang_<code>.json>`; `--put <local> [--dir] [--name]`. |
| `ble_serial.py` | Interactive serial console over BLE (Nordic UART Service), like a wireless `pio device monitor`. | run with no args to auto-scan; `--address <mac>`, `--name`, `--scan`. |
| `backup.py` | Drive the on-device backup command, transfer the encrypted container over the vFAT serial shell, and decrypt/encrypt it off-device. | `--export`/`--import <pass>`, `--delete`; `--download`/`--upload <file>`; `--decrypt`/`--encrypt` with `--out` and `--pass`. |
| `coredump.py` | Read an ESP32 core dump from flash and analyze it with GDB. | `python tools/coredump.py [port]`. |
| `build_lang_image.py` | Build the `plugins` FAT image, seeding `system/i18n/lang_<code>.json` and bundling the user-area demo files, for first-flash provisioning. | `--lang-dir <dir>` (default `assets/i18n`), `--assets-dir <dir>` (default `assets/vfat`), `--output`, `--partition-size`. |
| `2fa.py` | Provision OATH credentials (TOTP/HOTP) over the on-device serial command. | `--list`, `--add-totp`/`--add-hotp <name> <secret>`, `--get`/`--del <index>`, plus `--issuer`/`--digits`/`--period`/`--algo`/`--counter`. |
| `start_webflasher.py` | Serve the local web-flasher over HTTP against the most recent local build and open the browser. | `--port`, `--no-browser`, `--build`, `--lang`. |
| `pio_submodules.py` | PlatformIO pre-build hook: initialize the pinned git submodules if missing. | runs automatically during the build. |
| `pio_component_manager.py` | PlatformIO build hook: enable the ESP-IDF Component Manager. | runs automatically during the build. |
| `pio_python_deps.py` | PlatformIO build hook: install Python build dependencies into the PlatformIO env. | runs automatically during the build. |

## Notes on usage

### Flashing

`flash_firmware.py` flashes the three binaries to the ESP32-S3 layout
(bootloader at `0x0`, partition table at `0x8000`, app at `0x50000`). Point it at
a directory of `.bin` files (`--dir`) or let it pull a GitHub release
(`--release latest`). `--erase-nvs` additionally wipes the NVS partition, which
resets all settings. The port is auto-detected if `--port` is omitted.

### Upload

`upload.py` is the one tool for getting content onto the badge over serial. It
installs and manages plugins (`--wasm`/`--meta`, plus list/info/delete/
start/stop), writes a UI language file to `/vfat/system/i18n/` and reloads it
(`--lang-overlay`), or streams any file onto the plugins partition via the vFAT
`RECEIVE` path (`--put`). It needs `pyserial`.

### Backups

`backup.py` reproduces the firmware's encrypted container format byte-for-byte.
On-device modes (`--export`, `--import`, `--delete`) drive the AUTH-gated backup
command; `--download`/`--upload` move the container over the vFAT shell; and the
off-device `--decrypt`/`--encrypt` modes work with only the passphrase, no badge
attached. See [Encrypted backup & restore](/guide/backup-restore/).

### Core dumps

`coredump.py` reads the core-dump partition from flash and runs it through GDB to
produce a backtrace. It expects `esp-coredump` and `esptool` and uses the
PlatformIO-bundled GDB and firmware ELF, so run it from the project root after a
local build.

### Language image

`build_lang_image.py` builds a wear-levelled FAT image of the `plugins`
partition. It seeds the `lang_<code>.json` overlay files into `system/i18n/`
(`--lang-dir`, default `assets/i18n`) and bundles the user-area files from
`--assets-dir` (default `assets/vfat`, shipping `demo.jpg` and `demo.md`) into
the partition root, so a fresh flash already has the languages installed and the
file area populated. It is invoked by `start_webflasher.py --lang` and can be
run on its own. See [Languages](/power/languages/).

### Build helpers

The three `pio_*` scripts are PlatformIO build hooks, not interactive tools. They
run during the build to initialize submodules, enable the ESP-IDF Component
Manager, and install Python build dependencies.

:::note
`2fa.py` and `start_webflasher.py` are not in the core power-user workflow but
ship in `tools/` for convenience: `2fa.py` provisions two-factor credentials
from the host, and `start_webflasher.py` runs the browser-based flasher locally.
:::
