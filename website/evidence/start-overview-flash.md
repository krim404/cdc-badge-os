# Evidence ledger: start/overview.md + start/first-flash.md

| Claim | Source (path:line) | Tag |
|-------|--------------------|-----|
| Firmware is for the CDC Badge v1.0 hardware | components/cdc_hal/include/cdc_hal/hw_config.h:3 ("CDC Badge v1.0 Hardware Configuration") | VERIFIED |
| SoC is ESP32-S3 | boards/cdc-badge-usb.json:13 (`"mcu": "esp32s3"`), platformio.ini:8 (`platform = espressif32`), platformio.ini:17 (`toolchain-xtensa-esp32s3`) | VERIFIED |
| CPU runs at 240 MHz | boards/cdc-badge-usb.json:4 (`"f_cpu": "240000000L"`) | VERIFIED |
| Flash is 16 MB | platformio.ini:12 (`board_build.flash_size = 16MB`); boards/cdc-badge-usb.json:33 (`"flash_size": "16MB"`) | VERIFIED |
| PSRAM enabled, octal mode, 80 MHz | platformio.ini:13 (`board_build.psram = enabled`); sdkconfig.defaults:17-19 (CONFIG_SPIRAM=y, SPIRAM_MODE_OCT=y, SPIRAM_SPEED_80M=y) | VERIFIED |
| Exact PSRAM capacity not fixed in source | (absence) no PSRAM size constant found in platformio.ini / sdkconfig.defaults / boards/cdc-badge-usb.json | GAP |
| Secure element is TROPIC01 | components/cdc_hal/include/cdc_hal/hw_config.h:39 ("=== TROPIC01 Secure Element ==="); platformio.ini:31 (TROPIC01 pairing key) | VERIFIED |
| Display is e-paper GDEY029T94, 296 x 128, monochrome | components/cdc_hal/include/cdc_hal/hw_config.h:26 ("E-Paper GDEY029T94"); components/cdc_hal/src/EpaperDisplay.cpp:3 ("Gdey029T94 (296x128 B/W)"), :45 (WIDTH = 296), :46 (HEIGHT = 128) | VERIFIED |
| Display is 2.9" with frontlight | README.md:149 ("GDEY029T94-FL03 (2.9\" E-Paper + Frontlight)"); hw_config.h:28 (EPD_LED_PIN ... Backlight/Frontlight) | VERIFIED |
| 12-button keypad, phone-style T9 | components/cdc_hal/include/cdc_hal/IKeypad.h:9 ("12-button keypad"); README.md:36 ("12-Button Keypad ... Phone-style T9 input") | VERIFIED |
| Charger IC is BQ25895 | components/cdc_hal/include/cdc_hal/hw_config.h:21-22 ("Power / Charging (BQ25895)", BQ25895_ADDR 0x6A) | VERIFIED |
| I/O expander is TCA9535 (keypad matrix) | components/cdc_hal/include/cdc_hal/hw_config.h:17 ("IO Expander (TCA9535)"); README.md:151 ("I/O Expander | TCA9535 (Keypad)") | VERIFIED |
| SAO port and Grove port expansion | components/cdc_hal/include/cdc_hal/hw_config.h:46-53 (SAO Port, Grove Port) | VERIFIED |
| Second I2C bus on expansion header | components/cdc_hal/include/cdc_hal/hw_config.h:13-15 ("I2C1: Expansion header") | VERIFIED |
| Private keys stored in TROPIC01 ECC slots | main/tropic_slot_map.h:7 ("32 ECC slots"), :39-48 (ECC slot ranges per module) | VERIFIED |
| TOTP (2FA) capacity = 100 accounts | main/tropic_slot_map.h:57-58 (RMEM_SLOT_MOD_2FA_START 32 / END 131 -> 100 slots) | VERIFIED |
| Password vault capacity = 369 entries | main/tropic_slot_map.h:59-60 (RMEM_SLOT_MOD_PASSWORD_START 132 / END 500 -> 369 slots) | VERIFIED |
| Modules: FIDO2, 2FA(TOTP), password, GPG present | main/CMakeLists.txt MODULES list (mod_fido2, mod_2fa, mod_password, mod_gpg) | VERIFIED |
| GPG / OpenPGP smartcard over USB CCID (sign/encrypt/decrypt/SSH) | README.md:29 ("GPG/CCID ... OpenPGP smartcard via USB CCID, sign / encrypt / decrypt / SSH ... GnuPG"); main/CMakeLists.txt MODULES (mod_gpg) | VERIFIED |
| FIDO2/WebAuthn passkeys + U2F | main/CMakeLists.txt MODULES (mod_fido2); README.md feature table (U2F "Working") | VERIFIED |
| BLE HID acts as Bluetooth keyboard | main/CMakeLists.txt MODULES (mod_blehid); README.md:32 ("BLE HID ... Bluetooth keyboard for auto-type") | VERIFIED |
| USB CDC serial + HID | platformio.ini:25-28 (CONFIG_TINYUSB_ENABLED, CDC); main/CMakeLists.txt MODULES (mod_usbhid) | VERIFIED |
| WiFi NTP time sync via serial | components/serial_cmd/src/SerialCmd.cpp:2388-2398 (WIFI subcommands SCAN/STATUS/ON/CONNECT...); README.md:33 ("WiFi + NTP") | VERIFIED |
| BLE vCard hardware-verified 2026-06-18 (T-HIL03) | on-device test run | VERIFIED |
| BLE serial console (NUS) not yet hardware-verified | — | OPEN |
| WASM plugin runtime sandboxed (WAMR), separate 2 MB FAT partition | README.md:40 (WASM Plugin Runtime "Working"); partitions.csv:4 (plugins, data, fat, 0xDF0000, 0x200000); web-flasher/index.html:393 ("separate 2 MB FAT-FS partition ... WAMR runtime") | VERIFIED |
| English + German UI languages | README.md:38 ("Multi-Language ... English and German UI"); assets/i18n/lang_de.json (German overlay exists) | VERIFIED |
| Lock screen is PIN-protected | components/serial_cmd/src/SerialCmd.cpp:2436 (PIN command); existing page guide/lock-and-pin.md | VERIFIED |
| Display only refreshes on change (e-paper) | components/cdc_hal/src/EpaperDisplay.cpp:3 ("low-power"); memory note epaper_refresh_modes (hint) | VERIFIED |
| Target chip for flashing is ESP32-S3 | tools/flash_firmware.py:28 (CHIP = "esp32s3"); manifests chipFamily "ESP32-S3" (deploy-pages.yml:111, .gitlab-ci.yml:137) | VERIFIED |
| Bootloader entry via Tools menu | components/cdc_os_ui/src/AppUi.cpp:484-488 (kToolsFixed includes "core.expert") | VERIFIED |
| Bootloader entry via Expert submenu | components/cdc_os_ui/src/ExpertMenuUi.cpp:48 ({"core.bootloader", rebootIntoBootloader}) | VERIFIED |
| Tools / Expert / Bootloader UI labels | components/cdc_ui/src/I18n.cpp:43 ("core.tools" -> "Tools"), :188 ("core.expert" -> "Expert"), :190 ("core.bootloader" -> "Bootloader") | VERIFIED |
| Serial BOOTLOADER command exists, requires auth | components/serial_cmd/src/SerialCmd.cpp:2422 (registerCommand BOOTLOADER ... requiresAuth=true); :600-605 (cmdBootloader -> rebootIntoBootloader) | VERIFIED |
| `requiresAuth` field meaning | components/serial_cmd/include/serial_cmd/ICommandRegistry.h:36 ("Whether the command needs an authenticated session") | VERIFIED |
| AUTH command exists; serial session must authenticate first | components/serial_cmd/src/SerialCmd.cpp:2442 (registerCommand AUTH); :1007 ("Usage: AUTH <pin>"), :1503 ("Login with: AUTH <pin>") | VERIFIED |
| Hardware bootloader: hold FLASH, tap RESET | web-flasher/index.html:296-302 (Hardware: hold FLASH, press RESET, release FLASH) | VERIFIED |
| FLASH button is GPIO0 / power button | components/cdc_hal/include/cdc_hal/hw_config.h:43 (FLASH_BTN_PIN GPIO_NUM_0 // Flash/Power button) | VERIFIED |
| Bootloader mode: display not updating, looks off | web-flasher/index.html:304-310 (info alert) | VERIFIED |
| Web flasher uses ESP Web Tools | web-flasher/index.html:8-10 (esp-web-tools install-button.js) | VERIFIED |
| Web flasher requires Chrome/Edge, data USB cable, bootloader mode | web-flasher/index.html:400-404 (Requirements list) | VERIFIED |
| Firmware Update = manifest.json, keeps plugins/settings/PINs, erase unchecked by default | web-flasher/index.html:317-336, :356-364; manifest.json new_install_prompt_erase true (deploy-pages.yml:107) | VERIFIED |
| Factory Setup = manifest_factory.json, wipes plugins + reinstalls language overlay | web-flasher/index.html:338-352, :366-373 | VERIFIED |
| Erase checkbox does NOT reinstall language overlay | web-flasher/index.html:356-364 | VERIFIED |
| First boot after Factory Setup is longer (wipes SE slots, reinit NVS) | web-flasher/index.html:375-382 | VERIFIED |
| Python tool: chip esp32s3, 460800 baud, dio, 80m, 16MB | tools/flash_firmware.py:28-32 (CHIP/BAUD/FLASH_MODE/FLASH_FREQ/FLASH_SIZE) | VERIFIED |
| Python tool flash offsets 0x0/0x8000/0x50000 | tools/flash_firmware.py:36-40 (FLASH_MAP) | VERIFIED |
| app0 at 0x50000 not default 0x10000 | tools/flash_firmware.py:34-35 (comment); partitions.csv:3 (app0 ... 0x50000) | VERIFIED |
| Python tool: --release / --dir / --port / --erase-nvs flags | tools/flash_firmware.py:226-234 (argparse args) | VERIFIED |
| Python tool: auto port detect, hard_reset after | tools/flash_firmware.py:46-53 (find_port), :176-177 (--after hard_reset) | VERIFIED |
| Python tool: --erase-nvs erases 0x9000 / 0x5000 | tools/flash_firmware.py:193-200 (erase_region 0x9000 0x5000) | VERIFIED |
| Partition table offsets/sizes (nvs/app0/plugins/coredump) | partitions.csv:2-5 | VERIFIED |
| manifest.json builds: bootloader@0, partitions@32768, firmware@327680, chipFamily ESP32-S3, new_install_prompt_erase | .gitlab-ci.yml:130-146; .github/workflows/deploy-pages.yml:103-119 | VERIFIED |
| manifest_factory.json adds plugins_initial.bin@14614528 | .gitlab-ci.yml:147-164; .github/workflows/deploy-pages.yml:121-137 | VERIFIED |
| 32768 = 0x8000, 327680 = 0x50000, 14614528 = 0xDF0000 | (arithmetic over partitions.csv:2-4 offsets) | VERIFIED |
| Plugins/language overlay installable without re-flash | web-flasher/index.html:385-395 (step 3, optional plugin install) | VERIFIED |
| Pre-1.0, breaking changes / data loss on flash | README.md:7-12 (Beta - data loss on flash); web-flasher/index.html:366-373 | VERIFIED |
