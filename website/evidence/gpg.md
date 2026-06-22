# Evidence ledger: GPG / OpenPGP & SSH

Each row backs one claim in the GPG documentation pages. `path:line` is relative
to the repository root. Tags: `VERIFIED` (read in source), `GAP` (absent /
stubbed / uncertain), `CAVEAT` (verified but with a correctness limitation).

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Badge exposes a USB CCID smart-card interface, class 0x0B | components/mod_gpg/include/mod_gpg/openpgp/ccid.h:21 | VERIFIED |
| CCID registered as USB interface "OpenPGP SmartCard", 64-byte EPs | components/mod_gpg/src/GpgModule.cpp:950-963 | VERIFIED |
| CCID uses Gemalto GemPC433 VID/PID 0x08e6/0x4433 | components/mod_gpg/include/mod_gpg/openpgp/ccid.h:24-26 | VERIFIED |
| OpenPGP card application targets spec 3.4.1 | components/mod_gpg/include/mod_gpg/openpgp/openpgp.h:8-9 | VERIFIED |
| AID = D2 76 00 01 24 01, version 3.4, manufacturer "CD", serial from MAC | components/mod_gpg/src/openpgp/openpgp.cpp:149-157, 833-861 | VERIFIED |
| Three key roles: SIG (0xB6), DEC (0xB8), AUT (0xA4) | components/mod_gpg/include/mod_gpg/openpgp/openpgp.h:35-37 | VERIFIED |
| GPG ECC slots 1-3, RMEM slots 1-3 | main/tropic_slot_map.h:48-49, 62-63 | VERIFIED |
| SIG/AUT keys live in TROPIC01 ECC slots | components/mod_gpg/src/gpg.cpp:148-156 | VERIFIED |
| DEC key uses software P-256 ECDH (TROPIC01 has no ECDH); privkey stored encrypted in R-Memory | components/mod_gpg/src/gpg.cpp:158-176; components/mod_gpg/src/openpgp/openpgp.cpp:230-238, 2374-2393 | VERIFIED |
| Supported SIG/AUT curves: Ed25519 (EdDSA) and P-256 (ECDSA) | components/mod_gpg/include/mod_gpg/gpg.h:11-12; components/mod_gpg/src/openpgp/openpgp.cpp:237-238, 381-394 | VERIFIED |
| DEC curve fixed to P-256 ECDH | components/mod_gpg/src/openpgp/openpgp.cpp:230-238, 401-404, 1327-1332 | VERIFIED |
| SIG/AUT default curve Ed25519; flippable via PUT DATA C1/C3 | components/mod_gpg/src/openpgp/openpgp.cpp:237-238, 1307-1357 | VERIFIED |
| Algorithm attribute OIDs: ed25519 1.3.6.1.4.1.11591.15.1, P-256 1.2.840.10045.3.1.7 | components/mod_gpg/src/openpgp/openpgp.cpp:381-404 | VERIFIED |
| RSA not supported (algo_attr_validate_capability rsa_supported=false) | components/mod_gpg/src/openpgp/openpgp.cpp:1324 | VERIFIED |
| On-device key generation generates SIG+DEC+AUT in one wizard | components/mod_gpg/src/gpg.cpp:132-222; components/mod_gpg/src/GpgModule.cpp:594-675 | VERIFIED |
| Host-side generation via GENERATE ASYMMETRIC KEY PAIR (INS 0x47, P1=0x80) requires PW3 | components/mod_gpg/src/openpgp/openpgp.cpp:2507-2547 | VERIFIED |
| Public key export as SubjectPublicKeyInfo PEM (Ed25519 / P-256 DER prefix) | components/mod_gpg/src/gpg.cpp:233-317 | VERIFIED |
| Export also shown as QR + alchemy fingerprint | components/mod_gpg/src/GpgModule.cpp:680-709 | VERIFIED |
| v4 fingerprint = SHA-1 (20 bytes); gen-time embedded | components/mod_gpg/include/mod_gpg/openpgp/constants.h:33-34; components/mod_gpg/src/gpg.cpp:77-82, 196-213 | VERIFIED |
| PW1 = User PIN (refs 0x81 sign, 0x82 other); PW3 = Admin PIN (ref 0x83) | components/mod_gpg/include/mod_gpg/openpgp/constants.h:71-78 | VERIFIED |
| PW1 min 6, PW3 min 8, PIN max 32 (OpenPGP layer) | components/mod_gpg/include/mod_gpg/openpgp/openpgp.h:40-42 | VERIFIED |
| PinManager PW1_MIN 6, PW3_MIN 8, PIN_MAX 16 | components/cdc_core/include/cdc_core/PinManager.h:51-53 | VERIFIED |
| Default PW1 "123456", PW3 "12345678" | components/cdc_core/include/cdc_core/PinManager.h:75-76; components/cdc_core/src/pin_storage_c.cpp:57-58 | VERIFIED |
| PW1/PW3 smartcard semantics: pre-decrement persisted, 3 retries, zero is terminal until admin reset (no timed recovery) | components/cdc_core/include/cdc_core/PinManager.h:22-23, 37-39, 167 | VERIFIED |
| VERIFY (INS 0x20): Lc=0 query retries; blocked returns 0x6983; failure 0x63Cx | components/mod_gpg/src/openpgp/openpgp.cpp:1566-1632 | VERIFIED |
| PSO:CDS (INS 0x2A P1P2 9E9A) requires PW1; SHA-256 digest for P-256; increments sig counter | components/mod_gpg/src/openpgp/openpgp.cpp:1847-1885, 2786-2788 | VERIFIED |
| PSO:DECIPHER (INS 0x2A P1P2 8086) requires PW1; ECDH or AES (pad byte 0x02) | components/mod_gpg/src/openpgp/openpgp.cpp:1980-1991, 2789-2790 | VERIFIED |
| PSO:DECIPHER ECDH decrypts DEC privkey in RAM, zeroized after use | components/mod_gpg/src/openpgp/openpgp.cpp:2066-2080 | VERIFIED |
| AES symmetric key (DO 0xD5) stored via gpg_storage; CFB-128 decrypt | components/mod_gpg/src/openpgp/openpgp.cpp:1413-1420, 1928-1978 | VERIFIED |
| INTERNAL AUTHENTICATE (INS 0x88) signs challenge with AUT key, requires PW1 (SSH) | components/mod_gpg/src/openpgp/openpgp.cpp:2126-2167 | VERIFIED |
| AUT key usable for SSH via gpg-agent | components/mod_gpg/src/openpgp/openpgp.cpp:2126-2131 | VERIFIED |
| MANAGE SECURITY ENVIRONMENT (INS 0x22) no-op, role-checked | components/mod_gpg/src/openpgp/openpgp.cpp:2097-2123 | VERIFIED |
| GET CHALLENGE (INS 0x84) returns random bytes | components/mod_gpg/src/openpgp/openpgp.cpp:2808-2815 | VERIFIED |
| Command chaining (CLA 0x10) and GET RESPONSE (INS 0xC0) supported | components/mod_gpg/src/openpgp/openpgp.cpp:2648-2681, 2707-2741 | VERIFIED |
| Secure messaging and channels > 0 rejected (SW 0x6E00) | components/mod_gpg/src/openpgp/openpgp.cpp:2701-2705 | VERIFIED |
| CHANGE REFERENCE DATA (INS 0x24) splits old||new, decrements on fail | components/mod_gpg/src/openpgp/openpgp.cpp:1784-1838 | VERIFIED |
| RESET RETRY COUNTER (INS 0x2C): P1=0x02 admin-driven (PW3), P1=0x00 via Resetting Code | components/mod_gpg/src/openpgp/openpgp.cpp:2265-2339 | VERIFIED |
| Resetting Code (DO 0xD3) set/clear via PUT DATA; stored as iterated-salted SHA-256 in NVS, 3 retries | components/mod_gpg/src/openpgp/openpgp.cpp:182-228, 1380-1412 | VERIFIED |
| TERMINATE DF (INS 0xE6) needs PW3 or both PINs blocked | components/mod_gpg/src/openpgp/openpgp.cpp:2179-2195 | VERIFIED |
| ACTIVATE FILE (INS 0x44) factory-resets when terminated | components/mod_gpg/src/openpgp/openpgp.cpp:2205-2216 | VERIFIED |
| Terminated state only accepts SELECT + ACTIVATE FILE (else 0x6285) | components/mod_gpg/src/openpgp/openpgp.cpp:2744-2758 | VERIFIED |
| PUT DATA requires PW3 | components/mod_gpg/src/openpgp/openpgp.cpp:1359-1363 | VERIFIED |
| PUT DATA (odd 0xDB) imports DEC privkey only; SIG/AUT have no import path | components/mod_gpg/src/openpgp/openpgp.cpp:1481-1557 | VERIFIED |
| DOs returned: AID, 6E app-related, 65 cardholder, C0-C9, CA-CC, CD-D0, 93 sig count, 5F50 URL, etc. | components/mod_gpg/src/openpgp/openpgp.cpp:1016-1193 | VERIFIED |
| Fingerprints stored: SIG/DEC/AUT + 3 CA fingerprints | components/mod_gpg/src/openpgp/openpgp.cpp:337-353, 574-588 | VERIFIED |
| Extended capabilities: SM none, max cert 2048, special DO 256, PIN block 2 none, MSE-DO none | components/mod_gpg/src/openpgp/openpgp.cpp:407-417 | VERIFIED |
| KDF DO (0xF9): GET DATA returns empty = "no KDF configured"; no PUT DATA handler | components/mod_gpg/src/openpgp/openpgp.cpp:1183-1185 (no PUT DATA case) | GAP |
| kdf.h codec exists but kdf_do_parse/build unused by openpgp.cpp | components/mod_gpg/include/mod_gpg/openpgp/kdf.h:94-114; grep: no callers in openpgp.cpp | GAP |
| Internal PIN hash is iterated-salted SHA-256 S2K, 100000 bytes | components/mod_gpg/src/openpgp/openpgp.cpp:1650-1684; components/cdc_core/include/cdc_core/PinManager.h:69-71 | VERIFIED |
| Cardholder Certificate (7F21) GET DATA returns 0x6A88 (not stored) | components/mod_gpg/src/openpgp/openpgp.cpp:1187-1188 | GAP |
| OpenPGP NVS state signed with slot-0 P-256 ECDSA attestation; invalid sig -> reinit | components/mod_gpg/src/openpgp/openpgp.cpp:646-693, 716-723 | VERIFIED |
| DEC privkey + AES key encrypted with AES-256-GCM, HKDF over (chip_id\|\|pin_hash), AAD binds slot | components/mod_gpg/src/GpgStorage.cpp:1-7, 38-46 | VERIFIED |
| ECDH path uses MBEDTLS_ECP_DP_SECP256R1 | components/mod_gpg/src/openpgp/ecdh.cpp:92, 170, 231 | VERIFIED |
| Serial commands: STATUS/GENERATE/EXPORT/RESET/RECV_LIST/RECV_INFO/RECV_DELETE/RECV_IMPORT/RECV_CROSS_SIGN/RECV_EXPORT/MYCERT_LIST/MYCERT_DELETE/MYCERT_IMPORT | components/mod_gpg/src/GpgModule.cpp:167-181 | VERIFIED |
| GPG GENERATE arg: curve 1=Ed25519, 2=P-256, then user_id | components/mod_gpg/src/GpgModule.cpp:95, 144-173 | VERIFIED |
| GPG RESET is two-step token-confirmed (30 s window) | components/mod_gpg/src/GpgModule.cpp:194-219 | VERIFIED |
| Cross-sign: badge signs a received key's V4 pubkey + UID as RFC 4880 cert (sig type 0x10) | components/mod_gpg/src/openpgp/xsig.cpp:193-281 | VERIFIED |
| Cross-sign hash is SHA-256; sig algo from own curve; R\|\|S 64 bytes | components/mod_gpg/src/openpgp/xsig.cpp:217-281 | VERIFIED |
| Signed key exported as ASCII-armored PGP PUBLIC KEY BLOCK (pubkey + UID + sig packets, CRC24) | components/mod_gpg/src/openpgp/xsig.cpp:283-451 | VERIFIED |
| BLE cross-sign service UUID 8E2F1F30-...; RX 1F31 write, STATUS 1F32 read+notify | components/mod_gpg/src/ble_gpg_xsig.cpp:43-59, 254-290 | VERIFIED |
| BLE chars require encryption (WRITE_ENC / READ_ENC) | components/mod_gpg/src/ble_gpg_xsig.cpp:264-272 | VERIFIED |
| BLE chunk opcodes 0x11/0x12/0x13 (write), 0x91/0x92/0x93 (status); status codes 0x00-0x03 | components/mod_gpg/src/ble_gpg_xsig.cpp:61-71 | VERIFIED |
| BLE payload layout: curve, pubkey_len, pubkey (32/64), fp_v4 (20), uid_len, uid (<=63) | components/mod_gpg/src/ble_gpg_xsig.cpp:73-75, 109-143, 214-250 | VERIFIED |
| Received keys persisted via GpgRecvStore (NVS blob per key, max 128) | components/mod_gpg/include/mod_gpg/GpgRecvStore.h:58-106 | VERIFIED |
| gpg_get_status always reports curve = Ed25519 (real curve not exposed) | components/mod_gpg/src/gpg.cpp:108-111 | CAVEAT |
| Cross-sign signature algorithm taken from self_status.curve (=Ed25519 const) | components/mod_gpg/src/openpgp/xsig.cpp:199-218; components/mod_gpg/src/gpg.cpp:108-111 | CAVEAT |
| "Send Key (BLE)" menu action is a WIP placeholder toast | components/mod_gpg/src/GpgModule.cpp:751-755 | GAP |
| Received-key context actions (Sign/Export/Delete) wired in UI | components/mod_gpg/src/GpgModule.cpp:833-900 | VERIFIED |
