# Evidence ledger: dev/secure-element.md + dev/api-reference.md

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| ECC slot count = 32, index 0-31 | components/cdc_hal/include/cdc_hal/ISecureElement.h:61 ; main/tropic_slot_map.h:18-19 | OK |
| R-Memory slot count = 512, index 0-511 | components/cdc_hal/include/cdc_hal/ISecureElement.h:62 ; main/tropic_slot_map.h:22-23 | OK |
| ECC slot 0 reserved | main/tropic_slot_map.h:20 | OK |
| R-Memory slot 0 reserved | main/tropic_slot_map.h:24 | OK |
| Lowest allocatable R-Memory slot = 1 | main/tropic_slot_map.h:25 | OK |
| RMEM_SLOT_SIZE = 444 (minimum guaranteed) | components/cdc_hal/include/cdc_hal/ISecureElement.h:67 | OK |
| RMEM_SLOT_SIZE_MAX = 475 (largest known FW) | components/cdc_hal/include/cdc_hal/ISecureElement.h:68-69 | OK |
| getRmemSlotSize() runtime, >=444 and <=475 | components/cdc_hal/include/cdc_hal/ISecureElement.h:244-249 | OK |
| RMEM_NAME_LEN = 16 | components/cdc_hal/include/cdc_hal/ISecureElement.h:70 | OK |
| Class comment says "512 slots, 476 bytes each" (nuance) | components/cdc_hal/include/cdc_hal/ISecureElement.h:55 | NUANCE |
| rmemWrite comment "max 476 bytes" (nuance) | components/cdc_hal/include/cdc_hal/ISecureElement.h:171 | NUANCE |
| Module ID SYSTEM=0, GPG=2, CA=3, FIDO2=4, 2FA=5, PASSWORD=6, PLUGIN_POOL=7, UNKNOWN=255 | main/tropic_slot_map.h:30-37 | OK |
| ECC: GPG 1-3 | main/tropic_slot_map.h:40-41 | OK |
| ECC: CA 4-4 | main/tropic_slot_map.h:42-43 | OK |
| ECC: FIDO2 5-30 | main/tropic_slot_map.h:44-45 | OK |
| ECC: Plugins 31-31 (last physical slot) | main/tropic_slot_map.h:46-48 | OK |
| RMEM: GPG 1-3 | main/tropic_slot_map.h:51-52 | OK |
| RMEM: CA 4-4 | main/tropic_slot_map.h:53-54 | OK |
| RMEM: FIDO2 5-31 | main/tropic_slot_map.h:55-56 | OK |
| RMEM: 2FA 32-131 | main/tropic_slot_map.h:57-58 | OK |
| RMEM: Password 132-500 | main/tropic_slot_map.h:59-60 | OK |
| RMEM: Plugins 501-511 (dynamic sub-slots, central bounds check) | main/tropic_slot_map.h:61-65 | OK |
| 2FA + Password are R-Memory only (absent from ECC map) | main/tropic_slot_map.h:68-72 (ECC map) vs 74-80 (RMEM map) | OK |
| Module IDs permanent, never reassigned | main/tropic_slot_map.h:27-29 | OK |
| Curves: P256 (secp256r1), Ed25519 | components/cdc_hal/include/cdc_hal/ISecureElement.h:12-15 | OK |
| curveByte: Ed25519=0, P256=1 | components/cdc_hal/include/cdc_hal/ISecureElement.h:22-24 | OK |
| curveFromByte: 0=Ed25519 else P256 | components/cdc_hal/include/cdc_hal/ISecureElement.h:31-33 | OK |
| SeResult enum members | components/cdc_hal/include/cdc_hal/ISecureElement.h:38-47 | OK |
| sessionStart/End/isSessionActive/sleep | components/cdc_hal/include/cdc_hal/ISecureElement.h:79,84,89,94 | OK |
| eccGenerate(slot, curve) | components/cdc_hal/include/cdc_hal/ISecureElement.h:103 | OK |
| eccImport(slot, privKey 32B, curve) | components/cdc_hal/include/cdc_hal/ISecureElement.h:111 | OK |
| eccGetPublicKey(slot, pubKey, curve*) - 65B P256 / 32B Ed25519 | components/cdc_hal/include/cdc_hal/ISecureElement.h:117-119 | OK |
| eccDelete(slot) | components/cdc_hal/include/cdc_hal/ISecureElement.h:124 | OK |
| eccSlotUsed(slot) const | components/cdc_hal/include/cdc_hal/ISecureElement.h:129 | OK |
| No private-key read/export call (only public read-back) | components/cdc_hal/include/cdc_hal/ISecureElement.h:96-129 (no export method present) | OK |
| ecdsaSign P-256, internal SHA-256, raw R\|\|S 64B, no pre-hash | components/cdc_hal/include/cdc_hal/ISecureElement.h:133-143 | OK |
| eddsaSign Ed25519, 64B | components/cdc_hal/include/cdc_hal/ISecureElement.h:145-153 | OK |
| rmemRead/Write/Erase/SlotUsed | components/cdc_hal/include/cdc_hal/ISecureElement.h:164-165,173,178,183 | OK |
| RMemHeader packed fields (magic, checksum, moduleId, flags, name[16], payloadLen) | components/cdc_hal/include/cdc_hal/ISecureElement.h:187-194 | OK |
| rmemWriteWithHeader / rmemReadWithHeader | components/cdc_hal/include/cdc_hal/ISecureElement.h:199-201,206-208 | OK |
| getRandom: TRNG with ESP32 fallback, WARN on fallback | components/cdc_hal/include/cdc_hal/ISecureElement.h:212-219 | OK |
| getRandomStrict: SE TRNG only, no fallback | components/cdc_hal/include/cdc_hal/ISecureElement.h:221-229 | OK |
| getChipId(serialNum, size) | components/cdc_hal/include/cdc_hal/ISecureElement.h:236 | OK |
| getFwVersion(riscvVer[4], spectVer[4]) - index 3 major..0 build | components/cdc_hal/include/cdc_hal/ISecureElement.h:239-242 | OK |
| getRmemSlotSize() | components/cdc_hal/include/cdc_hal/ISecureElement.h:249 | OK |
| Factory getSecureElementInstance() | components/cdc_hal/include/cdc_hal/ISecureElement.h:253 | OK |
| ALARM_MODE = tamper detected | components/cdc_hal/include/cdc_hal/ISecureElement.h:45 | OK |
| Doxygen PROJECT_NAME "CDC Badge OS" | Doxyfile:3 | OK |
| Doxygen PROJECT_BRIEF | Doxyfile:4 | OK |
| Doxygen OUTPUT_DIRECTORY doxygen_output | Doxyfile:6 | OK |
| Doxygen HTML_OUTPUT html (so doxygen_output/html) | Doxyfile:49 | OK |
| Doxygen RECURSIVE YES | Doxyfile:28 | OK |
| Doxygen FILE_PATTERNS *.h *.hpp *.cpp *.c | Doxyfile:29 | OK |
| Doxygen GENERATE_HTML YES, LATEX/XML NO | Doxyfile:46-48 | OK |
| Doxygen INPUT dirs (components + main) | Doxyfile:9-27 | OK |
| Doxygen EXCLUDE_PATTERNS | Doxyfile:32 | OK |
| Doxygen EXTRACT_ALL/STATIC YES, PRIVATE NO | Doxyfile:35-37 | OK |
| Doxygen SOURCE_BROWSER YES | Doxyfile:40 | OK |
| Doxygen preprocessing (ENABLE/MACRO_EXPANSION, PREDEFINED) | Doxyfile:63-70 | OK |
| Doxygen HAVE_DOT NO, CLASS_DIAGRAMS YES | Doxyfile:52-53 | OK |
| /api/ link convention (bare root-relative) | website/src/content/docs/dev/host-api.md:16,77,130 | OK |

## GAPS / nuances

- R-Memory slot size is ambiguous in the source: the authoritative class
  constants are `RMEM_SLOT_SIZE = 444` (min guaranteed) and
  `RMEM_SLOT_SIZE_MAX = 475`, but the class summary comment (line 55) and the
  `rmemWrite` param doc (line 171) say "476 bytes". Documented the constants as
  authoritative and flagged the comment discrepancy in an aside. Not resolved in
  source; consider correcting the comments to 475.
- The slot map header has a stale lead-in comment "512 R-Memory slots" while the
  enum max constant resolves to indices 0-511 (512 slots). Consistent with the
  count; no doc impact.
