# Evidence ledger: FIDO2 / WebAuthn / CTAP

Tags: VERIFIED = exact source line supports the claim; GAP = not verifiable in source (constant exists but no implementation, or behaviour absent).

All paths are relative to the repo root `~/GIT/cdc-badge-os`.

## Transport (CTAPHID / USB HID)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Transport is USB HID with the FIDO Alliance usage page (0xF1D0) and U2F HID usage | components/mod_fido2/src/Fido2Module.cpp:21-38 (`s_fido_report_desc`) | VERIFIED |
| HID report/packet size is 64 bytes | components/mod_fido2/include/mod_fido2/ctaphid.h:13 (`CTAPHID_PACKET_SIZE 64`) | VERIFIED |
| CTAPHID commands handled: INIT, PING, WINK, CANCEL, CBOR, MSG | components/mod_fido2/src/ctaphid.cpp:387-403 | VERIFIED |
| CTAPHID INIT advertises capabilities WINK \| CBOR | components/mod_fido2/src/ctaphid.cpp:298 (`CTAPHID_CAP_WINK \| CTAPHID_CAP_CBOR`) | VERIFIED |
| CTAPHID max message size constant is 2048 | components/mod_fido2/include/mod_fido2/ctaphid.h:16 (`CTAPHID_MAX_MSG_SIZE 2048`) | VERIFIED |
| Only one HID transport reported in getInfo: "usb" | components/mod_fido2/src/ctap2.cpp:54 (`INFO_TRANSPORTS[] = {"usb"}`); 622-626 | VERIFIED |

## getInfo surface

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| getInfo reports versions FIDO_2_0, FIDO_2_1, U2F_V2 | components/mod_fido2/src/ctap2.cpp:553-558 | VERIFIED |
| getInfo extensions: appid, credProtect, appidExclude | components/mod_fido2/src/ctap2.cpp:562-568 | VERIFIED |
| getInfo options map (7 keys): rk=true, up=true, uv=false, plat=false, credMgmt=true, clientPin=true, pinUvAuthToken=true | components/mod_fido2/src/ctap2.cpp:577-594 | VERIFIED |
| maxMsgSize reported = 1200 | components/mod_fido2/src/ctap2.cpp:546, 597-600 | VERIFIED |
| pinUvAuthProtocols reported = [2] | components/mod_fido2/src/ctap2.cpp:548, 603-607 | VERIFIED |
| maxCredentialCountInList reported = 8 | components/mod_fido2/src/ctap2.cpp:550, 610-613 | VERIFIED |
| maxCredentialIdLength reported = 64 | components/mod_fido2/src/ctap2.cpp:616-619; components/mod_fido2/include/mod_fido2/fido2.h:20 (`FIDO2_CRED_ID_LEN 64`) | VERIFIED |
| Algorithms advertised: ES256 (-7) and EdDSA (-8), both type public-key | components/mod_fido2/src/ctap2.cpp:629-644; 84-85 | VERIFIED |

## Supported CTAP2 commands

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Dispatched: getInfo(0x04), makeCredential(0x01), getAssertion(0x02), getNextAssertion(0x08), clientPIN(0x06), reset(0x07), credMgmt(0x0A), selection(0x0B) | components/mod_fido2/src/ctap2.cpp:3559-3590 | VERIFIED |
| LargeBlobs(0x0C) and Config(0x0D) return CTAP2_ERR_UNSUPPORTED_OPTION (not implemented) | components/mod_fido2/src/ctap2.cpp:3592-3597 | VERIFIED |
| Any other command returns CTAP1_ERR_INVALID_COMMAND (BioEnrollment 0x09 not handled) | components/mod_fido2/src/ctap2.cpp:3599-3603 | VERIFIED |

## ClientPIN

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| PIN protocol version supported = 2 (protocol 0/unset also accepted) | components/mod_fido2/src/ctap2.cpp:78 (`PIN_PROTOCOL_VERSION 2`); 2986-2991 | VERIFIED |
| ClientPIN subcommands handled: getRetries(0x01), getKeyAgreement(0x02), getPinToken(0x05), getPinUvAuthToken(0x09) | components/mod_fido2/src/ctap2.cpp:2993-3004 | VERIFIED |
| setPIN(0x03) and changePIN(0x04) return CTAP2_ERR_UNSUPPORTED_OPTION (PIN set via badge UI, not over CTAP) | components/mod_fido2/src/ctap2.cpp:3006-3011 | VERIFIED |
| Key agreement key is COSE EC2 P-256, alg ECDH-ES+HKDF-256 (-25) | components/mod_fido2/src/ctap2.cpp:2370-2390; 87 | VERIFIED |
| Protocol 2 pinHashEnc = 16-byte IV + 16-byte ciphertext, AES-256-CBC | components/mod_fido2/src/ctap2.cpp:2588-2608 | VERIFIED |
| PIN hash is verified against the badge's stored FIDO2 PIN hash (pin_storage_verify_fido2_hash) | components/mod_fido2/src/ctap2.cpp:2648; 2416 | VERIFIED |
| PIN retries max = 8; UV retries max = 3 | components/mod_fido2/src/ctap2.cpp:80-81 (`PIN_RETRIES_MAX 8`, `PIN_UV_RETRIES_MAX 3`) | VERIFIED |
| getPinToken returns PIN_BLOCKED when retries reach 0; PIN_NOT_SET when no FIDO2 hash | components/mod_fido2/src/ctap2.cpp:2409-2421; 2649-2658 | VERIFIED |
| pinUvAuthParam verified as HMAC-SHA-256(pinToken, clientDataHash), first 32 bytes for protocol 2 | components/mod_fido2/src/ctap2.cpp:984-997 | VERIFIED |
| pinUvAuthToken permission flags defined: mc, ga, cm, be, lbw, acfg | components/mod_fido2/src/ctap2.cpp:92-97 | VERIFIED |
| Permission flags be (bioEnrollment), lbw (largeBlobWrite), acfg (authnConfig) are constants only; no command consumes them | components/mod_fido2/src/ctap2.cpp:95-97 (defined); 3592-3603 (LargeBlobs/Config unsupported, no BioEnrollment) | GAP |

## makeCredential

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Supported algorithms selected: ES256 -> P-256, EdDSA -> Ed25519; else UNSUPPORTED_ALGORITHM | components/mod_fido2/src/ctap2.cpp:820-824; 1279-1288 | VERIFIED |
| option uv=true rejected with UNSUPPORTED_OPTION | components/mod_fido2/src/ctap2.cpp:1291-1295 | VERIFIED |
| option up=false rejected with INVALID_OPTION (user presence always required) | components/mod_fido2/src/ctap2.cpp:1296-1300 | VERIFIED |
| credProtect requested level parsed, range 1..3 | components/mod_fido2/src/ctap2.cpp:851-856 | VERIFIED |
| credProtect echoed in makeCredential authData extension | components/mod_fido2/src/ctap2.cpp:234-245; 272-278 | VERIFIED |
| appidExclude extension: matching existing credential -> CREDENTIAL_EXCLUDED | components/mod_fido2/src/ctap2.cpp:1009-1020; 50 | VERIFIED |
| User presence requested before credential creation | components/mod_fido2/src/ctap2.cpp:1307-1311 | VERIFIED |
| Existing RP+user credential is overwritten (action OVERWRITE) | components/mod_fido2/src/ctap2.cpp:1302-1305; components/mod_fido2/src/fido2_storage.cpp:777-799 | VERIFIED |

## getAssertion / sign counter

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Per-credential signature counter incremented per assertion | components/mod_fido2/src/ctap2.cpp:1908; components/mod_fido2/src/fido2_storage.cpp:915-934 | VERIFIED |
| Sign counter persisted in TROPIC01 R-Memory | components/mod_fido2/src/fido2_storage.cpp:924-931 | VERIFIED |
| Sign counter is per-credential (stored field sign_count) not a single global | components/mod_fido2/src/fido2_storage.cpp:39; 915-922 | VERIFIED |
| A separate global auth counter is kept in NVS (not on TROPIC01) | components/mod_fido2/src/fido2.cpp:280-291; components/mod_fido2/src/fido2_storage.cpp:374-405 | VERIFIED |
| authData flags: UP=0x01 set when user presence requested; UV=0x04 set when pinUvAuth verified | components/mod_fido2/src/ctap2.cpp:1915-1918 | VERIFIED |
| No allowList -> all resident credentials for the RP returned (discoverable) | components/mod_fido2/src/ctap2.cpp:1695-1701 | VERIFIED |
| credProtect level is NOT enforced at getAssertion (no UV-gated hiding of level-3 creds) | components/mod_fido2/src/ctap2.cpp:1656-1715 (ga_find_credentials has no credProtect check) | GAP |
| EdDSA assertions signed raw via eddsaSign; ECDSA via ecdsaSign | components/mod_fido2/src/fido2_storage.cpp:982-1009 | VERIFIED |

## Credential management on the host (credMgmt 0x0A)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| credMgmt requires a valid pinUvAuthToken first | components/mod_fido2/src/ctap2.cpp:3322-3328 | VERIFIED |
| Subcommands: getCredsMetadata, enumerateRPs(begin/next), enumerateCreds(begin/next), deleteCredential | components/mod_fido2/src/ctap2.cpp:3334-3471; 121-126 | VERIFIED |
| deleteCredential removes credential by credential ID | components/mod_fido2/src/ctap2.cpp:3442-3471 | VERIFIED |
| credMgmt response reports credProtect (defaults to 1 if unset) | components/mod_fido2/src/ctap2.cpp:3205-3207 | VERIFIED |
| The per-subcommand pinUvAuthParam is parsed but its HMAC is not re-verified (only token validity gate) | components/mod_fido2/src/ctap2.cpp:3307-3312 (`// We skip PIN auth verification for now`) | GAP |
| updateUserInformation (credMgmt 0x07, CTAP 2.1) not implemented (default -> UNSUPPORTED_OPTION) | components/mod_fido2/src/ctap2.cpp:3473-3476 | GAP |

## On-device confirmation (UI)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| User presence is a prompt on the badge; approve [Y] / deny [N] | components/mod_fido2/src/Fido2Ui.cpp:604; components/cdc_ui/src/I18n.cpp:248 (`core.hint_approve_deny` = "[Y] Approve  [N] Deny") | VERIFIED |
| User-presence timeout is 30 seconds | components/mod_fido2/src/Fido2Ui.cpp:634 (`pdMS_TO_TICKS(30000)`); components/mod_fido2/src/ctap2.cpp:56 (`USER_PRESENCE_TIMEOUT_MS 30000`) | VERIFIED |
| Prompt headlines: Register Key / Sign In / OVERWRITE KEY! / Use this device? | components/mod_fido2/src/Fido2Ui.cpp:588-597; 40-46 | VERIFIED |
| Reset (authenticatorReset) requires explicit user presence | components/mod_fido2/src/ctap2.cpp:3027-3033 | VERIFIED |
| Badge menu lists FIDO2 credentials; per-entry menu (key 3) offers Details / Delete | components/mod_fido2/src/Fido2Ui.cpp:158, 262-264 | VERIFIED |
| Delete from the badge calls fido2_delete_credential | components/mod_fido2/src/Fido2Ui.cpp:263; components/mod_fido2/src/fido2.cpp (fido2_delete_credential) | VERIFIED |
| Credential detail view shows resident_key yes/no and curve | components/mod_fido2/src/Fido2Ui.cpp:200-211; components/mod_fido2/include/mod_fido2/fido2.h:57-59 | VERIFIED |

## Capacity & storage (slot map)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| FIDO2 ECC slots = 5..30 (26 slots) | main/tropic_slot_map.h (`ECC_SLOT_MOD_FIDO2_START 5`, `ECC_SLOT_MOD_FIDO2_END 30`) | VERIFIED |
| FIDO2 R-Memory slots = 5..31 | main/tropic_slot_map.h (`RMEM_SLOT_MOD_FIDO2_START 5`, `RMEM_SLOT_MOD_FIDO2_END 31`) | VERIFIED |
| Every credential (resident or not) consumes one ECC slot; private key lives in the slot | components/mod_fido2/src/fido2_storage.cpp:793-819, 841-846 (cred ID = slot + nonce + padding, key generated in ECC slot) | VERIFIED |
| Maximum credentials bounded by the ECC slot count (find_free_slot iterates ecc_count) | components/mod_fido2/src/fido2_storage.cpp:487-495, 137-140 | VERIFIED |
| FIDO2_MAX_CREDENTIALS compile constant = 32 (not the binding limit; ECC range of 26 binds) | components/mod_fido2/include/mod_fido2/fido2.h:16 | VERIFIED |
| Credential ID length is fixed 64 bytes | components/mod_fido2/include/mod_fido2/fido2.h:20 | VERIFIED |
| Credential metadata (rp_id_hash, user, sign_count, curve, credProtect) stored in R-Memory | components/mod_fido2/src/fido2_storage.cpp:31-46 | VERIFIED |

## Attestation key & AAGUID

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| AAGUID exact bytes: CD CB AD 6E 39 C3 00 01 BA D6 E0 01 00 00 00 01 | components/mod_fido2/src/ctap2.cpp:45-51 | VERIFIED |
| AAGUID is the same 16 bytes in getInfo and in attested credential data | components/mod_fido2/src/ctap2.cpp:205, 571-573 | VERIFIED |
| makeCredential attestation format is "packed" with x5c (certificate present) | components/mod_fido2/src/ctap2.cpp:336-337, 349-358 | VERIFIED |
| attStmt uses alg ES256 and an x5c array with one certificate (basic attestation) | components/mod_fido2/src/ctap2.cpp:351-358 | VERIFIED |
| "none" attestation only used if both sig and cert are empty (not the normal path) | components/mod_fido2/src/ctap2.cpp:333-335, 346-348 | VERIFIED |
| Attestation signature is ECDSA P-256 over (authData \|\| clientDataHash) by the slot-0 key | components/mod_fido2/src/ctap2.cpp:1181-1206; components/mod_fido2/src/u2f.cpp:84-116 | VERIFIED |
| Attestation private key lives in TROPIC01 ECC slot 0, never leaves the chip | components/mod_fido2/src/u2f.cpp:38, 93; components/cdc_core/include/cdc_core/AttestationKeyService.h:11 | VERIFIED |
| Attestation key is generated on-chip (eccGenerate) per device, not provisioned at factory | components/cdc_core/src/AttestationKeyService.cpp:123-131 | VERIFIED |
| Attestation key curve is P-256; wrong curve triggers regeneration | components/cdc_core/src/AttestationKeyService.cpp:138-148; components/mod_fido2/src/u2f.cpp:147-150 | VERIFIED |
| Public-key hash of the attestation key is persisted in NVS (namespace "attest") and checked on boot | components/cdc_core/src/AttestationKeyService.cpp:10-11, 150-174 | VERIFIED |
| Attestation certificate is self-signed: issuer == subject, signed by the same slot-0 key | components/mod_fido2/src/u2f.cpp:118-122, 216-231 (issuer reuses fido2_subject), 286-321 | VERIFIED |
| Certificate subject/issuer: C=DE, O=CDC, OU=Authenticator Attestation, CN=CDC Badge FIDO2 | components/mod_fido2/src/u2f.cpp:192-217 | VERIFIED |
| Certificate carries basicConstraints (critical, CA:FALSE) and keyUsage digitalSignature | components/mod_fido2/src/u2f.cpp:249-265 | VERIFIED |
| Certificate validity hardcoded 2024-01-01 .. 2049-12-31 | components/mod_fido2/src/u2f.cpp:219-227 | VERIFIED |
| No batch/CA chain: each badge generates its own attestation key, so the cert is per-device, not a shared batch certificate | components/cdc_core/src/AttestationKeyService.cpp:123-131 (per-device gen); components/mod_fido2/src/u2f.cpp:118-329 (self-signed, single cert) | VERIFIED |
| U2F (CTAP1) register also returns the slot-0 attestation cert and an ES256 P-256 signature | components/mod_fido2/src/u2f.cpp:569-583, 490-509 | VERIFIED |

## Notes / GAPS summary

- Attestation is **self-signed basic attestation with a per-device key**, not CA-backed batch attestation. There is no embedded CA chain and no shared batch key. (u2f.cpp:118-329, AttestationKeyService.cpp:123-131)
- CTAP 2.1 features advertised by version string "FIDO_2_1" that are **not** implemented: LargeBlobs (0x0C) and AuthenticatorConfig (0x0D) return UNSUPPORTED_OPTION; BioEnrollment (0x09) is not dispatched; credMgmt updateUserInformation (0x07) is unhandled. (ctap2.cpp:3592-3603, 3473-3476)
- credProtect is recorded and reported but **not enforced** at assertion time. (ctap2.cpp:1656-1715)
- ClientPIN setPIN/changePIN are **not** supported over the wire; the PIN is the badge PIN, set on-device. (ctap2.cpp:3006-3011)
- Per-subcommand credMgmt pinUvAuthParam HMAC is not re-verified beyond the token-valid gate. (ctap2.cpp:3307-3312)
