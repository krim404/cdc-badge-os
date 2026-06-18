---
description: "Task list for USB Mass Storage (vFAT file transfer)"
---

# Tasks: USB Mass Storage (vFAT file transfer)

**Input**: Design documents from `/specs/019-usb-mass-storage/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md

**Tests**: Only the two host unit tests explicitly named in plan.md (block LBA/bounds
mapping and the host-active write gate) are included. No full TDD; firmware is
hardware-verified via quickstart.md (constitution: tests OPTIONAL).

**Organization**: Grouped by user story. US3 (private-by-default toggle) is implemented
before the P1 MVP US1 because it wires the shared MSC engine to the user (enable → drive
appears); each story remains independently testable.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependency on an incomplete task)
- **[Story]**: US1 / US2 / US3 (Setup, Foundational, Polish have no story label)

## Path Conventions

Firmware repo layout: shared code under `components/`, app config under `main/`, USB stack
in `components/usb_badge/`, host tests under `test/host/`. The new module is
`components/mod_msc/`. Paths below are repository-relative.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Build/config prerequisites and the module skeleton.

- [x] T001 Rename the FAT partition label `plugins` → `vfat` in `partitions.csv` and in `PluginStorage::PARTITION_LABEL` (`components/plugin_manager/src/PluginStorage.cpp`); confirm `MOUNT_POINT=/vfat`, `SYSTEM_DIR=/vfat/system`, and `I18n::OVERLAY_DIR=/vfat/system/i18n` stay consistent (research R6; no migration code).
- [x] T002 [P] Enable the MSC class in `include/tusb_config.h`: `#define CFG_TUD_MSC 1` and `#define CFG_TUD_MSC_EP_BUFSIZE 4096` (match the wl sector size).
- [x] T003 [P] Enable FatFs `f_chmod` support (`FF_USE_CHMOD`) via the ESP-IDF FatFs config (`sdkconfig.defaults` / ffconf override) so runtime attribute setting is available (research R5); verify the build applies it.
- [x] T004 Create the `components/mod_msc/` skeleton: `CMakeLists.txt` (REQUIRES `cdc_core cdc_ui cdc_views cdc_hal cdc_log usb_badge plugin_manager nvs_flash freertos`), `include/mod_msc/UsbMscModule.h` (`class UsbMscModule : public core::IModule`, Meyer singleton), and a stub `src/UsbMscModule.cpp` (empty `init/start/stop/getMenuItems` + `extern "C" void mod_msc_register()`).
- [x] T005 Register the module: add `mod_msc` to the `MODULES` list in `main/CMakeLists.txt` and add `X("mod_msc", false)` to `MODULE_DEFAULT_MAP` in `main/module_defaults.h` (default-disabled).
- [x] T006 [P] Add the `mod_msc` English i18n table (`kStrings` with `mod_msc.title`, `mod_msc.status`, `mod_msc.connection`, etc.) in `components/mod_msc/src/UsbMscModule.cpp` and mirror every key in `assets/i18n/lang_de.json` with real UTF-8 umlauts.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: The shared MSC block engine + dynamic descriptor + re-enumeration. Required by
all user stories. Not user-shippable on its own (no module/toggle yet).

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [x] T007 Add the wl block accessor to `PluginStorage` (`components/plugin_manager/src/PluginStorage.cpp` + `include/plugin_manager/PluginStorage.h`): `blockRead/blockWrite/blockTotalBytes/blockSize` over the existing `s_wl_handle` (`wl_read/wl_write/wl_size/wl_sector_size`).
- [x] T008 Add the host-active gate to `PluginStorage` (same files): `setHostActive(bool)` / `hostActive()`; on the true→false transition, remount `/vfat` (unmount + mount) so host writes become visible (FR-006).
- [x] T009 Implement the TinyUSB MSC callbacks in a new `components/usb_badge/usb_msc.cpp` per `contracts/msc-block.md`: `tud_msc_inquiry_cb`, `tud_msc_test_unit_ready_cb`, `tud_msc_capacity_cb`, `tud_msc_is_writable_cb`, `tud_msc_read10_cb`, `tud_msc_write10_cb`, `tud_msc_start_stop_cb` (load → `setHostActive(true)`, eject → `setHostActive(false)`), `tud_msc_scsi_cb`; bounds-check every read/write against `wl_size()`; use one static `CFG_TUD_MSC_EP_BUFSIZE` buffer (no per-transfer allocation). Depends on T007, T008.
- [x] T010 [P] Extend the descriptor builder in `components/usb_badge/usb_hid.cpp`: emit `TUD_MSC_DESCRIPTOR` (1 interface + 1 bulk IN + 1 bulk OUT) when MSC is active, extend the interface/endpoint allocator, and add MSC to the endpoint-budget check (reject when CDC 3 + active HID/CCID + MSC 2 would exceed `CFG_TUD_ENDPOINT_MAX`); add the MSC string/index constants in `components/usb_badge/usb_descriptors.h` (research R3).
- [x] T011 Add the MSC API to `UsbManager` (`components/cdc_core/include/cdc_core/UsbManager.h` + `src/UsbManager.cpp`): `registerMassStorage(owner)` / `unregisterMassStorage(owner)` / `massStorageActive()`; set the MSC-active flag and route through `applyConfiguration()` → `usb_hid_apply_config()` (MSC flag) → existing `tud_disconnect()/tud_connect()` re-enumeration; include MSC in budget accounting so `registerMassStorage` returns `false` when over budget. Depends on T010.
- [x] T012 [P] Host unit test in `test/host/test_msc/`: `read10`/`write10` LBA + offset bounds against `wl_size()` (in-range succeeds, out-of-range returns error). Use project-relative `../` includes only.
- [x] T013 [P] Host unit test in `test/host/test_msc/`: the host-active gate refuses badge-side writes when `hostActive()` is true and triggers a remount on the true→false transition.

**Checkpoint**: MSC engine builds, host tests pass; drive can enumerate and read/write at the block level once a LUN is registered.

---

## Phase 3: User Story 3 - Storage stays private unless explicitly enabled (Priority: P1) 🎯

**Goal**: The service is off by default; enabling it (Expert → Modules) registers the MSC
LUN so the drive appears, disabling removes it, and the choice persists across reboots.

**Independent Test**: quickstart Scenario 1 (default badge → no drive) + Scenario 4 (toggle
on → drive appears within ~5 s, toggle off → gone, survives reboot).

- [x] T014 [US3] Implement `UsbMscModule::init/start/stop` in `components/mod_msc/src/UsbMscModule.cpp`: `init()` registers strings + registers the module with `ModuleRegistry`; `start()` calls `UsbManager::registerMassStorage("mod_msc")` and returns `false` on budget-full (→ `[FAIL]`); `stop()` calls `unregisterMassStorage`. Wire `mod_msc_register()` to `registerInitializer([]{ instance().init(); })`.
- [x] T015 [US3] Implement `UsbMscModule::getMenuItems()` returning a `SETTINGS_MENU` status view that shows enabled state and host-connected status (i18n `mod_msc.*`), in `components/mod_msc/src/UsbMscModule.cpp`.
- [x] T016 [P] [US3] Surface an MSC status indicator on the lockscreen/statusbar via `components/cdc_os_ui/` (`AppUi::updatePowerStatusIcons` + `StatusIcon` in `LockScreenView.h`) reflecting "MSC active / host connected" (reuse the `USB` icon or add a flag), per FR-012.
- [x] T017 [US3] Verify default-off + persistence: `mod_msc` shows `[OFF]` in Expert → Modules on a fresh badge; enabling registers the LUN and the host sees a drive; disabling removes it; the state persists across a reboot (existing `ModuleRegistry` NVS `modules/disabled`). Confirms Scenarios 1 and 4.

**Checkpoint**: Drive appears only when the user enables `mod_msc`; default-off and persistence verified.

---

## Phase 4: User Story 1 - Copy files onto the badge over USB (Priority: P1) 🎯 MVP

**Goal**: With the service enabled, files copied from the host onto the drive land safely on
the volume and appear in the badge's Files menu, without corrupting badge data.

**Independent Test**: quickstart Scenario 2 (copy a file → it appears in Files and opens) +
Scenario 6 (badge writes gated while host-connected, no corruption, consistent after unsafe removal).

- [x] T018 [US1] Enforce the host-active write gate in `mod_vfat` write commands (`components/mod_vfat/src/VfatModule.cpp`): `VFAT PUT/RECEIVE/MKDIR/RMDIR/DELETE` check `PluginStorage::hostActive()` and refuse/defer while a host is connected.
- [x] T019 [US1] Enforce the host-active write gate in the plugin/lang-overlay writers (`components/plugin_manager/src/PluginSerialCommands.cpp` and `PluginStorage.cpp`): `PLUGIN UPLOAD/UPLOAD_META/UPLOAD_AOT/UPLOAD_LANG` refuse/defer while `hostActive()`.
- [x] T020 [US1] Set advisory `system`-folder protection: at mount, `f_chmod(AM_RDO|AM_HID|AM_SYS)` on `/vfat/system` (best-effort on its entries) in `components/plugin_manager/src/PluginStorage.cpp`; skip while `hostActive()`; idempotent (research R5).
- [x] T021 [US1] Verify copy-onto-badge end to end: a host-copied file appears in the main-menu Files browser after eject (remount path from T008/T009) and opens; badge writes are refused during host connection; filesystem stays consistent after an unsafe cable pull. Confirms Scenarios 2 and 6.

**Checkpoint**: Files can be dragged onto the badge and used; no corruption; system folder protected.

---

## Phase 5: User Story 2 - Copy files off the badge to a computer (Priority: P2)

**Goal**: With the service enabled, files on the badge can be copied to the host and are
byte-identical.

**Independent Test**: quickstart Scenario 3 (copy a known file off the badge, compare to the
on-badge copy).

- [x] T022 [US2] Verify the read-off path (`read10` from T009): existing files are visible and readable over MSC and byte-identical to the on-badge copy (e.g. checksum via `VFAT GET`); fix any read-correctness issues in `components/usb_badge/usb_msc.cpp`. Confirms Scenario 3 and SC-005.

**Checkpoint**: Bidirectional transfer confirmed; reads byte-identical.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, verification, and constitution compliance across all stories.

- [x] T023 [P] Update the website docs: add the USB Mass Storage section to `website/src/content/docs/power/storage-tools.md` and the USB-services/settings page, and note the default-off toggle, behaviour, and advisory `system` protection in the relevant `dev/` pages (CLAUDE.md docs mandate; current-state only).
- [x] T024 [P] Verify the build: `~/.platformio/penv/bin/pio run` is green and `~/.platformio/penv/bin/pio test -e native` is green (including the new `test_msc` suite).
- [ ] T025 Run quickstart.md Scenarios 1–7 on hardware (flash via the `AUTH 0000` → `BOOTLOADER` serial path); explicitly confirm Scenario 5 (serial console coexists) and Scenario 7 (over-budget combination shows `[FAIL]`, no crash).
- [x] T026 [P] Constitution compliance: confirm no version numbers were bumped and `host_api.h` is unchanged (Principle V), and that deleting `components/mod_msc/` leaves the build green with the MSC infra dormant (Principle I removability).

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately.
- **Foundational (Phase 2)**: Depends on Setup. BLOCKS all user stories.
- **US3 (Phase 3)**: Depends on Foundational. First user-visible slice (toggle + enumeration).
- **US1 (Phase 4)**: Depends on Foundational; uses the toggle from US3 to exercise (enable, then copy). Adds the write gate + system protection + visibility.
- **US2 (Phase 5)**: Depends on Foundational (read path); independently testable.
- **Polish (Phase 6)**: Depends on the desired stories being complete.

### Within Each User Story

- US3: T014 → T015 (same file) sequential; T016 parallel; T017 verifies.
- US1: T018 parallel to T019/T020; T019 and T020 both touch `PluginStorage.cpp` → sequence; T021 verifies.
- US2: single verification task.

### Parallel Opportunities

- Setup: T002, T003, T006 in parallel.
- Foundational: T010 parallel to T007/T008; T012, T013 in parallel (after the T007–T009 signatures exist).
- US3: T016 parallel to T014/T015.
- Polish: T023, T024, T026 in parallel.

---

## Parallel Example: Foundational

```bash
# After T007–T009 signatures exist, the descriptor work and host tests run in parallel:
Task: "T010 Extend descriptor builder + MSC endpoint budget in components/usb_badge/usb_hid.cpp"
Task: "T012 Host test: read10/write10 bounds in test/host/test_msc/"
Task: "T013 Host test: host-active write gate in test/host/test_msc/"
```

---

## Implementation Strategy

### MVP (deliver value fastest)

1. Phase 1 Setup → Phase 2 Foundational (engine + host tests green).
2. Phase 3 US3 → the drive is private-by-default and appears/disappears on toggle. **Validate Scenarios 1 + 4.**
3. Phase 4 US1 → drag files onto the badge, they appear in Files, no corruption. **Validate Scenarios 2 + 6.** This is the headline MVP.
4. STOP and demo if ready.

### Incremental Delivery

1. Foundational → engine ready (not shippable).
2. US3 → private-by-default toggle (shippable slice).
3. US1 → copy-onto-badge (MVP value).
4. US2 → copy-off verification.
5. Polish → docs, build/hardware verification, compliance.

---

## Notes

- [P] = different files, no dependency on an incomplete task.
- The MSC class plumbing (T009–T011) lives in shared `usb_badge`/`cdc_core` infra and stays dormant without a registrant, so `mod_msc` remains removable (Principle I; verified in T026).
- Endpoint budget is the hard constraint: over-budget combinations are rejected as `[FAIL]`, never crash (research R3; Scenario 7).
- Never bump version numbers without explicit instruction (Principle V).
- Flash conservation: review before flashing; verify on hardware via quickstart, do not trial-and-error.
