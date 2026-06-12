# CDC Badge OS

Modular firmware for the CDC Badge v1.0/v1.1 hardware security key featuring TROPIC01 secure element.

![CDC Badge Demo](docs/demo.jpg)

> ## ⚠️ Beta - data loss on flash
>
> This firmware is **pre-1.0 beta**. Every flash can wipe all stored data
> on the badge: FIDO2/U2F credentials, TOTP seeds, password-vault entries,
> GPG keys and the badge PIN. Layout-breaking changes between versions can
> trigger a re-initialisation; use the web-flasher's reset action to wipe
> the device deliberately.
>
> **Treat the badge as a working copy, not the authoritative store.** Keep an
> independent backup of anything you cannot afford to lose (FIDO2 recovery
> codes, password-manager export, GPG private subkeys, TOTP seeds) somewhere
> off-badge.

## Features

| Feature | Status | Description |
|---------|--------|-------------|
| **FIDO2/WebAuthn** | Working | FIDO 2.1 passwordless authentication via USB HID |
| **SSH Hardware Keys** | Working | Native SSH via ed25519-sk (OpenSSH 8.2+) |
| **U2F** | Working | Legacy two-factor authentication |
| **TOTP Authenticator** | Working | Time-based OTP (100 accounts, Google Authenticator compatible) |
| **Password Vault** | Working | Secure password storage (369 entries) |
| **GPG/CCID** | Working (UI WIP) | OpenPGP smartcard via USB CCID, sign / encrypt / decrypt / SSH end-to-end with GnuPG |
| **BLE vCard** | ⚠️ **WIP, untested on hardware.** | Badge-to-badge contact exchange via BLE |
| **BLE HID** | Working | Bluetooth keyboard for auto-type |
| **WiFi + NTP** | Working | Time synchronization over WiFi, serial control (scan, connect, status, ..) |
| **BLE Serial** | ⚠️ **WIP, untested on hardware.** | Bluetooth serial console (Nordic UART Service) |
| **SAO Detection** | Working | Shitty Add-On port detection and info |
| **E-Paper Display** | Working | 2.9" low-power display with backlight |
| **12-Button Keypad** | Working | Phone-style T9 input |
| **Multi-Language** | Working | English and German UI |
| **Secure Serial** | Working | PIN authentication for serial commands |
| **WASM Plugin Runtime** | Working | Sandboxed third-party plugins via WebAssembly (WAMR). The host API under module `"cdc"` covers NVS, vFAT, i18n, HTTP/WiFi, GPIO/ADC/I2C, Pixel-Strip, BLE, SecureElement, Crypto, and UI views including a Canvas for plugin-drawn UIs. |
| **vFAT File Browser** | Working | On-device file explorer (Tools&nbsp;→&nbsp;Expert) and a `VFAT` serial shell for the plugins FAT partition. |

### Planned

- [ ] Certificate Authority (CA) module

## Plugins

Third-party functionality (Home Assistant controller, RSS reader, Grove LED
driver, etc.) ships as sandboxed WebAssembly plugins instead of being
compiled into the firmware. Plugins live in a separate 2&nbsp;MB FAT-FS
partition and can be installed / updated without re-flashing.

| Resource | Where |
|----------|-------|
| Web installer | [krim404.github.io/cdc-badge-plugins](https://krim404.github.io/cdc-badge-plugins/) |
| SDK + examples + source | [github.com/krim404/cdc-badge-plugins](https://github.com/krim404/cdc-badge-plugins) |

The plugin host API surface (NVS, sandboxed vFAT file access, i18n, HTTP, WiFi,
GPIO, ADC, I2C, Pixel-Strip, BLE, SecureElement, Crypto, UI views, Canvas, RGB
Color Picker, …) is defined canonically in
[`components/plugin_manager/include/plugin_manager/host_api.h`](components/plugin_manager/include/plugin_manager/host_api.h)
and mirrored byte-identical into the SDK repo.

## Architecture

The firmware uses a modular plugin architecture:

```
components/
  cdc_core/       Core services (EventBus, ServiceRegistry, ModuleRegistry)
  cdc_hal/        Hardware abstraction (Display, Keypad, Power, SecureElement)
  cdc_ui/         UI framework (ViewStack, I18n)
  cdc_views/      Reusable views (ListView, T9Input, PinEntry, etc.)
  cdc_os_ui/      OS-level UI (LockScreen, Settings, Sleep)
  usb_badge/      USB CDC/HID composite device
  serial_cmd/     Serial command interface

  mod_fido2/      FIDO2/WebAuthn/U2F module
  mod_2fa/        2FA authenticator module (TOTP)
  mod_password/   Password vault module
  mod_gpg/        OpenPGP smartcard (CCID) module
  mod_vcard/      BLE vCard exchange (Badge2Badge)
  mod_blehid/     BLE HID keyboard for auto-type
  mod_usbhid/     USB HID keyboard for auto-type
  mod_otphid/     Yubico OTP challenge-response over USB HID
  mod_ble_serial/ BLE Serial console (Nordic UART Service)
  mod_sao/        SAO port detection
  mod_nvsedit/    NVS editor (privileged)
  mod_vfat/       vFAT file explorer + VFAT serial shell
```

Modules are self-contained and can be enabled/disabled in `main/CMakeLists.txt`.

See [Module Development Guide](docs/MODULE_DEVELOPMENT.md) for creating new modules.

## Security Architecture

| Feature | Implementation |
|---------|----------------|
| **Key Storage** | All private keys in TROPIC01 secure element |
| **Key Generation** | P-256 and Ed25519 generated on-chip, never exported |
| **PIN Protection** | 4-8 digit PIN with 3 attempt lockout |
| **FIDO2 ClientPIN** | Protocol 2 with HKDF-SHA256 |
| **Attestation** | Self-signed (device-unique AAGUID) |

### PIN Lockout

The device uses a multi-PIN system with brute-force protection:

| PIN | Purpose | Max Retries | Recovery |
|-----|---------|-------------|----------|
| Badge PIN | Device unlock, serial auth | 3 | 60 seconds (timer) |
| PW1 | OpenPGP user operations | 3 | None (smartcard semantics) |
| PW3 | OpenPGP admin operations | 3 | None (smartcard semantics) |

The Badge PIN retry counter lives only in RAM; R-Memory persists a single
locked flag. A boot grants one attempt (zero if locked) and starts the 60-second
recovery timer that restores the counter to MAX_RETRIES on expiry. A crash
mid-verify cannot brick the device.

PW1 and PW3 follow OpenPGP smartcard rules: the counter is decremented and
persisted *before* the verify, and reaching zero is terminal until an admin
reset.

### TROPIC01 Secure Element

- 32 ECC key slots (P-256 and Ed25519)
- 512 R-Memory slots (422 bytes payload each)
- Hardware random number generator
- Tamper-resistant key storage
- Keys cannot be extracted or cloned

See [Module Development Guide](docs/MODULE_DEVELOPMENT.md) for the storage map
(canonical source: `main/tropic_slot_map.h`).

### Anti-Block Instant Lock

Holding **N + Y together** at any time forces the badge back to a clean,
locked state without a hardware reset: it unloads every running plugin,
closes all open views and dialogs, notifies modules to reset their session
state, and returns to the lock screen. Use it to recover if a view ever gets
stuck with no way back.

## Hardware

| Component | Model |
|-----------|-------|
| MCU | ESP32-S3-WROOM-1 (16MB Flash, PSRAM) |
| Display | GDEY029T94-FL03 (2.9" E-Paper + Frontlight) |
| Secure Element | TROPIC01 |
| I/O Expander | TCA9535 (Keypad) |
| Power IC | BQ25895 (LiPo Charger) |

Schematics and PCB: https://github.com/riatlabs/cdc-badge

## Getting Started

### Flash Pre-built Firmware

No build environment needed. Flash a release directly to the badge.

**Option A: Web Flasher (easiest)**

Use the browser-based flasher at [CDC Badge Web Flasher](https://krim404.github.io/cdc-badge-os/) - requires Chrome/Edge with Web Serial support.

**Option B: Python Flash Tool**

```bash
# Install dependencies
pip install -r tools/requirements.txt

# Flash the latest release from GitHub
python tools/flash_firmware.py --release latest

# Flash a specific version
python tools/flash_firmware.py --release latest

# Flash from a local directory
python tools/flash_firmware.py --dir ./artifacts/

# Specify port manually (auto-detected by default)
python tools/flash_firmware.py --release latest --port /dev/cu.usbmodem1101

# Erase all settings (NVS) after flashing
python tools/flash_firmware.py --release latest --erase-nvs
```

If the device is not detected, hold **BOOT** while pressing **RESET** to enter download mode.
Alternatively, when the badge is already running, authenticate over serial (`AUTH <pin>`) and issue `BOOTLOADER` to reboot into USB download mode.

### Build from Source

Requires [PlatformIO](https://platformio.org/) with ESP-IDF framework.

```bash
# Initialize submodules
git submodule update --init --recursive

# Build
~/.platformio/penv/bin/pio run

# Flash
~/.platformio/penv/bin/pio run -t upload

# Monitor (115200 baud)
~/.platformio/penv/bin/pio device monitor
```

### Compile-Time Flags

Feature flags in `components/cdc_core/include/cdc_core/feature_flags.h`:

| Flag | Default | Description |
|------|---------|-------------|
| `DEBUG_MODE` | 1 | Disables PIN lockouts and increases log verbosity. **Set to 0 for production!** |
| `FEATURE_SECURE_SERIAL` | 0 | Require PIN authentication for serial commands |
| `FEATURE_NVS_EDIT` | 0 | Enable NVS delete operations in the privileged NVS editor |

Set via build flags in `platformio.ini`:
```ini
build_flags =
    -DDEBUG_MODE=0
    -DFEATURE_SECURE_SERIAL=1
    -DFEATURE_NVS_EDIT=1
```

Or via Kconfig menuconfig:
```bash
~/.platformio/penv/bin/pio run -t menuconfig
```

### First-Time Setup

1. **Change the default PIN** (Settings -> Change PIN)
   - Default PIN: `123456`
   - FIDO2 requires a non-default PIN

2. **Set the time** via WiFi NTP or serial command:
   ```bash
   echo "SET_DATE $(date +%s)" > /dev/ttyACM0
   ```

3. **Register your first WebAuthn credential** at a supported site

### Using FIDO2/WebAuthn

1. Navigate to a WebAuthn-enabled site (GitHub, Google, etc.)
2. Badge displays the site name for confirmation
3. Press **Y** to approve, **N** to deny
4. Enter PIN on badge if required

### Using SSH Hardware Keys

```bash
# Generate Ed25519-SK resident key
ssh-keygen -t ed25519-sk -O resident -O application=ssh:myserver

# Export public keys from badge
ssh-keygen -K

# Connect (badge prompts for confirmation)
ssh user@server
```

### Using 2FA (TOTP/HOTP)

1. Add accounts via serial command or on-device
2. View codes in the 2FA menu
3. The badge types the code via the active HID keyboard (BLE keyboard, or USB keyboard when enabled)

### USB Interface Slots

The badge enumerates as a USB composite device. The serial console (CDC) is
always active. The ESP32-S3 USB controller provides 5 IN endpoints including
EP0; CDC occupies two of them, so **at most two additional USB interfaces can
be active at the same time**.

Modules that require a USB interface slot:

| Module | USB interface | Default |
|--------|---------------|---------|
| `mod_fido2` | FIDO2/U2F HID | enabled |
| `mod_gpg` | OpenPGP smartcard (CCID) | enabled |
| `mod_usbhid` | USB keyboard (auto-type for 2FA/passwords) | disabled |
| `mod_otphid` | Yubico OTP challenge-response over HID (KeePassXC) | disabled |

All other modules, including `mod_blehid` (Bluetooth keyboard), use no USB
interface slot.

With the default set (FIDO2 + GPG) the USB budget is already full. To enable
`mod_usbhid` (USB auto-type) or `mod_otphid` (challenge-response), first
disable one of the active USB modules in Tools -> Expert -> Modules or via
serial (`MODULE DISABLE <name>`, `MODULE ENABLE <name>`); otherwise the
toggle fails with "No free USB slot". `mod_usbhid` and `mod_otphid` also
share the same interface slot and cannot run simultaneously. Toggling USB
modules re-enumerates the device; replug the USB cable if the host does not
pick up the new configuration.

## Serial Commands

Connect at 115200 baud via USB CDC. Type `HELP` to list all available commands.

The [Web Flasher](https://krim404.github.io/cdc-badge-os/) also provides a serial console for configuration.

> **Tip — `PASTE`:** when a T9 input is open on the badge (URL, API key, entity id, …), `PASTE <text>` over serial injects the text into the active input. Saves a lot of multi-tap typing for long tokens.

See [Serial Commands Reference](docs/SERIAL_COMMANDS.md) for the full command list.

## Power Management

| Mode | Trigger | Wake |
|------|---------|------|
| Active | Normal use | - |
| Light Sleep | Lock screen idle | Any key except [3] (Menu) |
| Deep Sleep | Hold N 5s on lock | Any key (reset) |
| Shipping | Press FLASH / PW OFF (back) | Hold PW ON 2s |

> **Shipping mode** disconnects the battery. Wake it by holding **PW ON** for 2s.

## License

GNU General Public License v3.0 - see [LICENSE.md](LICENSE.md)

---

## Disclaimer

*Co-developed with [Claude Code](https://claude.ai/code) by Anthropic.*

This repository is a **proof-of-concept / demonstrator**. It may contain **serious bugs**, incomplete edge-case handling, and other "sharp edges". Do **not** use it as-is for production or security-critical deployments.

While I'm experienced with cryptography and encryption concepts, this is my first project implemented directly on the ESP32. For ESP-IDF/embedded best practices I relied heavily on external guidance and reviews. As a result, you may still find non-idiomatic ESP32 code, suboptimal design patterns, duplication, or refactoring debt.

The intent is to clean this up before the first major release (v1.0.0), once I have more routine in ESP32 development and can consolidate patterns, structure, and implementation details specific for this device.
