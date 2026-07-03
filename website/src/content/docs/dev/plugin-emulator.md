---
title: Plugin emulator (off-device)
description: Run and regression-test WASM plugins on a desktop against the real firmware drawing stack and host API - no badge hardware needed.
sidebar:
  order: 6.5
---

The **cdc-badge-development** repository ships an off-device emulator that
runs a plugin's `.wasm` on Linux, macOS or Windows against the **same WAMR
runtime and the same 225-symbol `"cdc"` host API** this firmware exposes. It
compiles the firmware's drawing stack (`Adafruit_GFX`/CalEPD/`Gdey029T94`),
view stack and plugin-manager host-API bodies read-only on the host, so the
rendered frames are **pixel-identical** to the e-paper. The architecture is
recorded in [ADR-0013](./adr/0013-offdevice-emulator-rehosting/).

It emulates the plugin and the host-API boundary only - never the SoC,
firmware boot or the badge state machine.

## Getting it

The emulator lives in
[cdc-badge-development](https://github.com/krim404/cdc-badge-development)
under `emulator/`:

```sh
git clone --recursive https://github.com/krim404/cdc-badge-development
cd cdc-badge-development
cmake -S emulator -B emulator/build && cmake --build emulator/build
python3 tools/badge.py emulate canvas_demo
```

Self-contained binaries (Linux AppImage, Windows `.exe`, unsigned macOS
binary) are published by that repo's `release-emulator` workflow.

## Using it

`badge emulate <plugin>` opens a window showing the badge screen; the keyboard
maps to the 12-key keypad (`0`-`9`, arrows = 2/4/6/8, `Y`/Enter, `N`/Esc, hold
for long press). The launching terminal doubles as an interactive console -
the off-device serial-in - with `help`, `paste` (into an open T9 editor),
`key`, `event`, `cmd` and `screenshot` commands.

Headless mode drives everything scripted and deterministically, which is what
plugin CI uses for frame-hash regression tests:

```sh
badge emulate my_plugin --headless --keys 1,2,Y --frames out/
badge emulate my_plugin --headless --keys 1,2,Y --snapshot tests/snapshots/my_plugin
```

## Scope

| Works for real | Fails cleanly (stubbed) |
|---|---|
| Canvas + full UI view stack, keypad, virtual clock/ticks | Bluetooth |
| NVS, vFAT sandbox, R-Memory (firmware `RMemHeader` layout) | GPIO/PWM/ADC/I2C/SAO |
| Secure-element crypto (real P-256/Ed25519 signatures, software keystore) | Pixel strip |
| HTTP/socket over the host's real internet; WiFi reports always-on | USB CDC |
| Manifest prerequisites, i18n, `plugin_on_cmd`, EventBus | Badge-to-badge `msg` |

Residency semantics (`background`, `autoload`, `prevent_sleep`) are not
emulated; the plugin runs in the foreground. The software secure element is
**dev-only** (plaintext files, no irreversible TROPIC01 operations - enforced
by a CI check).

Full option reference and details:
[`emulator/README.md`](https://github.com/krim404/cdc-badge-development/blob/main/emulator/README.md)
in the development repo.
