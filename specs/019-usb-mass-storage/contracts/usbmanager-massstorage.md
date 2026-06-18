# Contract: UsbManager mass-storage registration + module lifecycle + storage gate

These are firmware-internal contracts (not a plugin/host API; `host_api.h` is unchanged).

## UsbManager (cdc_core) — new surface

Mirrors the existing `registerInterface()/unregisterInterface()` HID/CCID path.

```cpp
// cdc_core/UsbManager.h
/**
 * \brief Register the single USB Mass Storage LUN, adding the MSC interface and re-enumerating.
 * \return true if registered; false if the USB endpoint budget is exhausted.
 */
bool registerMassStorage(const char* owner);

/** \brief Unregister the MSC LUN and re-enumerate (drive disappears). */
void unregisterMassStorage(const char* owner);

/** \brief Whether the MSC LUN is currently active (in the descriptor). */
bool massStorageActive() const;
```

**Contract**:
- `registerMassStorage` sets `mscActive_=true` and calls `applyConfiguration()`, which passes
  the MSC flag to `usb_hid_apply_config()` to (re)build the descriptor with `TUD_MSC_DESCRIPTOR`
  and trigger `tud_disconnect()/tud_connect()`.
- Endpoint-budget accounting includes MSC's 2 bulk endpoints. If CDC(3) + active HID/CCID + MSC(2)
  would exceed `CFG_TUD_ENDPOINT_MAX` (8) or the hardware limit, `registerMassStorage` returns
  `false` and makes no descriptor change. Symmetric: a later HID/CCID registration that would
  exceed the budget while MSC is active also fails (existing `UsbBudgetFull` path).
- Idempotent: registering when already active is a no-op returning `true`.

## Module lifecycle (mod_msc) — `core::IModule`

```cpp
const char* getName() const override { return "mod_msc"; }   // matches module_defaults / MODULES
bool init();    // registerStrings(); ModuleRegistry::registerModule(this); state=INITIALIZED
bool start();   // UsbManager::registerMassStorage("mod_msc"); false on budget-full → STOPPED/[FAIL]
void stop();    // UsbManager::unregisterMassStorage("mod_msc"); state=STOPPED
uint8_t getMenuItems(ModuleMenuItem*, uint8_t);  // SETTINGS_MENU status view (i18n "mod_msc.*")
extern "C" void mod_msc_register();  // registerInitializer([]{ instance().init(); })  (default-off)
```

**Contract**:
- Default-disabled via `X("mod_msc", false)` in `module_defaults.h`; enable/disable via the
  Expert → Modules list (no bespoke toggle). Enable state persists in NVS (FR-003, SC-007).
- Toggling on → `start()` → drive appears; off → `stop()` → drive disappears (FR-008).
- `start()` returning `false` on budget-full is reported as `[FAIL]`; never crashes (R3).

## PluginStorage (plugin_manager) — block accessor + write gate

```cpp
// Block accessor used by the MSC callbacks (shared infra)
bool blockRead (uint32_t lba, uint32_t offset, void* buf, uint32_t len);   // wl_read
bool blockWrite(uint32_t lba, uint32_t offset, const void* buf, uint32_t len); // wl_write
uint64_t blockTotalBytes();   // wl_size()
uint16_t blockSize();         // wl_sector_size() (4096)

// Host-active gate (FR-007)
void setHostActive(bool active);  // on host_active false->: remount /vfat
bool hostActive() const;
```

**Contract**:
- All badge-side write paths (`PluginStorage` plugin upload, `mod_vfat` `VFAT PUT/RECEIVE`,
  i18n overlay writes, the R5 attribute set) MUST check `hostActive()` and refuse/defer when true.
- `setHostActive(false)` remounts `/vfat` (`unmount` + `mount`) so host-written files become
  visible to the Files browser (FR-006).
- The block accessor and the FatFs mount share the same `wl_handle_t` (the same wl logical space).

## Acceptance (maps to spec)

- FR-002/FR-003/FR-008/SC-003/SC-007: default-off, toggle persists, drive appears/disappears.
- FR-007: badge never writes while host-active; host is sole writer.
- FR-009: badge stays operable (reads served, other subsystems untouched) while host connected.
- FR-011: CDC serial console (interfaces 0–1) is never removed; MSC is additive (SC-006).
