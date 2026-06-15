# Evidence ledger: Encrypted backup & restore

Tags: VERIFIED = exact source line supports the claim; GAP = not verifiable in source.

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Backup lives in the menu under Tools | components/cdc_os_ui/src/AppUi.cpp:484-489 (`kToolsFixed[]` includes `core.expert`) | VERIFIED |
| Expert menu entry opens the Backup submenu | components/cdc_os_ui/src/ExpertMenuUi.cpp:42 (`{"core.backup", showBackupMenu}` in `kExpertTop[]`) | VERIFIED |
| Backup submenu has Export / Import / Delete | components/cdc_os_ui/src/BackupMenuUi.cpp:208-211 | VERIFIED |
| Export prompts for passphrase twice with a match check | components/cdc_os_ui/src/BackupMenuUi.cpp:52-81 (`onExportFirst` -> `onExportConfirm` strcmp) | VERIFIED |
| Passphrase max length 64 | components/cdc_os_ui/src/BackupMenuUi.cpp:29 (`PASSPHRASE_MAX = 64`) | VERIFIED |
| Empty passphrase rejected | components/cdc_os_ui/src/BackupManager.cpp:249-252; BackupMenuUi.cpp:68-71 | VERIFIED |
| Import shows per-section summary (imported/failed/modules/skipped/system) | components/cdc_os_ui/src/BackupMenuUi.cpp:100-109 | VERIFIED |
| Restore summary text states "No keys are backed up (FIDO2/GPG); only data." | components/cdc_ui/src/I18n.cpp:216 (`core.backup_scope_info`) | VERIFIED |
| Delete asks for confirmation (WARNING icon) | components/cdc_os_ui/src/BackupMenuUi.cpp:128-135 | VERIFIED |
| On-device file is `<plugins>/backup.cdcbak` | components/cdc_os_ui/src/BackupManager.cpp:66-73 (`backupPath()` -> `%s/backup.cdcbak` of `PluginStorage::basePath()`) | VERIFIED |
| File on disk is base64-encoded container | components/cdc_os_ui/src/BackupManager.cpp:317-320 (b64Encode then fwrite) | VERIFIED |
| Magic string is "CDCBAK" (6 bytes) | components/cdc_os_ui/src/BackupManager.cpp:42-43 (`MAGIC[] = {'C','D','C','B','A','K'}`) | VERIFIED |
| Container version byte == 1 | components/cdc_os_ui/src/BackupManager.cpp:44 (`CONTAINER_VERSION = 1`) | VERIFIED |
| Salt size 16 bytes | components/cdc_os_ui/src/BackupManager.cpp:45 (`SALT_SIZE = 16`) | VERIFIED |
| Nonce size 12 bytes | components/cdc_os_ui/src/BackupManager.cpp:46 (`NONCE_SIZE = 12`) | VERIFIED |
| GCM tag size 16 bytes | components/cdc_os_ui/src/BackupManager.cpp:47 (`TAG_SIZE = 16`) | VERIFIED |
| Key size 32 bytes (AES-256) | components/cdc_os_ui/src/BackupManager.cpp:48 (`KEY_SIZE = 32`) | VERIFIED |
| KDF iterations 200000 | components/cdc_os_ui/src/BackupManager.cpp:49 (`KDF_ITERATIONS = 200000`) | VERIFIED |
| KDF iterations stored in header as uint32 little-endian | components/cdc_os_ui/src/BackupManager.cpp:51-53, 159 (`putU32le(p_iters, KDF_ITERATIONS)`); 76-81 | VERIFIED |
| Header layout: magic\|\|version\|\|kdf_iters(LE)\|\|salt\|\|nonce | components/cdc_os_ui/src/BackupManager.cpp:51-53, 149-161 | VERIFIED |
| KDF is PBKDF2-HMAC-SHA256 | components/cdc_os_ui/src/BackupManager.cpp:127-135 (`mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, ...)`) | VERIFIED |
| Cipher is AES-256-GCM | components/cdc_core/include/cdc_core/Crypto.h:54,85 (`mbedtls_gcm_setkey(..., MBEDTLS_CIPHER_ID_AES, key, 256)`); BackupManager.cpp:169-173,219-223 | VERIFIED |
| Whole header used as GCM AAD | components/cdc_os_ui/src/BackupManager.cpp:51-52, 170-171 (AAD = `out, HEADER_SIZE`); 221 | VERIFIED |
| Container body is `ciphertext(N) \|\| gcm_tag(16)` | components/cdc_os_ui/src/BackupManager.cpp:146, 154-155 | VERIFIED |
| Wrong passphrase fails via GCM tag mismatch | components/cdc_os_ui/src/BackupManager.cpp:219-229 (`aesGcm256Open` false -> log "wrong passphrase?") | VERIFIED |
| Plaintext is one JSON document | components/cdc_os_ui/src/BackupManager.cpp:266-301 (`cJSON_CreateObject` ... `cJSON_PrintUnformatted`) | VERIFIED |
| Top-level JSON keys: host_api_level, fw_version, modules, system | components/cdc_os_ui/src/BackupManager.cpp:269-271, 294-296 | VERIFIED |
| host_api_level value is the build's HOST_API_LEVEL_STR ("0.7") | components/cdc_os_ui/src/BackupManager.cpp:269; plugin_manager/include/plugin_manager/host_api.h:30 | VERIFIED |
| fw_version value is APP_VERSION ("0.6.4") | components/cdc_os_ui/src/BackupManager.cpp:270; platformio.ini:23 | VERIFIED |
| Each module section keyed by module getName() | components/cdc_os_ui/src/BackupManager.cpp:283-284 (`cJSON_AddItemToObject(modules, m->getName(), section)`) | VERIFIED |
| Modules that do not implement exportBackup are absent | components/cdc_core/include/cdc_core/IModule.h:100 (default returns false); BackupManager.cpp:283-288 | VERIFIED |
| Only mod_2fa, mod_password, mod_vcard implement exportBackup/importBackup | grep across components/ (only those three .cpp define the overrides) | VERIFIED |
| 2FA section key is "mod_2fa" | components/mod_2fa/include/mod_2fa/TwoFaModule.h:32 (`ModuleBase("mod_2fa")`) | VERIFIED |
| Password section key is "mod_password" | components/mod_password/include/mod_password/PasswordModule.h:25 (`ModuleBase("mod_password")`) | VERIFIED |
| vCard section key is "mod_vcard" | components/mod_vcard/include/mod_vcard/VcardModule.h:13 (`getName() -> "mod_vcard"`) | VERIFIED |
| 2FA section: schema_ver + entries[] (name, issuer, type, algorithm, digits, period, counter, flags, secret) | components/mod_2fa/src/TwoFaModule.cpp:1389-1424 | VERIFIED |
| 2FA secret exported as Base32 string | components/mod_2fa/src/TwoFaModule.cpp:1408-1409, 1424 | VERIFIED |
| Password section: schema_ver + entries[] (title, username, password, url, notes, totp_slot) | components/mod_password/src/PasswordModule.cpp:944-971 | VERIFIED |
| Password plaintext exported in the (encrypted) container | components/mod_password/src/PasswordModule.cpp:968 (`cJSON_AddStringToObject(obj, "password", entry.password)`) | VERIFIED |
| vCard section: schema_ver + own (string) + received[] (strings) | components/mod_vcard/src/VcardModule.cpp:577-598 | VERIFIED |
| System section: schema_ver, language, backlight, sleep_interval, tz_offset, badge_name/info/info2, wifi{...}, wifi_timeout, wifi_enabled, modules_enabled{} | components/cdc_os_ui/src/SystemSettingsBackup.cpp:80-138 | VERIFIED |
| WiFi credentials (ssid, pass) included in the encrypted container | components/cdc_os_ui/src/SystemSettingsBackup.cpp:106-116 | VERIFIED |
| FIDO2/GPG modules do NOT implement backup (no secure-element key export) | no exportBackup/importBackup in components/mod_fido2 or components/mod_gpg (grep) | VERIFIED |
| Modules write semantic records, never raw slot/NVS blobs | components/cdc_core/include/cdc_core/IModule.h:91-99 | VERIFIED |
| Restore is gated on host_api_level (rejects backups newer than this build) | components/cdc_os_ui/src/BackupManager.cpp:380-385 | VERIFIED |
| Unknown module sections are skipped and counted | components/cdc_os_ui/src/BackupManager.cpp:391-396 (`summary.skipped++`) | VERIFIED |
| Restore is best-effort: per-module failures never abort | components/cdc_core/include/cdc_core/IModule.h:82-83, 102-107; BackupManager.cpp:397-401 | VERIFIED |
| Import overwrites entries with the same identity (name/title/text) | mod_2fa/src/TwoFaModule.cpp:1439-1448; mod_password/src/PasswordModule.cpp:986-997; mod_vcard/src/VcardModule.cpp:628-632 | VERIFIED |
| Restore applies many settings live; some (badge text, WiFi connect intent) take effect at next boot | components/cdc_os_ui/src/SystemSettingsBackup.cpp:191-194, 196-199 | VERIFIED |
| Plaintext is zeroized in PSRAM after use | components/cdc_os_ui/src/BackupManager.cpp:334-338, 421 (`mbedtls_platform_zeroize`) | VERIFIED |
| Derived key zeroized after seal/open | components/cdc_os_ui/src/BackupManager.cpp:174, 224 | VERIFIED |
| Max plaintext payload 256 KiB | components/cdc_os_ui/src/BackupManager.cpp:57 (`MAX_PAYLOAD = 256 * 1024`) | VERIFIED |
| Serial command BACKUP EXPORT/IMPORT/DELETE is AUTH-gated | components/cdc_os_ui/src/BackupMenuUi.cpp:218-222 (registerCommand requireAuth=true) | VERIFIED |
| Serial IMPORT prints "FIDO2/GPG signing keys are NOT restored." | components/cdc_os_ui/src/BackupMenuUi.cpp:172 | VERIFIED |
| tools/backup.py modes: --export/--import/--delete/--upload/--download/--decrypt/--encrypt | tools/backup.py:337-345 | VERIFIED |
| tools/backup.py reproduces byte-identical container format | tools/backup.py:9-25, 65-72, 79-124 | VERIFIED |
| tools/backup.py off-device decrypt/encrypt only needs the passphrase | tools/backup.py:101-124, 84-98 | VERIFIED |
| tools/backup.py serial transfer uses VFAT shell (RECEIVE / GET) | tools/backup.py:216-237, 281-307 | VERIFIED |
| On-device device file name is backup.cdcbak | tools/backup.py:74 (`DEVICE_FILE = "backup.cdcbak"`) | VERIFIED |
