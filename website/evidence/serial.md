# Evidence: Serial console + serial command reference

| Claim | Source (path:line) | Tag |
|-------|--------------------|-----|
| Serial command processor + line editing + dispatch live in serial_cmd | components/serial_cmd/src/SerialCmd.cpp:1 | infra |
| Connect at 115200 baud (documented baud) | docs/SERIAL_COMMANDS.md:5 (stale hint) + Console init in components/serial_cmd/src/Console.cpp | baud |
| Console banner "=== CDC Badge OS Serial Console ===" printed on init | components/serial_cmd/src/SerialCmd.cpp:1501 | banner |
| Banner shows "Login with: AUTH <pin>" only when FEATURE_SECURE_SERIAL | components/serial_cmd/src/SerialCmd.cpp:1502-1504 | banner |
| FEATURE_SECURE_SERIAL default = 1 | components/cdc_core/include/cdc_core/feature_flags.h:19 | flag |
| FEATURE_SECURE_SERIAL fallback 0 if undefined | components/cdc_core/include/cdc_core/feature_flags.h:21-22 | flag |
| Kconfig CONFIG_SECURE_SERIAL default y | components/serial_cmd/Kconfig.projbuild | flag |
| When secure serial on: only PING and AUTH allowed without auth | components/serial_cmd/src/CommandRegistry.cpp:140-147 | auth |
| When PIN blocked (lockout/retries 0): only PING allowed | components/serial_cmd/src/CommandRegistry.cpp:127-138 | auth |
| Per-command requiresAuth flag gates even when secure serial off | components/serial_cmd/src/CommandRegistry.cpp:156-159 | auth |
| Auth check is per top-level command (subcommands inherit) | components/serial_cmd/src/CommandRegistry.cpp:152-159 | auth |
| AUTH timeout = 5 minutes | components/serial_cmd/include/serial_cmd/SerialCmd.h:26 | auth |
| Session times out, isAuthenticated re-checks timestamp | components/serial_cmd/src/SerialCmd.cpp:1744-1762 | auth |
| Auth timer reset after each successful command | components/serial_cmd/src/SerialCmd.cpp:1484-1485, CommandRegistry.cpp:164-166 | auth |
| Wrong PIN drops active session | components/serial_cmd/src/SerialCmd.cpp:1016-1020 | auth |
| AUTH with no arg = logout (or usage if not logged in) | components/serial_cmd/src/SerialCmd.cpp:1001-1011 | auth |
| AUTH/LOGOUT only registered when FEATURE_SECURE_SERIAL | components/serial_cmd/src/SerialCmd.cpp:2441-2444 | auth |
| Release build gates INFO/DEBUG logs until authenticated | components/serial_cmd/src/SerialCmd.cpp:1486-1491 | auth |
| HELP prints grouped commands incl. subcommand listings | components/serial_cmd/src/CommandRegistry.cpp:180-213 | help |
| Bare group name or "<GROUP> HELP"/"?" prints subcommand help | components/serial_cmd/include/serial_cmd/SubCommand.h:82-86 | help |
| Unknown subcommand -> error + help | components/serial_cmd/include/serial_cmd/SubCommand.h:95-97 | help |
| Command history (up/down arrows), 10 entries | components/serial_cmd/src/SerialCmd.cpp:47, 1518-1540, 1553-1579 | edit |
| Line editing: backspace, Ctrl+C, Ctrl+U | components/serial_cmd/src/SerialCmd.cpp:1621-1641 | edit |
| UTF-8 input converted to CP437 on the fly | components/serial_cmd/src/SerialCmd.cpp:1644-1689 | edit |
| Byte interceptor: raw binary streaming bypasses line parsing (uploads) | components/serial_cmd/src/SerialCmd.cpp:1594-1600, ICommandRegistry.h:107-123 | upload |
| MAX_COMMANDS = 64 | components/serial_cmd/src/CommandRegistry.cpp:22 | infra |
| Builtin command registration table | components/serial_cmd/src/SerialCmd.cpp:2411-2451 | infra |
| HELP open, no auth | components/serial_cmd/src/SerialCmd.cpp:2414 | cmd |
| PING open, no auth | components/serial_cmd/src/SerialCmd.cpp:2415, CommandRegistry.cpp:142 | cmd |
| STATUS open (free heap, uptime) | components/serial_cmd/src/SerialCmd.cpp:2416, 442-449 | cmd |
| MEM open (heap/PSRAM) | components/serial_cmd/src/SerialCmd.cpp:2417, 553-582 | cmd |
| MEMINFO open (detailed heap + tasks) | components/serial_cmd/src/SerialCmd.cpp:2418, 481-551 | cmd |
| CPU open (~250 ms load measure) | components/serial_cmd/src/SerialCmd.cpp:2419, 451-458 | cmd |
| ERROR_LOG open (CLEAR arg resets) | components/serial_cmd/src/SerialCmd.cpp:2420, 626-633 | cmd |
| REBOOT requiresAuth=true | components/serial_cmd/src/SerialCmd.cpp:2421, 588-594 | cmd |
| BOOTLOADER requiresAuth=true (USB download mode) | components/serial_cmd/src/SerialCmd.cpp:2422, 600-605 | cmd |
| PASTE requiresAuth=true (into active T9 input) | components/serial_cmd/src/SerialCmd.cpp:2423, 607-620 | cmd |
| NVS group requiresAuth=true | components/serial_cmd/src/SerialCmd.cpp:2425 | cmd |
| NVS subcommands LIST/READ/DEL/CLEAR | components/serial_cmd/src/SerialCmd.cpp:2353-2358 | cmd |
| NVS CLEAR requires YES confirmation | components/serial_cmd/src/SerialCmd.cpp:643-661 | cmd |
| NVS DEL erases namespace if key omitted | components/serial_cmd/src/SerialCmd.cpp:755-799 | cmd |
| GET_TIME/GET_DATE open | components/serial_cmd/src/SerialCmd.cpp:2427-2428 | cmd |
| SET_TIME open (HH:MM:SS) | components/serial_cmd/src/SerialCmd.cpp:2429, 839-871 | cmd |
| SET_DATE open (DD.MM.YYYY or Unix seconds) | components/serial_cmd/src/SerialCmd.cpp:2430, 877-937 | cmd |
| SET_NAME/SET_INFO/SET_INFO2 open | components/serial_cmd/src/SerialCmd.cpp:2432-2434 | cmd |
| SET_NAME etc. wired via setTextCallback | components/cdc_os_ui/src/AppUi.cpp:995-999 | cmd |
| PIN group requiresAuth=true | components/serial_cmd/src/SerialCmd.cpp:2436 | cmd |
| PIN subcommands STATUS/RESET/CHANGE/DURESS/DURESS_CLEAR | components/serial_cmd/src/SerialCmd.cpp:2362-2369 | cmd |
| PIN CHANGE <currentPin> <newPin> (4-8 digits) | components/serial_cmd/src/SerialCmd.cpp:1078-1136 | cmd |
| PIN DURESS arms self-destruct PIN | components/serial_cmd/src/SerialCmd.cpp:1146-1169 | cmd |
| TR01 group requiresAuth=true | components/serial_cmd/src/SerialCmd.cpp:2438-2439 | cmd |
| TR01 subcommands table | components/serial_cmd/src/SerialCmd.cpp:2372-2385 | cmd |
| TR01 WIPE requires CONFIRM | components/serial_cmd/src/SerialCmd.cpp:1431-1441 | cmd |
| WIFI group requiresAuth=true | components/serial_cmd/src/SerialCmd.cpp:2446-2447 | cmd |
| WIFI subcommands SCAN/STATUS/ON/OFF/CONNECT/TIMEOUT/FORGET | components/serial_cmd/src/SerialCmd.cpp:2388-2397 | cmd |
| WIFI ON modes [sta\|ap\|sta_ap] | components/serial_cmd/src/SerialCmd.cpp:2055-2067 | cmd |
| WIFI TIMEOUT bounds 3000-60000 ms | components/serial_cmd/src/SerialCmd.cpp:2189-2192 (kWifiSubs:2394) | cmd |
| MODULE group requiresAuth=true | components/serial_cmd/src/SerialCmd.cpp:2449-2450 | cmd |
| MODULE subcommands LIST/ENABLE/DISABLE | components/serial_cmd/src/SerialCmd.cpp:2400-2404 | cmd |
| GPG group requiresAuth=true, module "gpg" | components/mod_gpg/src/GpgModule.cpp:117-119 | cmd |
| GPG subcommands table | components/mod_gpg/src/GpgModule.cpp:93-104 | cmd |
| GPG RESET two-step token | components/mod_gpg/src/GpgModule.cpp:97 | cmd |
| GPG registerCommands called in module init | components/mod_gpg/src/GpgModule.cpp:918 | cmd |
| TOTP group requiresAuth=true, module "totp" | components/mod_2fa/src/TwoFaModule.cpp:431-433 | cmd |
| TOTP subcommands LIST/ADD/DEL/GET | components/mod_2fa/src/TwoFaModule.cpp:413-418 | cmd |
| TOTP ADD first arg is type totp\|hotp | components/mod_2fa/src/TwoFaModule.cpp:415, usage 2 | cmd |
| TOTP ADD full args incl. counter (for hotp) | components/mod_2fa/src/TwoFaModule.cpp:415 | cmd |
| CHALRESP requiresAuth=true (no subcommands) | components/mod_2fa/src/TwoFaModule.cpp:434-436 | cmd |
| CHALRESP <name> <hex-challenge> | components/mod_2fa/src/TwoFaModule.cpp:365-372 | cmd |
| 2FA registerCommands called in init | components/mod_2fa/src/TwoFaModule.cpp:1239 | cmd |
| PASSWORD group requiresAuth=true, module "password" | components/mod_password/src/PasswordModule.cpp:417-419 | cmd |
| PASSWORD subcommands LIST/GET/ADD/EDIT/DEL | components/mod_password/src/PasswordModule.cpp:396-402 | cmd |
| PASSWORD registerCommands called in init | components/mod_password/src/PasswordModule.cpp:857 | cmd |
| VCARD requiresAuth=FALSE (open), module "vcard" | components/mod_vcard/src/VcardModule.cpp:450-452 | cmd |
| VCARD subcommands SET/GET/DELETE | components/mod_vcard/src/VcardModule.cpp:434-438 | cmd |
| VFAT group requiresAuth=true, module "vfat" | components/mod_vfat/src/VfatModule.cpp:225-228 | cmd |
| VFAT subcommands table | components/mod_vfat/src/VfatModule.cpp:183-195 | cmd |
| VFAT registered in module init | components/mod_vfat/src/VfatModule.cpp:225 | cmd |
| PLUGIN group requiresAuth=true, module "plugin_manager" | components/plugin_manager/src/PluginSerialCommands.cpp:603-605 | cmd |
| PLUGIN subcommands table (incl. ENABLE/DISABLE/UPLOAD_AOT) | components/plugin_manager/src/PluginSerialCommands.cpp:555-571 | cmd |
| LANG group requiresAuth=true, subcommands INFO/RELOAD | components/plugin_manager/src/PluginSerialCommands.cpp:576-583, 606-608 | cmd |
| PLUGIN+LANG registered via registerPluginSerialCommands in main | main/main.cpp:538 | cmd |
| Upload protocol: READY then raw bytes, CRC32 verified | components/plugin_manager/src/PluginSerialCommands.cpp:587-598 (byte interceptor) | upload |
| BACKUP group requiresAuth=true, module "backup" | components/cdc_os_ui/src/BackupMenuUi.cpp:218-222 | cmd |
| BACKUP subcommands EXPORT/IMPORT/DELETE (<passphrase>) | components/cdc_os_ui/src/BackupMenuUi.cpp:187-192 | cmd |
| BACKUP registered via registerBackupSerialCommand in AppUi init | components/cdc_os_ui/src/AppUi.cpp:994 | cmd |
| GPIO/ADC/I2C/SAO registration function exists | components/plugin_manager/src/GpioSerialCommands.cpp:302-309 | gap |
| registerGpioSerialCommands() never called anywhere (commands not active) | grep components/ main/: only decl (GpioSerialCommands.h:13) + def (GpioSerialCommands.cpp:302) | gap |
| Modules shipped (MODULES list) | main/CMakeLists.txt:8-21 | infra |
| SerialCmd::init called at boot | main/main.cpp:446 | infra |
