---
title: Expert menu
description: The Expert menu under Tools - hardware info, module enable/disable, encrypted backup, the duress PIN, secure-element cache maintenance, ship mode and bootloader entry.
sidebar:
  order: 6
---

The Expert menu groups the maintenance and diagnostic actions that are not part
of everyday use. Open it from **Main menu → Tools → Expert**. On entry the badge
shows a caution prompt that you have to acknowledge before the menu appears.

:::caution
These actions reach hardware and stored data directly. Rebooting into the
bootloader and rebuilding secure-element caches are safe but interrupt normal
operation; toggling modules and setting a duress PIN change how the badge
behaves. Read each entry below before using it.
:::

## Entries

The menu is built from a fixed top group, then any module-provided entries (for
example the **System Files** explorer), then a fixed bottom group. The fixed
entries are:

| Entry | What it does |
| --- | --- |
| **Hardware Info** | Opens a read-only screen that probes the I2C bus, power IC (BQ25895), keypad expander (TCA9535), display, TROPIC01 secure element (including its session state, RISC-V/SPECT firmware versions and R-Memory slot size), WiFi and BLE, and reports heap/DRAM memory usage. |
| **Modules** | Opens the module list. Each row shows a module name and its current status. Selecting a module toggles it enabled/disabled and starts or stops it; a module that failed to start shows an error and selecting it offers a retry. |
| **Backup** | Opens the encrypted backup submenu (Export / Import / Delete). See [Encrypted backup & restore](/guide/backup-restore/). |
| **Set Duress PIN** | Starts the duress-PIN setup flow. See [Duress PIN / self-destruct](/security/duress/). |
| **TR01 Cache Rebuild** | Rebuilds the cached TROPIC01 metadata and reports OK or failure. |
| **TR01 Cache Cleanup** | Cleans the cached TROPIC01 metadata and reports OK or failure. |
| **Shipping Mode** | Disconnects the battery for storage/shipping after a confirmation prompt. See [Ship mode](/guide/power-sleep/#ship-mode). |
| **Bootloader** | Reboots the badge into USB download mode for flashing. It shows a "bootloader mode" lock screen, detaches USB so the host re-enumerates, and triggers a hard reset with the ROM download-boot bit set. |

:::note[Module-provided entries]
Modules can add their own rows between the top and bottom groups. The **System
Files** explorer (the full file view, including the `system/` folder) appears
here when the `mod_vfat` module is enabled - see
[File storage & NVS tools](/power/storage-tools/).
:::

## Modules: enable and disable

The **Modules** screen lists every registered module with a status marker.
Selecting an entry:

- toggles the module's enabled state and immediately starts or stops it;
- if a module fails to start, shows the failure reason. A start can fail because
  a secure-element slot is in error, because there is no free USB interface
  slot, or for a generic reason;
- if the toggle changes the USB configuration (for example enabling a USB-HID
  module), shows a sticky alert that you need to re-plug the badge.

A module that is in an error state shows that error; selecting it offers to
retry initialization rather than toggling it.

The factory-default enabled state of each module is fixed at build time. Your
own enable/disable choices persist independently in NVS and survive reboots.

## TROPIC01 cache maintenance

The badge keeps a cache of metadata for the TROPIC01 secure element. Two Expert
actions maintain it:

- **TR01 Cache Rebuild** re-reads the secure element and rebuilds the cache.
- **TR01 Cache Cleanup** removes stale cache entries.

Both run synchronously, show a working indicator, and report OK or failure. Use
them if the badge reports inconsistent secure-element state.

## Ship mode

**Shipping Mode** disconnects the battery so the badge can be stored or shipped
without draining it. Selecting it shows a confirmation prompt first; on confirm
the charger opens its BATFET and the badge powers off (or keeps running from USB
if USB is attached). The same action is available by holding the Flash button
for three seconds or via the `SHIPMODE` serial command. Full details are on the
[Power, battery & sleep](/guide/power-sleep/#ship-mode) page.

## Bootloader entry

**Bootloader** is the on-device way to enter USB download mode without holding
the BOOT button. After you select it the badge switches to a quiet lock-screen
state, detaches USB, and performs a hard reset into the ROM download mode, so a
flashing tool on the host can talk to it. A normal power cycle returns the badge
to firmware.
