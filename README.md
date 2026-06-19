# CDC Badge OS

Modular firmware for the CDC Badge v1.0/v1.1 hardware security key, built around a
TROPIC01 secure element: FIDO2/WebAuthn, GPG/SSH, TOTP, a password vault, a
sandboxed WASM plugin runtime and Bluetooth LE (badge-to-badge message transfer,
vCard exchange, beacon and BLE keyboard/HID), on an ESP32-S3 with an e-paper
display and a 12-button keypad.

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

## Documentation

The full documentation (introduction, flashing, a feature-by-feature user guide,
the security background and automatic processes, plugin and power-user guides, and
the developer and protocol reference) lives on the documentation site:

### **https://krim404.github.io/cdc-badge-os/**

The site is generated from [`website/`](website/) with Astro Starlight and is
deployed to Codeberg, GitHub and GitLab Pages. Highlights:

- **Getting started** - what the badge is, flashing, first boot, the keypad and the user guide.
- **Security & background** - the secure element, automatic on-chip key generation, the attestation key, PIN and lockout behaviour, and the duress PIN.
- **Intermediate** - installing and managing WASM plugins, the serial console, the expert menu and the companion tools.
- **Developer** - architecture, the build system, module and plugin development, the host API, the protocol references and the generated code reference.

## Quick links

| | |
|--|--|
| Web flasher | <https://krim404.github.io/cdc-badge-os/flasher/> |
| Plugin SDK, examples and web installer | <https://github.com/krim404/cdc-badge-plugins> |
| Hardware (schematics and PCB) | <https://github.com/riatlabs/cdc-badge> |

## Build from source

Requires [PlatformIO](https://platformio.org/) with the ESP-IDF framework.

```bash
git submodule update --init --recursive
~/.platformio/penv/bin/pio run            # build
~/.platformio/penv/bin/pio run -t upload  # flash
~/.platformio/penv/bin/pio device monitor # serial monitor (115200 baud)
```

See the build guide on the documentation site for the partition layout, compile-time
flags and the module system.

## License

GNU General Public License v3.0 - see [LICENSE.md](LICENSE.md)

---

## Disclaimer

*Co-developed with [Claude Code](https://claude.ai/code) by Anthropic.*

This repository is a **proof-of-concept / demonstrator**. It may contain **serious bugs**, incomplete edge-case handling, and other "sharp edges". Do **not** use it as-is for production or security-critical deployments.

While I'm experienced with cryptography and encryption concepts, this is my first project implemented directly on the ESP32. For ESP-IDF/embedded best practices I relied heavily on external guidance and reviews. As a result, you may still find non-idiomatic ESP32 code, suboptimal design patterns, duplication, or refactoring debt.

The intent is to clean this up before the first major release (v1.0.0), once I have more routine in ESP32 development and can consolidate patterns, structure, and implementation details specific for this device.
