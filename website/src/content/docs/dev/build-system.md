---
title: Build and flash
description: Build CDC Badge OS with PlatformIO and ESP-IDF, understand the MODULES-list code generation, the flash partition layout, and the internal-RAM-versus-PSRAM memory model.
sidebar:
  order: 2
---

CDC Badge OS builds with PlatformIO on top of the ESP-IDF framework. The single build environment is `cdc_badge_usb` (`platformio.ini`).

## Prerequisites

The build pulls in git submodules (the WAMR runtime and other vendored components). Initialize them first:

```bash
git submodule update --init --recursive
```

The `pio` binary is not on `PATH` in the standard setup; use its full path under the PlatformIO virtualenv.

## Build and flash commands

```bash
# Build firmware
~/.platformio/penv/bin/pio run

# Build and flash
~/.platformio/penv/bin/pio run -t upload

# Monitor serial output (115200 baud)
~/.platformio/penv/bin/pio device monitor
```

The serial monitor speed is 115200 baud, configured as `monitor_speed` in `platformio.ini`.

:::caution[Flash sparingly]
Each flash cycle stresses the chip and is time-consuming. Review changes carefully before flashing instead of relying on trial and error.
:::

### Whether a manual reset is needed after upload

Whether the chip needs a manual reset after `pio run -t upload` depends on how download mode was entered:

- Via the serial `BOOTLOADER` command (send `AUTH <pin>` then `BOOTLOADER` over the CDC port): no reset needed. After each esptool operation the chip auto-reboots into the new firmware and re-enumerates as `BadgeV1`.
- Via manual flash mode (hold BOOT and press RESET): the chip stays in the ROM bootloader after upload, so you must press RESET manually to boot the firmware.

Wait a few seconds for re-enumeration as `BadgeV1` before running host-side checks.

## Environment configuration

Key settings from `platformio.ini`:

| Setting | Value |
| --- | --- |
| `platform` | `espressif32` |
| `framework` | `espidf` |
| `board` | `cdc-badge-usb` |
| `board_build.flash_size` | 16 MB |
| `board_build.psram` | enabled |
| `board_build.partitions` | `partitions.csv` |
| `board_build.sdkconfig_defaults` | `sdkconfig.defaults` |
| C++ standard | `-std=gnu++17` (c++2b/c++23 explicitly unflagged) |

Three pre-build scripts run before compilation: a Python dependency installer, a submodule fetcher, and a component manager (`tools/pio_python_deps.py`, `tools/pio_submodules.py`, `tools/pio_component_manager.py`).

## The MODULES list and generated registration

Feature modules are added in exactly one place: the `MODULES` list in `main/CMakeLists.txt`.

```cmake
set(MODULES
    mod_2fa
    mod_fido2
    mod_password
    mod_gpg
    mod_sao
    mod_vcard
    mod_ble_serial
    mod_nvsedit
    mod_blehid
    mod_usbhid
    mod_otphid
    mod_vfat
)
```

That list does two things during the CMake configure step:

1. Each name is added to the `main` component's `REQUIRES`, so the module component is linked in.
2. CMake generates `modules_init.gen.h` in the build directory. For every entry it emits an `extern "C" void <name>_register();` declaration and a call inside a generated `modules_register_all()` function.

`main.cpp` includes the generated header and calls `modules_register_all()` at boot, followed by `ModuleRegistry::instance().runAllInitializers()`, then rebuilds the UI menus with `ui_on_modules_ready()`. There are no manual includes per module.

### Adding a module

1. Create the component under `components/mod_<name>/` with its own `CMakeLists.txt`.
2. Export `extern "C" void mod_<name>_register(void);`.
3. Add `mod_<name>` to the MODULES list in `main/CMakeLists.txt`.

Deleting a module's folder and removing its line from the MODULES list is enough to remove it cleanly, because the core never references modules directly. See [Architecture](/dev/architecture/) for the isolation rules and the `IModule` lifecycle.

## Flash and partition layout

The flash layout comes from `partitions.csv`. On the ESP32-S3 the bootloader lives at offset `0x0` and the partition table at `0x8000`; the partitions below start after that.

| Name | Type | SubType | Offset | Size |
| --- | --- | --- | --- | --- |
| `nvs` | data | nvs | `0x9000` | `0x47000` |
| `app0` | app | factory | `0x50000` | `0xDA0000` (about 13.6 MB) |
| `plugins` | data | fat | `0xDF0000` | `0x200000` (2 MB) |
| `coredump` | data | coredump | `0xFF0000` | `0x10000` (64 KB) |

The `plugins` FAT partition is mounted at `/vfat`. Installed WASM plugins and the runtime i18n overlay files live under the `system/` subfolder (`/vfat/system/<id>.wasm`, `/vfat/system/i18n/lang_<code>.json`); the partition root is the user file area. The `coredump` partition stores crash dumps for offline analysis.

## Memory model

Internal SRAM on this build is the bottleneck; PSRAM is the default allocation pool. PSRAM is configured as octal mode at 80 MHz, with BSS allowed in external memory (`sdkconfig.defaults`):

```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
```

Guidelines for new code:

- Reserve internal RAM for task stacks, ISR-touched data, BLE/WiFi-internal buffers, and small static state. Everything else goes to PSRAM.
- Large static buffers use the `EXT_RAM_BSS_ATTR` attribute so they land in PSRAM rather than internal BSS.
- Runtime allocations larger than a few kilobytes use PSRAM. `cdc::core::psramAlloc<T>(count)` (in `cdc_core/Raii.h`) wraps `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` and returns a `PsramUniquePtr<T>` that frees through the caps allocator on destruction.
