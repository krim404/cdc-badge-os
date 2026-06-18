# Implementation Plan: USB Mass Storage (vFAT file transfer)

**Branch**: `message-transfer-framework` | **Date**: 2026-06-18 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/019-usb-mass-storage/spec.md`

## Summary

Add an optional, default-disabled module `mod_msc` that exposes the badge's vFAT volume
(`vfat` partition, mounted at `/vfat`) to a connected host as a USB Mass Storage (MSC)
removable drive, so users can drag-and-drop files instead of using the serial tools.

Technical approach: extend the existing hand-rolled TinyUSB integration in `usb_badge`
(which already builds the configuration descriptor dynamically and re-enumerates via
`tud_disconnect()/tud_connect()`) with a generic, dormant MSC class backed by the
wear-leveling block device of the `vfat` partition. `mod_msc` follows the exact
`mod_usbhid` / `mod_otphid` pattern: registered in the MODULES list, default-off in
`module_defaults.h`, toggled at runtime from the Expert → Modules list, and surfaced with
an i18n status view. When enabled it registers the MSC LUN with `UsbManager`, which adds
the MSC interface to the descriptor and re-enumerates; disabling unregisters and
re-enumerates the drive away. While a host has the volume, the badge does not write to it
(a global host-active gate refuses badge-side writes) but keeps read access; on host
disconnect the badge remounts so newly written files appear in the Files menu.

## Technical Context

**Language/Version**: C++17 (firmware), Python 3 (image build tooling)

**Primary Dependencies**: ESP-IDF (PlatformIO env `cdc_badge_usb`), TinyUSB
(`managed_components/espressif__tinyusb`, MSC class driver present but `CFG_TUD_MSC=0`),
FatFs + wear-leveling (`esp_vfs_fat_spiflash_mount_rw_wl`, `wl_*`), existing `usb_badge`
USB stack and `cdc_core::UsbManager`.

**Storage**: `vfat` FAT partition, 2 MiB at offset `0xDF0000`, WL sector size 4096 B,
mounted at `/vfat`; system content under `/vfat/system` (plugins, i18n overlays).

**Testing**: Host unit tests (`pio test -e native`) for the LBA/bounds mapping and the
host-active write gate; on-device manual verification per `quickstart.md` (flash
conservation: no trial-and-error).

**Target Platform**: ESP32-S3 (16 MB flash, Octal PSRAM), USB-OTG FS device.

**Project Type**: Embedded firmware, self-contained module extending shared USB/storage
infrastructure.

**Performance Goals**: Drive enumerates on the host within 5 s of enable+connect (SC-001);
copy-and-appear in Files under 1 min (SC-002); byte-identical transfers (SC-005).

**Constraints**:
- **USB endpoint budget** is the hard limit: `CFG_TUD_ENDPOINT_MAX = 8`; CDC always uses 3
  (0x81 notif, 0x02 out, 0x82 in). MSC needs 1 bulk IN + 1 bulk OUT. With CCID (2) + two
  HID interfaces (2) already possible, MSC + everything exceeds the budget. MSC must be
  accounted for in the endpoint allocator and a combination that does not fit must be
  rejected cleanly (existing `[FAIL]` path in the Modules list), not crash.
- No secrets are on this volume (keys/FIDO/passwords/GPG live in TROPIC01/NVS); the MSC
  surface therefore exposes only user files, plugin `.wasm`, and i18n overlays.
- No concurrent badge writes while a host has the volume (corruption avoidance, FR-007).
- Internal RAM is scarce: the single MSC sector transfer buffer (4096 B) is the only
  notable buffer; keep it static/PSRAM, no per-transfer allocation.

**Scale/Scope**: One new module (~3 files) + bounded edits to `usb_badge`, `cdc_core`
(`UsbManager`), `plugin_manager` (`PluginStorage` block accessor + attribute set),
`tusb_config.h`, `module_defaults.h`, `main/CMakeLists.txt`, sdkconfig (FatFs chmod),
i18n tables, and the documentation website.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Assessment |
|-----------|------------|
| **I. Module Isolation** | PASS. `mod_msc` is removable and self-contained for everything user-facing (lifecycle, settings/menu, i18n, status, enable-gate, LUN registration). The MSC *class plumbing* (descriptor entry, `tud_msc_*` callbacks, wl block backing) lives in shared USB/storage infra (`usb_badge`, `cdc_core::UsbManager`, `PluginStorage`), mirroring how CDC/HID/CCID class plumbing already lives there and modules only *register* interfaces. The MSC infra is **dormant** with no registrant: deleting `components/mod_msc/` leaves it compiling (callbacks present, no LUN registered → MSC interface never added to the descriptor → host never sees a drive). No core→module reference is introduced. |
| **II. PSRAM-First** | PASS with note. Only new buffer is one MSC sector transfer buffer (4096 B). It is USB-task-touched; allocate it once (static or PSRAM), never per transfer. No vectors/maps in core APIs. |
| **III. Security (NON-NEGOTIABLE)** | PASS. Default-OFF (FR-002) preserves the "no storage exposed unless opted in" posture. No secret leaves the secure element (none are on this volume). Plugin sandbox unchanged: a host altering a `.wasm` cannot escape the WAMR sandbox; plugins are still validated/sandboxed at load. No GPIO hard-block or capability changes. The host-active write gate must be correct to avoid FS corruption; advisory `system` attributes are best-effort only (documented). Reviewed against `website/src/content/docs/security/`. |
| **IV. Simplicity & Surgical Change** | PASS. Reuses the existing dynamic-descriptor + register/unregister + Modules-toggle pattern; no new partition; no speculative config. |
| **V. Versioning & Pre-1.0** | PASS. No version bumps (firmware, `HOST_API_LEVEL_*`, schema) — none are touched and none will be bumped without instruction. `host_api.h` is unchanged (MSC is not a plugin API). The `plugins`→`vfat` partition-label rename is data-breaking but pre-1.0 allowed: no migration code; users re-flash. |

No violations → Complexity Tracking is empty.

## Project Structure

### Documentation (this feature)

```text
specs/019-usb-mass-storage/
├── plan.md              # This file
├── spec.md              # Feature spec (with Clarifications)
├── research.md          # Phase 0 output (decisions + rationale)
├── data-model.md        # Phase 1 output (entities/state)
├── quickstart.md        # Phase 1 output (validation guide)
├── contracts/           # Phase 1 output (MSC block + UsbManager + module contracts)
│   ├── msc-block.md
│   └── usbmanager-massstorage.md
├── checklists/
│   └── requirements.md  # Spec quality checklist (from /speckit-specify)
└── tasks.md             # Phase 2 output (/speckit-tasks - NOT created here)
```

### Source Code (repository root)

New module (self-contained):

```text
components/mod_msc/
├── CMakeLists.txt                         # REQUIRES cdc_core cdc_ui cdc_views cdc_hal cdc_log usb_badge plugin_manager nvs_flash freertos
├── include/mod_msc/
│   └── UsbMscModule.h                     # class UsbMscModule : core::IModule (singleton)
└── src/
    └── UsbMscModule.cpp                   # lifecycle, i18n table, status/menu view, mod_msc_register()
```

Shared infrastructure edited (generic MSC capability + block backing):

```text
include/tusb_config.h                      # CFG_TUD_MSC=1, CFG_TUD_MSC_EP_BUFSIZE
components/usb_badge/usb_hid.cpp           # build_config_descriptor(): emit TUD_MSC_DESCRIPTOR when MSC active; extend EP/itf allocator
components/usb_badge/usb_descriptors.h     # MSC string index / interface count constants
components/usb_badge/                      # new: tud_msc_*_cb implementations backed by the vfat wl block device (dormant w/o registrant)
components/cdc_core/include/cdc_core/UsbManager.h  # registerMassStorage()/unregisterMassStorage(); MSC in endpoint-budget accounting
components/cdc_core/src/UsbManager.cpp     # MSC active flag → applyConfiguration() path
components/plugin_manager/src/PluginStorage.cpp     # expose block accessor (wl read/write/size/sector) + host-active write gate + setSystemAttributes()
components/plugin_manager/include/plugin_manager/PluginStorage.h
```

Configuration, build, tooling, docs:

```text
main/module_defaults.h                     # X("mod_msc", false)
main/CMakeLists.txt                        # add mod_msc to MODULES
sdkconfig.defaults (or board sdkconfig)    # FF_USE_CHMOD=y (FatFs attribute set)
components/cdc_ui/src/I18n.cpp             # any new core.* keys (if used by status view)
assets/i18n/lang_de.json                   # German for every new mod_msc.* / core.* key
partitions.csv + PluginStorage PARTITION_LABEL  # plugins → vfat (rename, in flight)
website/src/content/docs/...               # power storage-tools + USB services pages; dev serial/USB notes
```

**Structure Decision**: One new self-contained module `components/mod_msc/` plus surgical
extensions to the already-shared USB stack (`usb_badge`, `cdc_core::UsbManager`) and the
storage layer (`plugin_manager::PluginStorage`). This mirrors how HID/CCID are shared class
plumbing consumed by modules, keeping `mod_msc` removable while the MSC class support stays
dormant infrastructure when no module registers a LUN.

## Complexity Tracking

No constitution violations. Table intentionally empty.
