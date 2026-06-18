# Phase 1 Data Model: USB Mass Storage (vFAT file transfer)

This feature is firmware, not a database. "Data model" here means the runtime state objects,
their fields, validation/invariants, and state transitions.

## Entities

### UsbMscModule (the optional service)

The `core::IModule` that owns the user-facing capability. Singleton, mirrors `UsbHidModule`.

| Field | Type | Notes |
|-------|------|-------|
| `state_` | `core::ServiceState` | UNINITIALIZED → INITIALIZED → STARTED → STOPPED |
| (enable flag) | NVS, via `ModuleRegistry` | `module_defaults.h` default = **false**; persisted in NVS namespace `"modules"`, key `"disabled"` (comma-separated). Not stored by the module itself. |

**Invariants**:
- `start()` registers the MSC LUN with `UsbManager` and returns `false` if the USB endpoint
  budget is exhausted (no crash; surfaces as `[FAIL]`).
- `stop()` unregisters the LUN and triggers re-enumeration (drive disappears).
- Removable: deleting the module leaves MSC class plumbing dormant (no LUN registered).

### MscLun (storage logical unit, shared infra)

The single MSC logical unit exposed to the host, backed by the `vfat` wl block device.

| Field | Type | Notes |
|-------|------|-------|
| `active` | bool | True between `UsbManager::registerMassStorage()` and `unregisterMassStorage()`. Drives whether the MSC interface is in the descriptor. |
| `wl_handle` | `wl_handle_t` | Owned by `PluginStorage`; block accessor reads/writes the wl logical space. |
| `block_size` | uint16 | `wl_sector_size()` = 4096 B (reported in `tud_msc_capacity_cb`). |
| `block_count` | uint32 | `wl_size() / block_size`. |
| `writable` | bool | True (FR-005); reported by `tud_msc_is_writable_cb`. |
| `host_present` | bool | Set on `tud_msc_start_stop_cb(load)` / cleared on eject or USB unmount. Mirrors into the host-active gate. |

**Validation (per `read10`/`write10`)**: `lba*block_size + offset + bufsize <= wl_size()`,
else `TUD_MSC_RET_ERROR`. `lun == 0`, else error.

### VfatVolume / host-active gate (PluginStorage)

The existing `vfat` partition mount, extended with a block accessor and a write gate.

| Field | Type | Notes |
|-------|------|-------|
| `s_wl_handle` | `wl_handle_t` | Existing static handle; block accessor added. |
| `MOUNT_POINT` | const | `/vfat`. |
| `SYSTEM_DIR` | const | `/vfat/system` (attribute-protected, R5). |
| `host_active` | bool (new) | True while a host has the MSC LUN. **Gates all badge-side writes** (refuse/defer). |

**Invariants**:
- While `host_active` is true: no badge-side write to `/vfat` (plugin upload, `VFAT PUT/RECEIVE`,
  lang writes, attribute set). Reads allowed.
- On `host_active` false→ (host disconnect/eject): remount `/vfat` so host-written files
  appear in the Files browser.

### USB configuration descriptor (shared infra)

Already built dynamically in `usb_hid.cpp::build_config_descriptor`. Extended so the MSC
interface (1 interface, 2 bulk endpoints) is emitted when `MscLun.active`.

**Invariant**: total endpoints (CDC 3 + active HID/CCID + MSC 2) `<= CFG_TUD_ENDPOINT_MAX`
(8) and within the ESP32-S3 hardware limit; otherwise registration is rejected.

## State Transitions

### Module enable/disable (user, via Expert → Modules)

```
[Disabled]  --enable-->  start(): register MSC LUN
                              |-- budget OK --> [Active]  (re-enumerate; drive appears)
                              \-- budget full --> [FAIL]  (state STOPPED; no drive)
[Active]    --disable--> stop(): unregister LUN --> [Disabled] (re-enumerate; drive gone)
```

Enable state persists across reboots (NVS). At boot, `ModuleRegistry` starts `mod_msc` only
if enabled; default is disabled (FR-002, SC-003, SC-007).

### Host connection lifecycle (while module Active)

```
[No host]  --host mounts (start_stop load / SOF)-->  [Host present]
   set host_active=true; badge writes refused; badge reads still served (may be stale)
[Host present] --eject / USB disconnect-->  [No host]
   set host_active=false; remount /vfat; Files browser reflects new files
```

### USB re-enumeration (shared, existing mechanism)

```
register/unregister MSC LUN -> UsbManager::applyConfiguration()
   -> usb_hid_apply_config(): rebuild descriptor (+/- MSC interface)
   -> if tud_inited(): tud_disconnect() -> delay 20ms -> tud_connect()  (host re-probes)
```

## Relationships

- `UsbMscModule` 1—1 `MscLun` (registers/unregisters it via `UsbManager`).
- `MscLun` 1—1 `VfatVolume` (block accessor over the same `wl_handle_t` the FatFs mount uses).
- `VfatVolume.host_active` gates badge writes from all writers (plugin_manager, mod_vfat
  serial commands, i18n overlay writer).
- Descriptor composition: CDC (fixed) + dynamic {HID×n, CCID, MSC} bounded by endpoint budget.
