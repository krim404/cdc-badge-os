# Evidence ledger: Two-factor & OTP HID CR

Tags: VERIFIED = exact source line supports the claim; GAP = not verifiable in source.

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| mod_2fa is default-enabled | main/module_defaults.h:24 (`X("mod_2fa", true)`) | VERIFIED |
| mod_otphid is default-disabled | main/module_defaults.h:32 (`X("mod_otphid", false)`) | VERIFIED |
| OATH types are TOTP, HOTP, CR | components/mod_2fa/include/mod_2fa/OathStore.h:23-27 | VERIFIED |
| Hash algorithms SHA1/SHA256/SHA512 | components/mod_2fa/include/mod_2fa/OathStore.h:14-18 | VERIFIED |
| hmacCompute maps SHA1=20, SHA256=32, SHA512=64 byte digests | components/mod_2fa/src/OathStore.cpp:451-464 | VERIFIED |
| CR restricted to SHA1/SHA256 at write time | components/mod_2fa/src/OathStore.cpp:43-50 | VERIFIED |
| CR restricted to SHA1/SHA256 at compute time | components/mod_2fa/src/OathStore.cpp:657-661 | VERIFIED |
| Digits allowed 6-8, default 6 | components/mod_2fa/src/OathStore.cpp:17-18, 52-58; OathStore.h:60 | VERIFIED |
| Zero digits -> default 6 | components/mod_2fa/src/OathStore.cpp:52-53 | VERIFIED |
| TOTP period 15-300 s, default 30 | components/mod_2fa/src/OathStore.cpp:23-24, 61-68; OathStore.h:61 | VERIFIED |
| Zero period -> default 30 | components/mod_2fa/src/OathStore.cpp:62-63 | VERIFIED |
| Capacity = R-Memory slots 32-131 (100 slots) | main/tropic_slot_map.h (RMEM_SLOT_MOD_2FA_START 32 / _END 131) | VERIFIED |
| Secrets stored in TROPIC01 secure element | components/mod_2fa/src/OathStore.cpp:174-184, 254-265 (rmemReadWithHeader/rmemWriteWithHeader) | VERIFIED |
| Name max 16 chars | components/mod_2fa/include/mod_2fa/OathStore.h:57 (NAME_LEN=16) | VERIFIED |
| Secret entered as Base32; decoded via base32Decode | components/mod_2fa/src/OathStore.cpp:112-139, 312 | VERIFIED |
| On-device add wizard: Type/Name/Secret/Issuer/Digits/Algorithm/Period | components/mod_2fa/src/TwoFaModule.cpp:943-1106 | VERIFIED |
| Wizard issuer skipped for CR | components/mod_2fa/src/TwoFaModule.cpp:1012-1015 | VERIFIED |
| Wizard digits offered 6/7/8 | components/mod_2fa/src/TwoFaModule.cpp:1026-1044 | VERIFIED |
| Wizard algo SHA1/SHA256/SHA512; CR limited to first 2 | components/mod_2fa/src/TwoFaModule.cpp:1055-1064 | VERIFIED |
| Wizard period 30s/60s (TOTP) | components/mod_2fa/src/TwoFaModule.cpp:1088-1105 | VERIFIED |
| Name and secret required; empty aborts save | components/mod_2fa/src/TwoFaModule.cpp:1171-1176 | VERIFIED |
| 2FA app in MAIN_MENU | components/mod_2fa/src/TwoFaModule.cpp:1354-1361 (MenuLocation::MAIN_MENU) | VERIFIED |
| Serial TOTP ADD/LIST/GET/DEL command group | components/mod_2fa/src/TwoFaModule.cpp:413-419, 431-433 | VERIFIED |
| Serial command AUTH/PIN-gated (CMD_MODULE, requiresAuth flag true) | components/mod_2fa/src/TwoFaModule.cpp:67, 431-436 | VERIFIED |
| TOTP ADD usage: type/name/secret/[issuer]/[digits]/[period]/[algo]/[counter] | components/mod_2fa/src/TwoFaModule.cpp:208-209, 415 | VERIFIED |
| Serial type tokens totp/hotp/cr | components/mod_2fa/src/TwoFaModule.cpp:111-113 | VERIFIED |
| Serial algo tokens sha1/sha256/sha512 | components/mod_2fa/src/TwoFaModule.cpp:87-89 | VERIFIED |
| TOTP GET advances/persists HOTP counter | components/mod_2fa/src/TwoFaModule.cpp:285-318; OathStore.cpp:529-541 | VERIFIED |
| TOTP code refreshes once per second in view | components/mod_2fa/src/TwoFaModule.cpp:495-502 | VERIFIED |
| HOTP key 5 advances to next code; consumes counter | components/mod_2fa/src/TwoFaModule.cpp:595-601, 676-687 | VERIFIED |
| Key Y types code if USB keyboard connected | components/mod_2fa/src/TwoFaModule.cpp:602-614 | VERIFIED |
| Key 3 edits, key N back | components/mod_2fa/src/TwoFaModule.cpp:587-594 | VERIFIED |
| TOTP needs valid clock = year >= 2024 | components/mod_2fa/src/OathStore.cpp:573-578 | VERIFIED |
| TOTP shows dashes / "Time not set" when clock invalid | components/mod_2fa/src/OathStore.cpp:544-548; TwoFaModule.cpp:539-545 | VERIFIED |
| CR has no displayable code | components/mod_2fa/src/TwoFaModule.cpp:532-537, 669-674 | VERIFIED |
| CR response = full untruncated HMAC; 20 (SHA1) or 32 (SHA256) | components/mod_2fa/include/mod_2fa/OathStore.h:84-85; OathStore.cpp:663-672 | VERIFIED |
| Per-entry touch-required flag (TOUCH_REQUIRED=0x01) | components/mod_2fa/include/mod_2fa/OathStore.h:32-34; TwoFaModule.cpp:1128-1135 | VERIFIED |
| USB-CR slot designation flag (USB_CR_SLOT=0x02), single responder | components/mod_2fa/include/mod_2fa/OathStore.h:35-36; OathStore.cpp:741-771; TwoFaModule.cpp:1158-1214 | VERIFIED |
| Serial CHALRESP computes HMAC for named CR entry, prints hex, no touch | components/mod_2fa/src/TwoFaModule.cpp:357-408, 434-436 | VERIFIED |
| BLE CR transport: write challenge, optional touch, notify response | components/mod_2fa/include/mod_2fa/ble_chalresp.h:7-19 | VERIFIED |
| Touch gate applied for BLE/USB, not serial | components/mod_2fa/src/TwoFaModule.cpp:357-362 (serial trusted); OathStore.h:94-97 | VERIFIED |
| MAX_RESPONSE_LEN = 32 | components/cdc_core/include/cdc_core/IChallengeResponder.h:20 | VERIFIED |
| OTP HID: vendor HID usage page 0xFF00, not keyboard | components/mod_otphid/src/OtpHidInterface.cpp:41-56, 108 | VERIFIED |
| OTP HID identity OnlyKey VID 0x1D50 / PID 0x60FC | components/usb_badge/usb_descriptors.h:21-22; OtpHidInterface.cpp:111-112; OtpHidModule.cpp:80 | VERIFIED |
| Acquires UsbManager Keyboard slot; exclusive w/ mod_usbhid; MAX_ACTIVE_HID=2 | components/mod_otphid/include/mod_otphid/OtpHidModule.h:7-18; OtpHidModule.cpp:139-155; cdc_core/UsbManager.h:98 | VERIFIED |
| Feature report size 8 bytes | components/mod_otphid/include/mod_otphid/OtpHidConstants.h:8 | VERIFIED |
| Status struct version 2.4.0, pgmSeq 1, touchLevel bits 0x01\|0x02\|0x08 | components/mod_otphid/src/OtpHidCr.cpp:46-51, 89-105 | VERIFIED |
| CONFIG2_TOUCH advertises touch-configured slot 2 | components/mod_otphid/src/OtpHidCr.cpp:44-51 | VERIFIED |
| 70-byte frame: payload(64)+slot(1)+crc(2)+filler(3) | components/mod_otphid/src/OtpHidCr.cpp:36-37, 169 | VERIFIED |
| 8-byte reports = 7 data + seq/flag byte | components/mod_otphid/src/OtpHidCr.cpp:55, 134-147 | VERIFIED |
| SLOT_WRITE_FLAG=0x80; report w/o it resets state | components/mod_otphid/src/OtpHidCr.cpp:39, 140-144 | VERIFIED |
| Block 0 supersedes stale frame | components/mod_otphid/src/OtpHidCr.cpp:151-158 | VERIFIED |
| Frame CRC16 over 64-byte payload, LE after slot byte | components/mod_otphid/src/OtpHidCr.cpp:167-176 | VERIFIED |
| Slot byte must = SLOT_CHAL_HMAC2 0x38 | components/mod_otphid/src/OtpHidCr.cpp:38, 177-181 | VERIFIED |
| CRC16 poly 0x8408, init 0xFFFF, reflected | components/mod_otphid/src/OtpHidCr.cpp:107-118 | VERIFIED |
| CRC16 residual 0xF0B8 (ISO13239) | components/mod_otphid/src/OtpHidCr.cpp:11, 286-287 | VERIFIED |
| HMAC delegated to IChallengeResponder USB slot; module never hashes | components/mod_otphid/src/OtpHidCr.cpp:258-280; OtpHidCr.h:20-21 | VERIFIED |
| SHA1-only: digest must be exactly 20 bytes else rejected | components/mod_otphid/src/OtpHidCr.cpp:42, 270-280 | VERIFIED |
| Response = 20-byte digest + 2-byte CRC (one's-complement, LE) | components/mod_otphid/src/OtpHidCr.cpp:53-54, 285-291 | VERIFIED |
| Touch gate shows E-Paper confirm; release on confirm / idle on cancel | components/mod_otphid/src/OtpHidCr.cpp:227-246, 295-300 | VERIFIED |
| Pending/Waiting GET returns RESP_PENDING_FLAG=0x40, seq 0 | components/mod_otphid/src/OtpHidCr.cpp:40, 193-200 | VERIFIED |
| READY readback: 7 bytes/report, trailing RESP_PENDING_FLAG\|(seq & 0x1F) | components/mod_otphid/src/OtpHidCr.cpp:41, 201-219 | VERIFIED |
| Completion: final zeroed frame seq 0, state -> idle | components/mod_otphid/src/OtpHidCr.cpp:203-210 | VERIFIED |
| Crypto/UI on main task tick; callbacks only buffer | components/mod_otphid/src/OtpHidCr.cpp:12-15, 248-256; OtpHidModule.cpp:162-166 | VERIFIED |
| KeePassXC/ykchalresp slot-2 CR + fixed 64-byte challenge | components/mod_otphid/include/mod_otphid/OtpHidCr.h:8-30; OtpHidCr.cpp:270-271 | VERIFIED (per source comment) |
| Protocol references: ykdef.h, ykcore.c, ykcrc.c | components/mod_otphid/src/OtpHidCr.cpp:6-10 | VERIFIED (per source comment) |
| End-to-end host (KeePassXC/ykinfo) test capture | not present in source | GAP |
| usb_hid_ready() host-enumeration condition | components/mod_otphid/src/OtpHidInterface.cpp:137-140 (delegates to usb_hid_ready, outside module) | GAP |
| Input report behaviour (declared, unused for CR) | components/mod_otphid/src/OtpHidInterface.cpp:38-40 (comment only) | GAP |
