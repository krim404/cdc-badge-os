# Evidence ledger: Intermediate / power-tools pages

Tags: VERIFIED = exact source line supports the claim; GAP = not verifiable in source.

Covers: power/expert-menu.md, power/languages.md, power/storage-tools.md,
power/companion-tools.md, power/index.md.

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Expert menu is reached via Tools | components/cdc_os_ui/src/AppUi.cpp:484-491 (`kToolsFixed[]` includes `{"core.expert", ..., showExpertMenu}`) | VERIFIED |
| Tools sits under the main menu | components/cdc_os_ui/src/AppUi.cpp:62 (`MainMenuFixed { MM_PLUGINS, MM_TOOLS, MM_SETTINGS }`); 463 (`core.tools`) | VERIFIED |
| Expert entry shows a caution prompt on entry that must be acknowledged | components/cdc_os_ui/src/ExpertMenuUi.cpp:236-247 (`showToastInfo(tr("core.expert_warning"), 0)`); I18n.cpp:189 (`core.expert_warning = "Caution"`) | VERIFIED |
| Expert menu = fixed top + module entries + fixed bottom | components/cdc_os_ui/src/ExpertMenuUi.cpp:39-52, 252-280 | VERIFIED |
| Top fixed entries: Hardware Info, Modules, Backup, Set Duress PIN | components/cdc_os_ui/src/ExpertMenuUi.cpp:39-44 (`kExpertTop[]`) | VERIFIED |
| Bottom fixed entries: TR01 Cache Rebuild, TR01 Cache Cleanup, Bootloader | components/cdc_os_ui/src/ExpertMenuUi.cpp:45-49 (`kExpertBottom[]`) | VERIFIED |
| Entry labels English text | components/cdc_ui/src/I18n.cpp:44 (Hardware Info), 111 (Modules), 199 (Backup), 87 (Set Duress PIN), 186-187 (TR01 Cache Rebuild/Cleanup), 190 (Bootloader) | VERIFIED |
| Hardware Info opens a read-only system-test screen | components/cdc_os_ui/src/ExpertMenuUi.cpp:196-198 (`runSystemTest` -> `showHardwareInfo`) | VERIFIED |
| Hardware Info probes I2C, BQ25895, TCA9535, display, TROPIC01, WiFi, BLE | components/cdc_os_ui/src/HardwareInfo.cpp:56,62,68,74,80-81,107,113 | VERIFIED |
| Hardware Info shows TR01 session state, RISC-V/SPECT FW, R-Mem slot size | components/cdc_os_ui/src/HardwareInfo.cpp:81,87-100 | VERIFIED |
| Hardware Info shows heap/DRAM memory usage | components/cdc_os_ui/src/HardwareInfo.cpp:115-129 | VERIFIED |
| Modules entry opens module list view | components/cdc_os_ui/src/ExpertMenuUi.cpp:40 (`{"core.modules", showModulesView}`); 183-191 | VERIFIED |
| Each module row shows name + status label | components/cdc_os_ui/src/ExpertMenuUi.cpp:166-169 (`"%s %s", getName(), getModuleStatusLabel(i)`) | VERIFIED |
| Selecting a module toggles enabled and starts/stops it | components/cdc_os_ui/src/ExpertMenuUi.cpp:118-141 (`toggleModuleEnabled` -> startModule/stop) | VERIFIED |
| Start failure reasons: slot error, no free USB slot, generic | components/cdc_os_ui/src/ExpertMenuUi.cpp:124-135 (`ModuleStartFailure::SlotError/UsbBudgetFull/Generic`) | VERIFIED |
| USB-config-changing toggle shows sticky replug alert | components/cdc_os_ui/src/ExpertMenuUi.cpp:144-146 (`newlyRequiresReplug` -> `showToastAlertSticky("core.usb_replug_required")`) | VERIFIED |
| Module in error state offers retry instead of toggle | components/cdc_os_ui/src/ExpertMenuUi.cpp:104-115, 72-85 (`onModuleRetryConfirm` -> `retryModule`) | VERIFIED |
| Factory-default module enable map fixed at build time; user toggles persist in NVS separately | main/module_defaults.h:3-10, 13-27 | VERIFIED |
| Backup entry opens the Export/Import/Delete submenu | components/cdc_os_ui/src/ExpertMenuUi.cpp:42 (`{"core.backup", showBackupMenu}`); BackupMenuUi.cpp:208-211 | VERIFIED |
| Set Duress PIN starts the duress setup flow | components/cdc_os_ui/src/ExpertMenuUi.cpp:43; AppUi.cpp:389 (`showDuressPinSetup`) | VERIFIED |
| TR01 Cache Rebuild rebuilds cache, reports OK/failure | components/cdc_os_ui/src/ExpertMenuUi.cpp:203-212 (`TropicStorage::rebuild()` -> OK/failed toast) | VERIFIED |
| TR01 Cache Cleanup cleans cache, reports OK/failure | components/cdc_os_ui/src/ExpertMenuUi.cpp:217-226 (`TropicStorage::cleanup()` -> OK/failed toast) | VERIFIED |
| Bootloader reboots into USB download mode (lock-screen banner, USB detach, hard reset, download-boot bit) | components/cdc_os_ui/src/ExpertMenuUi.cpp:282-316 (`prepareForBootloaderReset`, `RTC_CNTL_FORCE_DOWNLOAD_BOOT`, `esp_restart`) | VERIFIED |
| Module-provided entries appear between top and bottom groups (e.g. vFAT) | components/cdc_os_ui/src/ExpertMenuUi.cpp:261-274; mod_vfat/src/VfatModule.cpp:249-262 (MenuLocation::EXPERT_MENU) | VERIFIED |
| UI strings looked up by key via tr() | components/cdc_ui/src/I18n.cpp:355-365 (`tr()`) | VERIFIED |
| English fallback compiled into firmware (core + per-module tables) | components/cdc_ui/src/I18n.cpp:290-310 (`registerCoreEnglishTable`, `registerEnglishTable`) | VERIFIED |
| Lookup: overlay first (if not "en"), else English, else "?key" | components/cdc_ui/src/I18n.cpp:355-365 | VERIFIED |
| Each non-English language is a flat lang_<code>.json of key:value | components/cdc_ui/include/cdc_ui/I18n.h:9-16; CLAUDE.md i18n section | VERIFIED |
| Overlay files live under /vfat/system/i18n | components/cdc_ui/include/cdc_ui/I18n.h (`OVERLAY_DIR = "/vfat/system/i18n"`) | VERIFIED |
| Active overlay parsed into PSRAM | components/cdc_ui/src/I18n.cpp:461-526 (psramAlloc blob/refs) | VERIFIED |
| UTF-8 umlauts converted to CP437 at parse time | components/cdc_ui/src/I18n.cpp:490,507 (`cdc::core::cp437::fromUtf8`) | VERIFIED |
| Picker reached via Settings -> Language | components/cdc_os_ui/src/AppUi.cpp:525 (`SETTINGS_IDX_LANGUAGE = core.language`), 606-609 | VERIFIED |
| Picker: English first, then one entry per lang_<code>.json | components/cdc_os_ui/src/AppUi.cpp:641-658 (`addLanguage("en")` then `availableOverlayLanguages()`) | VERIFIED |
| Overlay entries labelled with file's core.lang_name endonym | components/cdc_os_ui/src/AppUi.cpp:651 (`languageName(code)`); I18n.cpp:419-421, 554-563 | VERIFIED |
| Missing core.lang_name -> code shown | components/cdc_ui/src/I18n.cpp:407 (default name=code), 419-422 (only overrides if present); 563 (return code) | VERIFIED |
| Directory scan discovers lang_*.json (skips "en") | components/cdc_ui/src/I18n.cpp:384-435 (`scanAvailableLanguages`, opendir OVERLAY_DIR, prefix "lang_", suffix ".json") | VERIFIED |
| lang_de sets core.lang_name = "Deutsch" | assets/i18n/lang_de.json:2 | VERIFIED |
| Selecting a language applies immediately, persists, reloads plugin overlay | components/cdc_os_ui/src/AppUi.cpp:676-683 (`setLanguageCode` + `reloadActiveLangOverlay` + pop) | VERIFIED |
| Language choice persisted in NVS | components/cdc_ui/src/I18n.cpp:379 (`saveLanguageToNvs`), 566-577 (`loadLanguageFromNvs`) | VERIFIED |
| Missing/omitted keys fall back to English (partial overlay = partial translation) | components/cdc_ui/src/I18n.cpp:358-361 | VERIFIED |
| mod_vfat default-enabled | main/module_defaults.h:27 (`X("mod_vfat", true)`) | VERIFIED |
| mod_vfat adds a main-menu Files entry (user area, hides system/) and an Expert System Files entry (full view) | components/mod_vfat/src/VfatModule.cpp (MenuLocation::MAIN_MENU + MenuLocation::EXPERT_MENU) | VERIFIED |
| mod_vfat registers AUTH-gated VFAT serial command | components/mod_vfat/src/VfatModule.cpp:225-228 (registerCommand "VFAT", requireAuth=true) | VERIFIED |
| VFAT sub-commands: LIST/PWD/CD/GET/PUT/RECEIVE/DELETE/MKDIR/RMDIR/FREE | components/mod_vfat/src/VfatModule.cpp:183-195 (`kSubs[]`) | VERIFIED |
| VFAT PUT unescapes \n \t \\ | components/mod_vfat/src/VfatModule.cpp:53-67, 119 | VERIFIED |
| VFAT RECEIVE streams binary verified by size + crc32 | components/mod_vfat/src/VfatModule.cpp:165-181 (size, crc32_hex; `beginFileReceive`) | VERIFIED |
| VFAT FREE reports total/used/free KB | components/mod_vfat/src/VfatModule.cpp:155-163 | VERIFIED |
| mod_nvsedit default-enabled | main/module_defaults.h:23 (`X("mod_nvsedit", true)`) | VERIFIED |
| NVS deletes gated by FEATURE_NVS_EDIT, which defaults to 0 | components/mod_nvsedit/src/NvsEditModule.cpp:63-65 (`deleteEnabled() = FEATURE_NVS_EDIT != 0`); cdc_core/include/cdc_core/feature_flags.h:27-28 (`#define FEATURE_NVS_EDIT 0`) | VERIFIED |
| Label is "NVS Browser" (read-only) or "NVS Editor" (writable) | components/mod_nvsedit/src/NvsEditModule.cpp:546 (`deleteEnabled() ? "NVS Editor" : "NVS Browser"`) | VERIFIED |
| NVS entry sits in Tools menu | components/mod_nvsedit/src/NvsEditModule.cpp:551 (`MenuLocation::TOOLS_MENU`) | VERIFIED |
| Opening NVS asks for confirmation first | components/mod_nvsedit/src/NvsEditModule.cpp:519-525 (`showConfirm`, WARNING icon), 507-513 | VERIFIED |
| NVS browse: namespaces -> keys (with type) -> value preview | components/mod_nvsedit/src/NvsEditModule.cpp:98-150, 453-462, 490-501 | VERIFIED |
| Key types shown (u8/i32/str/blob etc.) | components/mod_nvsedit/src/NvsEditModule.cpp:79-93 (`nvsTypeToString`) | VERIFIED |
| Value preview: ints decimal+hex, str inline, short blob hex, large blob byte count | components/mod_nvsedit/src/NvsEditModule.cpp:213-289 | VERIFIED |
| Writable editor context menu: Delete Key, Delete NS | components/mod_nvsedit/src/NvsEditModule.cpp:354-360, 305-352 | VERIFIED |
| Deletes irreversible; warned before entry | components/mod_nvsedit/src/NvsEditModule.cpp:520-521 ("Deletes are irreversible. Continue?") | VERIFIED |
| mod_nvsedit registers no serial command | components/mod_nvsedit/src/NvsEditModule.cpp (no registerCommand call; only getMenuItems) | VERIFIED |
| flash_firmware.py: --dir or --release, --port, --erase-nvs | tools/flash_firmware.py:225-234 | VERIFIED |
| flash offsets bootloader 0x0 / partitions 0x8000 / app 0x50000 | tools/flash_firmware.py:36-40 (FLASH_MAP) | VERIFIED |
| --erase-nvs wipes NVS partition (resets settings) | tools/flash_firmware.py:193-204, 233-234 | VERIFIED |
| flash port auto-detected if omitted | tools/flash_firmware.py:46-53, 251 | VERIFIED |
| upload.py modes: plugin (wasm/meta + list/info/delete/start/stop), lang-overlay, put | tools/upload.py:8-22, 310-322 | VERIFIED |
| upload.py --lang-overlay writes to partition-relative system/i18n/ (VFAT MKDIR system + system/i18n) and reloads | tools/upload.py | VERIFIED |
| upload.py --put streams via VFAT RECEIVE | tools/upload.py:13-15, 313 | VERIFIED |
| upload.py needs pyserial, supports --pin AUTH | tools/upload.py:29, 306-307 | VERIFIED |
| ble_serial.py: BLE NUS console; --address/--name/--scan | tools/ble_serial.py:4-16, 160-176 | VERIFIED |
| ble_serial.py needs bleak | tools/ble_serial.py:17, 22-25 | VERIFIED |
| backup.py modes: export/import/delete, upload/download, decrypt/encrypt(+out/pass) | tools/backup.py:28-39, 338-346 | VERIFIED |
| backup.py off-device crypto needs no badge | tools/backup.py:37-39 | VERIFIED |
| backup.py reproduces byte-identical container | tools/backup.py:9-25 | VERIFIED |
| coredump.py usage `python tools/coredump.py [port]` | tools/coredump.py:6-9, 60-63 | VERIFIED |
| coredump.py reads coredump partition + runs GDB; needs esp-coredump/esptool | tools/coredump.py:11-19, 26-38 | VERIFIED |
| build_lang_image.py builds plugins FAT seeding system/i18n lang files + user-area assets; --lang-dir/--assets-dir/--output/--partition-size | tools/build_lang_image.py | VERIFIED |
| build_lang_image default lang-dir assets/i18n, default assets-dir assets/vfat | tools/build_lang_image.py | VERIFIED |
| 2fa.py provisions TOTP/HOTP over serial; list/add/get/del + options | tools/2fa.py:23-30, 154-169 | VERIFIED |
| start_webflasher.py serves web-flasher over HTTP; --port/--no-browser/--build/--lang | tools/start_webflasher.py:13-14, 150-160 | VERIFIED |
| start_webflasher.py --lang invokes build_lang_image | tools/start_webflasher.py:84-89 | VERIFIED |
| pio_submodules.py initializes pinned submodules if missing | tools/pio_submodules.py:25-41 | VERIFIED |
| pio_component_manager.py enables ESP-IDF Component Manager | tools/pio_component_manager.py:1-4 | VERIFIED |
| pio_python_deps.py installs Python build deps into pio env | tools/pio_python_deps.py:9-34 | VERIFIED |
