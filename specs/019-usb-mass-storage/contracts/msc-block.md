# Contract: MSC block device (TinyUSB `tud_msc_*` ↔ vfat wl device)

The host-facing contract is the standard USB Mass Storage Bulk-Only Transport / SCSI
transparent command set. The firmware implements it through TinyUSB's `tud_msc_*` callbacks
(shared infra in `usb_badge`), backed by the `vfat` wear-leveling block device. There is one
LUN (`lun == 0`); any other LUN is an error.

## Geometry

- `block_size = wl_sector_size()` → **4096 bytes**
- `block_count = wl_size() / block_size`
- Total bytes = `wl_size()` (the `vfat` partition payload, 2 MiB minus FS overhead)

## Callbacks

| Callback | Behaviour |
|----------|-----------|
| `tud_msc_inquiry_cb(lun, vendor[8], product[16], rev[4])` | Fill fixed ASCII identity (e.g. vendor `"CDCBadge"`, product `"vFAT Storage"`, rev `"1.0"`). |
| `tud_msc_test_unit_ready_cb(lun)` | Return `true` when the LUN is active and the block device is usable; `false` (with sense `NOT READY`) when not. |
| `tud_msc_capacity_cb(lun, &block_count, &block_size)` | Report the geometry above. |
| `tud_msc_is_writable_cb(lun)` | Return `true` (read-write, FR-005). |
| `tud_msc_read10_cb(lun, lba, offset, buffer, bufsize)` | Validate range (below); `wl_read(handle, lba*block_size + offset, buffer, bufsize)`; return `bufsize` on success, `TUD_MSC_RET_ERROR` on range/IO error. |
| `tud_msc_write10_cb(lun, lba, offset, buffer, bufsize)` | Validate range; `wl_write(handle, lba*block_size + offset, buffer, bufsize)`; return `bufsize` on success, `TUD_MSC_RET_ERROR` on range/IO error. Sets/keeps host-active gate. |
| `tud_msc_start_stop_cb(lun, power, start, load_eject)` | On load: mark host present, set the `PluginStorage` host-active gate. On eject: clear host present, clear gate, request remount of `/vfat`. Return `true`. |
| `tud_msc_scsi_cb(lun, scsi_cmd, buffer, bufsize)` | Handle or reject unsupported SCSI opcodes; return `-1` with `ILLEGAL REQUEST` sense for unknown commands. |

## Invariants / validation

- `lun == 0` required; otherwise return error.
- Range check on every `read10`/`write10`:
  `lba * block_size + offset + bufsize <= wl_size()`. Out of range → `TUD_MSC_RET_ERROR`
  (never read/write past the partition).
- `write10` is the **only** writer to the volume while a host is connected; the badge's own
  write paths are gated off (see usbmanager-massstorage.md and data-model.md).
- No dynamic allocation per transfer: a single static/PSRAM sector buffer (`CFG_TUD_MSC_EP_BUFSIZE`).

## Host-observable acceptance (maps to spec)

- Drive enumerates without drivers, capacity = actual volume (FR-001, FR-010, SC-001).
- Read existing files (FR-004); create/write/delete files (FR-005); byte-identical (SC-005).
- With the module disabled, no LUN is registered → no drive (FR-002, SC-003).
