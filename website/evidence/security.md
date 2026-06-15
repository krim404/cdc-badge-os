# Security evidence ledger

Every claim on the `/security/` pages traces to firmware source here. Format:

| Claim | Source (path:line) | Tag |
| --- | --- | --- |

## Secure element & key model

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| 32 ECC slots, slot 0 reserved | main/tropic_slot_map.h:18-20 | SE |
| 512 R-Memory slots, slot 0 reserved, min alloc slot 1 | main/tropic_slot_map.h:22-25 | SE |
| Interface constants: 32 ECC slots, 512 RMEM slots | components/cdc_hal/include/cdc_hal/ISecureElement.h:60-62 | SE |
| R-Memory min guaranteed slot size 444 bytes; runtime size queried via getRmemSlotSize() (>=444, <=475) | components/cdc_hal/include/cdc_hal/ISecureElement.h:63-69, 243-249 | SE |
| GPG ECC slots 1-3, RMEM 1-3 | main/tropic_slot_map.h:40-41, 51-52 | SLOTS |
| CA ECC slot 4, RMEM 4 | main/tropic_slot_map.h:42-43, 53-54 | SLOTS |
| FIDO2 ECC slots 5-30, RMEM 5-31 | main/tropic_slot_map.h:44-45, 55-56 | SLOTS |
| 2FA RMEM 32-131 (ECC none) | main/tropic_slot_map.h:57-58 | SLOTS |
| Password RMEM 132-500 (ECC none) | main/tropic_slot_map.h:59-60 | SLOTS |
| Plugin ECC slot 31, RMEM 501-511 | main/tropic_slot_map.h:46-48, 61-65 | SLOTS |
| System/attestation ECC slot 0, RMEM 0 reserved | main/tropic_slot_map.h:20, 24; components/cdc_core/include/cdc_core/PinManager.h:56, 61 | SLOTS |
| SE operations: eccGenerate, eccImport, eccGetPublicKey, eccDelete | components/cdc_hal/include/cdc_hal/ISecureElement.h:103-129 | SE-OPS |
| ECDSA P-256 sign (hashes internally, SHA-256); EdDSA Ed25519 sign | components/cdc_hal/include/cdc_hal/ISecureElement.h:133-153 | SE-OPS |
| Curves supported: P-256 (secp256r1) and Ed25519 | components/cdc_hal/include/cdc_hal/ISecureElement.h:12-15 | SE-OPS |
| Hardware TRNG: getRandom (with ESP32 fallback) and getRandomStrict (SE-only) | components/cdc_hal/include/cdc_hal/ISecureElement.h:210-229 | SE-OPS |
| Interface exposes no private-key read/export operation (only public key getter) | components/cdc_hal/include/cdc_hal/ISecureElement.h:96-129 | SE-NONEXPORT |
| Attestation private key generated on-chip in ECC slot 0; only public key read back | components/cdc_core/src/PinManager.cpp:126-135; website attestation page | ATTEST |
| Attestation key service: ensureKey generates/validates slot 0 | components/cdc_core/include/cdc_core/AttestationKeyService.h:24; (lifecycle described in attestation page) | ATTEST |
| FIDO2 keys generated on-chip via eccGenerate | components/mod_fido2/src/fido2_storage.cpp:816 | KEYGEN |
| GPG keys generated on-chip via eccGenerate | components/mod_gpg/src/gpg.cpp:62, 67; components/mod_gpg/src/openpgp/openpgp.cpp:86, 91 | KEYGEN |
| PIN payload signed by chip-bound slot-0 key; tamper/regen invalidates record -> reset to defaults | components/cdc_core/include/cdc_core/PinManager.h:58-61; components/cdc_core/src/PinManager.cpp:114-170, 197-201 | TAMPER |
| Alarm/tamper mode is a possible SE result code | components/cdc_hal/include/cdc_hal/ISecureElement.h:45 | TAMPER |

## Badge PIN & lockout

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Badge PIN 4-8 digits, digits only | components/cdc_core/include/cdc_core/PinManager.h:49-50; components/cdc_core/src/PinManager.cpp:559-569 | PIN |
| Badge PIN stored as LEFT(SHA256(PIN),16) | components/cdc_core/include/cdc_core/PinManager.h:64; components/cdc_core/src/PinManager.cpp:354-367 | PIN |
| Badge retry counter is RAM-only; R-Memory persists only a "locked" flag | components/cdc_core/include/cdc_core/PinManager.h:33-35, 174-177, 209-210 | PIN |
| MAX_RETRIES = 3 | components/cdc_core/include/cdc_core/PinManager.h:167 | PIN |
| On boot, 1 attempt granted (0 if locked), recovery timer started | components/cdc_core/src/PinManager.cpp:50-51 | PIN |
| Lockout duration 60000 ms (60 s), RAM-only | components/cdc_core/include/cdc_core/PinManager.h:92; components/cdc_core/src/PinManager.cpp:877-899 | PIN |
| On expiry retries restored to MAX_RETRIES, locked flag cleared | components/cdc_core/src/PinManager.cpp:919-930 | PIN |
| Wrong badge PIN reaching 0 sets locked flag, saves, starts lockout | components/cdc_core/src/PinManager.cpp:465-470 | PIN |
| Correct badge PIN restores retries to MAX_RETRIES, clears lockout | components/cdc_core/src/PinManager.cpp:454-462 | PIN |
| Constant-time-style hash comparison | components/cdc_core/src/PinManager.cpp:416-422 | PIN |
| Badge PIN never permanently bricks (no DEBUG_MODE gate in verify path; recovery is automatic) | components/cdc_core/src/PinManager.cpp:439-471, 919-930 | PIN |

## OpenPGP card PINs (PW1 user / PW3 admin)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| PW1 min 6, PW3 min 8, max 16 | components/cdc_core/include/cdc_core/PinManager.h:51-53; components/cdc_core/src/PinManager.cpp:737, 816 | PGP |
| PW1/PW3 hashed with iterated+salted S2K SHA-256, default 100000 iterations | components/cdc_core/include/cdc_core/PinManager.h:68-71; components/cdc_core/src/PinManager.cpp:376-407 | PGP |
| PW1/PW3 smartcard semantics: pre-decrement persisted synchronously, zero is terminal until admin reset | components/cdc_core/include/cdc_core/PinManager.h:37-39; components/cdc_core/src/PinManager.cpp:474-517 | PGP |
| PW1/PW3 MAX_RETRIES = 3 | components/cdc_core/include/cdc_core/PinManager.h:167; components/cdc_core/src/PinManager.cpp:745, 823 | PGP |
| Blocked PW1/PW3 = retries == 0 | components/cdc_core/include/cdc_core/PinManager.h:112, 122 | PGP |
| RESET RETRY COUNTER (INS 0x2C): admin (PW3) resets PW1; Resetting Code path resets PW1 | components/mod_gpg/src/openpgp/openpgp.cpp:2256-2339 | PGP |

## Duress / self-destruct

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Duress PIN optional, default disarmed | components/cdc_core/include/cdc_core/PinManager.h:125-146, 190; components/cdc_core/src/PinManager.cpp:79-82 | DURESS |
| Duress PIN 4-8 digits, digits only, KDF-hashed | components/cdc_core/src/PinManager.cpp:641-663 | DURESS |
| Duress PIN must differ from badge PIN | components/cdc_core/include/cdc_core/PinManager.h:131-137; components/cdc_core/src/PinManager.cpp:653-660 | DURESS |
| Setting badge PIN equal to duress PIN is also rejected | components/cdc_core/src/PinManager.cpp:571-574 | DURESS |
| Duress fields covered by the same slot-0 attestation signature | components/cdc_core/include/cdc_core/PinManager.h:28-30, 188-192 | DURESS |
| Duress check runs before badge verify on unlock; match calls selfDestruct() | components/cdc_os_ui/src/AppUi.cpp:355-365 | DURESS |
| selfDestruct() erases boot-profile marker, commits, reboots (does not wipe directly) | components/cdc_core/src/FactoryReset.cpp:60-68; components/cdc_core/include/cdc_core/FactoryReset.h:39-53 | DURESS |
| Absent boot-profile marker on next boot triggers full wipe (NVS + all TROPIC01 ECC/RMEM) | main/main.cpp:176-230, 240-250, 602, 619-621 | DURESS |
| wipeTropic erases every ECC slot 0..31 and every RMEM slot 0..511 | components/cdc_core/src/FactoryReset.cpp:11-44 | DURESS |
| wipeNvs erases and re-inits the NVS partition | components/cdc_core/src/FactoryReset.cpp:46-58 | DURESS |
| Duress setup entry "Set duress PIN" in Expert menu | components/cdc_os_ui/src/ExpertMenuUi.cpp:43 ("core.set_duress_pin", showDuressPinSetup) | DURESS |
| Expert menu reached from Tools | components/cdc_os_ui/src/AppUi.cpp:488 ("core.expert" -> showExpertMenu); README.md:40 | DURESS |
| Duress setup wizard re-auths with badge PIN, then enters/confirms duress PIN | components/cdc_os_ui/src/AppUi.cpp:378-392, 955-961 | DURESS |
| Self-destruct entry is indistinguishable from a normal PIN attempt (no UI/log/timing tell) | components/cdc_os_ui/src/AppUi.cpp:357-363 | DURESS |

## Beta status, no-migration, DEBUG_MODE

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Pre-1.0 beta, data loss on flash (FIDO2, TOTP, vault, GPG, PIN) | README.md:7-18 | BETA |
| No-migration policy: breaking changes wipe + reinit, no version-detect fallbacks | /Users/krim/GIT/cdc-badge-os/CLAUDE.md "NO MIGRATION CODE" section | BETA |
| Build-profile byte mismatch triggers full factory wipe at next boot | components/cdc_core/include/cdc_core/feature_flags.h:43-50; main/main.cpp:176-230 | BETA |
| WIP/untested: BLE vCard, BLE Serial untested on hardware; GPG UI WIP | README.md:29-33 | WIP |
| DEBUG_MODE default 1 | components/cdc_core/include/cdc_core/feature_flags.h:39-41 | DEBUG |
| DEBUG_MODE folds into BUILD_PROFILE_BYTE (bit 0) | components/cdc_core/include/cdc_core/feature_flags.h:49-50 | DEBUG |
| DEBUG_MODE sets log level DEBUG vs WARN | components/cdc_log/src/cdc_log.cpp:140-147 | DEBUG |
| Release build swallows hex dumps (key-material leak guard) | components/cdc_log/src/cdc_log.cpp:225-228 | DEBUG |
| Release gates INFO/DEBUG serial logs behind auth; raises/lowers level on auth | components/serial_cmd/src/SerialCmd.cpp:1486-1491, 1751-1753, 1794-1796, 1818-1820 | DEBUG |
| DEBUG_MODE logs ECDH/crypto intermediate values in FIDO2 PIN path | components/mod_fido2/src/ctap2.cpp:2142-2152 | DEBUG |
| DEBUG_MODE logs main-task stack low-water | main/main.cpp:583-590 | DEBUG |
| Build-profile byte is the documented beta-phase software guard; Secure Boot v2 + anti-rollback is 1.0 roadmap | components/cdc_core/include/cdc_core/feature_flags.h:43-50 | DEBUG |
