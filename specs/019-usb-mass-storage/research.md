# Phase 0 Research: USB Mass Storage (vFAT file transfer)

All product-level ambiguities were resolved during `/speckit-clarify` (see spec
Clarifications). This document records the **technical** decisions for the implementation,
each grounded in the current code.

## R1 — MSC backing: hand-rolled `tud_msc_*` over the wl block device

**Decision**: Implement the TinyUSB MSC class callbacks by hand in `usb_badge`, backed by
the wear-leveling block device of the `vfat` partition (`wl_read` / `wl_write` /
`wl_size` / `wl_sector_size` on the `wl_handle_t` owned by `PluginStorage`). Do **not**
adopt ESP-IDF's `esp_tinyusb` `tinyusb_msc_storage_*` helper.

**Rationale**: The firmware does not use `esp_tinyusb`'s driver manager; it owns its USB
init (`usb_cdc.cpp` → `tusb_init()`) and builds the configuration descriptor itself
(`usb_hid.cpp::build_config_descriptor`). `tinyusb_msc_storage_init_spiflash()` assumes the
ESP managed USB driver and its own mount lifecycle, which would conflict with the existing
hand-rolled stack and the dynamic re-enumeration path. Hand-rolling the few `tud_msc_*`
callbacks over `wl_*` is the smaller, consistent change.

**Callbacks to implement** (`managed_components/espressif__tinyusb/src/class/msc/msc_device.h`):
`tud_msc_inquiry_cb`, `tud_msc_test_unit_ready_cb`, `tud_msc_capacity_cb`,
`tud_msc_read10_cb`, `tud_msc_write10_cb`, `tud_msc_start_stop_cb`, `tud_msc_is_writable_cb`,
`tud_msc_scsi_cb`. They live in shared infra (`usb_badge`) so the module stays removable.

**Alternatives considered**: `esp_tinyusb` MSC helper (rejected: conflicts with the
hand-rolled stack and its mount model); a custom synthesized/virtual FAT exposing only user
files (rejected: large custom SCSI-to-FS shim, contradicts the "expose entire volume"
clarification and Simplicity).

## R2 — Block geometry: expose the wl logical space at its native sector size

**Decision**: MSC LUN reports `block_count = wl_size()/sector` and `block_size =
wl_sector_size()` (4096 B). `read10`/`write10` map `lba`+`offset` directly onto
`wl_read`/`wl_write` against the wl logical address space (the same space FatFs sits on).
Every callback validates that `lba*block_size + offset + bufsize <= wl_size()` and returns
`TUD_MSC_RET_ERROR` on out-of-range (see contracts/msc-block.md).

**Rationale**: The volume was formatted with `--sector_size 4096` (`tools/build_lang_image.py`)
and mounted with `allocation_unit_size = CONFIG_WL_SECTOR_SIZE` (4096). Exposing the wl
logical space at 4096 B keeps the host's FAT view identical to the badge's, with no
translation layer. The wear-leveling layer continues to handle physical wear underneath.

**Alternatives considered**: Reporting 512 B blocks with 8:1 translation (rejected:
needless complexity and read-modify-write hazards on a 4096 B WL sector).

## R3 — Dynamic interface integration + endpoint budget

**Decision**: Extend the existing dynamic descriptor builder rather than adding a static
descriptor. Add a `bool mscActive_` to `UsbManager` with
`registerMassStorage()/unregisterMassStorage()`; `applyConfiguration()` passes the MSC flag
to `usb_hid_apply_config()`, which (a) emits `TUD_MSC_DESCRIPTOR` with the next free
interface number and one bulk IN + one bulk OUT endpoint and (b) triggers the existing
`tud_disconnect()` → delay → `tud_connect()` re-enumeration. Set `CFG_TUD_MSC=1` and define
`CFG_TUD_MSC_EP_BUFSIZE` in `tusb_config.h`.

**Endpoint budget**: `CFG_TUD_ENDPOINT_MAX=8`; CDC fixes 3 (0x81/0x02/0x82). The allocator
in `usb_hid.cpp` already assigns `next_in=0x83`, `next_out=0x03` sequentially for HID/CCID.
MSC adds 2. The combination CDC(3) + 2×HID + CCID(2) + MSC(2) = 9 exceeds the budget.
Therefore MSC participates in budget accounting: if registering MSC (or another service
while MSC is active) would exceed `CFG_TUD_ENDPOINT_MAX` or the ESP32-S3 hardware endpoint
limit, the registration fails and the module's `start()` returns `false`, surfacing as
`[FAIL]` in the Expert → Modules list (the existing `classifyStartFailure` /
`UsbBudgetFull` path). No crash, no silent truncation.

**Rationale**: Mirrors the existing HID-slot budgeting (`MAX_ACTIVE_HID`) and the existing
re-enumeration path; nothing new architecturally. Honestly bounds concurrency at the
hardware limit instead of pretending all services coexist.

**Alternatives considered**: Raising `CFG_TUD_ENDPOINT_MAX` (rejected: bounded by ESP32-S3
USB-OTG hardware endpoints/FIFO, not a free knob); making MSC strictly mutually exclusive
with all HID/CCID (rejected as too coarse — budget accounting allows MSC + CDC + one HID,
which fits, and only rejects the genuinely-over-budget combinations).

## R4 — Host/badge concurrency: keep mounted, gate writes, remount on disconnect

**Decision** (implements the spec clarification "the badge does not lock itself out"):
- The badge keeps its `/vfat` FatFs **mounted** while a host is connected and serves
  **reads** (Files browser, image/Markdown viewers, plugin reads).
- A global **host-active gate** in `PluginStorage` (set when the host is connected to the
  MSC LUN, i.e. between `tud_msc_start_stop_cb` load and eject / USB unmount) makes all
  badge-side **write** paths refuse or defer: plugin upload, `VFAT PUT/RECEIVE`, lang-overlay
  writes, and the boot-time attribute set (R5). FR-007: the host is the sole writer.
- On host disconnect/eject, `PluginStorage` **remounts** `/vfat` (unmount + mount) so the
  badge's FatFs cache is refreshed and host-written files appear in the Files menu (FR-006).

**Rationale**: With zero badge-side writes, the host cannot corrupt and the badge cannot
flush a stale window over the host's changes; FatFs read-only paths never call `disk_write`.
A read taken mid host-write may return a transiently stale window (display glitch, accepted
per clarification), never corruption. The remount-on-disconnect closes the staleness window
so the Files view reflects the transfer.

**Residual / honesty note**: While the host is mid-write, the badge's cached FAT window can
be stale, so the Files view may briefly show an inconsistent listing. This is the accepted
glitch, not data loss.

**Alternatives considered**: Full unmount handoff (badge relinquishes the volume entirely
while the host has it — the standard `esp_tinyusb` pattern). Rejected by the clarification
("don't lock the badge out"); recorded here as the simpler-but-rejected option.

## R5 — Advisory `system` protection via FatFs attributes

**Decision**: Mark `/vfat/system` (and, best-effort, its direct entries) read-only +
hidden + system using FatFs attributes. Enable `FF_USE_CHMOD` in sdkconfig and call
`f_chmod` against the FatFs volume path for the `system` directory once at mount (before any
host can connect), guarded so it is skipped while the host-active gate is set. The set is
idempotent.

**Rationale**: `tools/build_lang_image.py` uses `wl_fatfsgen.py`, which cannot set FAT
attributes at image-build time, and ESP-IDF's FATFS VFS exposes no POSIX `chmod`. Setting
`AM_RDO|AM_HID|AM_SYS` at runtime via FatFs is the only available mechanism. This makes
standard host file managers hide `system` and refuse to modify it (advisory only, per the
spec clarification); a host can still clear attributes or write raw blocks, and re-flashing
restores system content.

**Open implementation detail (non-blocking)**: obtaining the FatFs drive path that
`esp_vfs_fat` assigned for `f_chmod`. If `FF_USE_CHMOD` or the drive-path lookup proves
impractical, fall back to marking only the top-level `system` directory; if even that is
infeasible, the protection degrades to "visible but documented as do-not-touch" and the
spec's advisory wording still holds. Decide during implementation; does not change scope.

**Alternatives considered**: Setting attributes in the prebuilt image (rejected: tool
support absent); device-enforced block write-protection of the system region (already
rejected in the spec, option B).

## R6 — Partition label rename `plugins` → `vfat` (in flight)

**Decision**: Treat the rename as a prerequisite handled alongside this work, not by this
feature's logic. `partitions.csv` line `plugins,data,fat,0xDF0000,0x200000` becomes label
`vfat`; `PluginStorage::PARTITION_LABEL` changes `"plugins"` → `"vfat"`. Mount point
(`/vfat`), `SYSTEM_DIR` (`/vfat/system`), and `OVERLAY_DIR` (`/vfat/system/i18n`) are
already on the `vfat` naming. No migration code (pre-1.0): users re-flash.

**Rationale**: `legacy` `plugins` is being retired; the MSC plan references the volume by
its target label so the docs and code stay consistent. The MSC backing reads the
`wl_handle_t` regardless of label, so this is purely a naming alignment.

## Resolved unknowns summary

| # | Topic | Decision |
|---|-------|----------|
| R1 | MSC backing | Hand-rolled `tud_msc_*` over `wl_*`, in shared `usb_badge` infra |
| R2 | Block geometry | Expose wl logical space at 4096 B sector; bounds-check every op |
| R3 | Interface + EP budget | Dynamic descriptor + re-enum; MSC in budget accounting, over-budget → `[FAIL]` |
| R4 | Concurrency | Keep mounted, gate badge writes while host-active, remount on disconnect |
| R5 | `system` protection | Runtime FatFs `f_chmod` (RDO+HID+SYS), `FF_USE_CHMOD`, advisory |
| R6 | Partition rename | Align to `vfat` label; no migration |

No `NEEDS CLARIFICATION` remain.
