# Plan: VFAT user area + hidden system folder + main-menu Files

**Status**: Draft for review (not yet implemented)
**Branch**: `message-transfer-framework`

## Goal

Today the `plugins` FAT partition (mounted at `/vfat`) stores plugin files and
the i18n overlays in its **root**. A user-facing file browser is therefore unsafe
(a user could delete `hello.wasm` or `lang_de.json`), so the explorer is hidden
under Expert.

Move all system content into a **hidden subfolder** so the partition root becomes
a safe user area:

- A user-facing **"Files"** entry in the **main menu** browses the root and hides
  the system folder.
- The Expert entry is renamed **"System Files"** and shows everything.
- Bundle a demo image + demo Markdown into the default partition image so the
  user area is non-empty out of the box.

## Layout decision

System folder: **`/vfat/.system/`** (leading dot → already skipped by
`VfatFs::list`, so it is invisible to the user view for free).

```
/vfat/                      partition root = USER AREA (browsable, deletable)
├── demo.jpg                   bundled demo (user area)
├── demo.md                    bundled demo (user area)
├── <user files…>
└── .system/                   HIDDEN system folder
    ├── <id>.wasm / .meta / .aot / .lang / .disabled   (plugins)
    └── i18n/
        └── lang_<code>.json   (UI language overlays)
```

The partition label and mount point stay `/vfat` (internal only). No migration
code (pre-1.0): existing on-device root plugins/overlays will not be found after
the change; users re-flash the default image and re-upload plugins. Documented in
release notes.

## Key finding that bounds scope

Plugin upload is **protocol-based**: `PLUGIN UPLOAD/UPLOAD_META/UPLOAD_AOT/UPLOAD_LANG <id>`
let the firmware resolve the path via `PluginStorage`. So **the webflasher
(`cdc-badge-plugins/webflasher/`) and `tools/upload.py` plugin paths need NO
change** — moving `PluginStorage` paths is enough. Only **raw** paths change:
the i18n directory and the default-image staging.

## Changes — firmware (cdc-badge-os)

### Storage paths (the core move)
- `components/plugin_manager/src/PluginStorage.cpp`: add `SYSTEM_SUBDIR = ".system"`;
  `wasmPath/aotPath/metaPath/langPath/disabledPath` → `<MOUNT>/.system/<id>.<ext>`;
  `listPluginIds()` scans `<MOUNT>/.system` (create it on mount if missing). `basePath()`
  stays `/vfat`; add a `systemPath()` helper.
- `components/cdc_ui/src/I18n.cpp` + `I18n.h`: `OVERLAY_DIR` `/vfat/i18n` →
  `/vfat/.system/i18n`. Scan/load logic unchanged otherwise.
- `components/plugin_manager/src/PluginSerialCommands.cpp`: it references
  `OVERLAY_DIR` / `/vfat/i18n` (lang-overlay handling) → use the new constant
  (reuse `I18n::OVERLAY_DIR` rather than a literal).

### Explorer + hidden filtering
- `components/mod_vfat/src/VfatFs.{h,cpp}`: add an overload/param to `list()` to
  optionally include dot-entries (`listAll`), keeping the default dot-filter for
  the user view.
- `components/mod_vfat/src/VfatExplorerView.{h,cpp}`: add a `showSystem_` flag;
  user mode uses the filtered list (root, `.system` hidden) and forbids ascending
  above root; system mode uses `listAll` and may enter `.system`. Two factory
  functions (`filesView()` user, `systemFilesView()` admin) both reusing the
  singleton with the flag set.

### Menu entries (mod_vfat)
- `components/mod_vfat/src/VfatModule.cpp` `getMenuItems()` returns **two** items:
  - `core.files` → `MenuLocation::MAIN_MENU` (user view), priority chosen to sit
    sensibly in the main menu.
  - `core.system_files` → `MenuLocation::EXPERT_MENU` (system view) — replaces the
    current `core.vfat` Expert entry.
- i18n keys: add `core.files` ("Files") and `core.system_files` ("System Files")
  to `cdc_ui/src/I18n.cpp` (EN) and `assets/i18n/lang_de.json` (DE: "Dateien",
  "Systemdateien"). Keep/repoint `core.vfat` or drop if unused.

### Demo assets bundled into the default image
- Add `assets/vfat/demo.jpg` (the progressive JPEG) and `assets/vfat/demo.md`
  (a short Markdown showcase) to the repo. **License note**: confirm the demo
  image is freely redistributable (the picsum/Unsplash image is free-use; or
  swap for a generated image).

## Changes — tools / build / CI (cdc-badge-os)

- `tools/build_lang_image.py`: stage language files into `i18n/` **under
  `.system/`** (i.e. image path `.system/i18n/lang_*.json`), and copy
  `assets/vfat/*` (demo.jpg, demo.md) into the image **root**. Output
  `plugins_initial.bin` unchanged in name/size/offset.
- `tools/upload.py`: `cmd_lang` raw path `i18n/<file>` → `.system/i18n/<file>`
  (and `VFAT MKDIR .system` + `.system/i18n`). Plugin upload (`PLUGIN UPLOAD …`)
  unchanged. `--put` still targets the user root (correct).
- `tools/backup.py`: uses `VFAT RECEIVE` — verify whether it targets i18n/plugin
  raw paths; adjust only if it writes system content.
- `tools/start_webflasher.py` + `.forgejo/workflows/{build,deploy-pages}.yml`:
  offsets/filenames unchanged; they just call `build_lang_image.py`, so they get
  the new layout automatically. Verify no literal `i18n/` root assumptions.

## Changes — cdc-badge-plugins repo

- `webflasher/`: uploads plugins via `PLUGIN UPLOAD` protocol → **no path change**.
  Verify it does not write raw `/vfat/...` or `i18n/...` paths (grep shows only
  `PLUGIN UPLOAD`). 
- Docs (`README.md`, `docs/*.md`, plugin `README`s) that mention `/vfat` paths
  → update prose to the `.system` layout where they describe on-device paths.
  (SDK/runtime code uses host calls, not raw paths — no functional change.)

## Documentation (website)

Update pages that state on-device paths or the i18n directory to the new layout:
`dev/architecture.md`, `dev/build-system.md`, `dev/plugin-sdk.md`,
`dev/proto/serial-commands.md`, `power/companion-tools.md`, `power/languages.md`,
`power/vfat/{index,install,manage,capabilities}.md`, `power/storage-tools.md`
(split into user "Files" vs Expert "System Files"), `start/first-flash.md`,
`power/index.md`. (The `website/evidence/*` and `website/SPEC.md` are as-built
notes — update for accuracy.)

## Verification

1. `pio run` green; `pio test -e native` green.
2. `python tools/build_lang_image.py …` produces `plugins_initial.bin`; inspect it
   contains `.system/i18n/lang_*.json` + root `demo.jpg`/`demo.md`.
3. On device (flash new firmware + image): main menu shows **Files** → lists
   `demo.jpg`, `demo.md` (and NOT `.system`); opening each works. Expert →
   **System Files** → shows `.system/` with plugins + i18n. Language switch still
   works (overlay found at new path). Existing plugins load (scan `.system`).
4. `tools/upload.py --lang-overlay …` lands in `.system/i18n`; `--put` lands in
   root; `PLUGIN UPLOAD` still installs and the plugin starts.
5. Webflasher install still works against the new firmware.

## Decisions (confirmed)

1. **System folder**: `system` (no leading dot). NOTE: a leading-dot name
   (`.system`) does NOT work on ESP-IDF FatFs — `readdir` does not enumerate
   dot-prefixed directories (the folder is reachable by exact path via `CD`, but
   never listed), so the System Files view could never show it. The user view
   hides `system` by NAME at the partition root only (subfolders named `system`
   are not affected); the System Files view shows it.
2. **Demo image**: bundle the picsum/Unsplash progressive `demo.jpg` (free-use) as a
   committed binary under `assets/vfat/`.
3. **Menu labels**: fully replace — main-menu **"Files"** (`core.files`) + Expert
   **"System Files"** (`core.system_files`); the old `core.vfat` Expert entry is removed.
