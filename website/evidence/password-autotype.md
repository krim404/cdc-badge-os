# Evidence: Password vault & Auto-type

| Claim | Source (path:line) | Tag |
|-------|--------------------|-----|
| Title field max 24 chars | components/mod_password/include/mod_password/PasswordStore.h:11 | VERIFIED |
| Username field max 64 chars | components/mod_password/include/mod_password/PasswordStore.h:12 | VERIFIED |
| Password field max 64 chars | components/mod_password/include/mod_password/PasswordStore.h:13 | VERIFIED |
| URL field max 96 chars | components/mod_password/include/mod_password/PasswordStore.h:14 | VERIFIED |
| Entry struct fields: title/username/password/url/totpSlot/notes | components/mod_password/include/mod_password/PasswordStore.h:23-30 | VERIFIED |
| PAYLOAD_MAX = RMEM_SLOT_SIZE - sizeof(RMemHeader) | components/mod_password/include/mod_password/PasswordStore.h:16-17 | VERIFIED |
| RMEM_SLOT_SIZE = 444 | components/cdc_hal/include/cdc_hal/ISecureElement.h:67 | VERIFIED |
| RMemHeader = magic+checksum+moduleId+flags+name[16]+payloadLen (packed = 22 bytes) | components/cdc_hal/include/cdc_hal/ISecureElement.h:187-194 | VERIFIED |
| FIXED_PAYLOAD = title+user+pw+url+1 = 249 | components/mod_password/include/mod_password/PasswordStore.h:18-19 | VERIFIED |
| NOTES_LEN = PAYLOAD_MAX - FIXED_PAYLOAD = 422 - 249 = 173 | components/mod_password/include/mod_password/PasswordStore.h:16-20 | VERIFIED (computed) |
| On-device notes input capped at min(NOTES_LEN, T9 MAX_TEXT_LEN=320) = 173 | components/mod_password/src/PasswordModule.cpp:444-447; components/cdc_views/include/cdc_views/T9InputView.h:22 | VERIFIED |
| One entry per R-memory slot | components/mod_password/src/PasswordStore.cpp:110-155 (addEntry writes one slot) | VERIFIED |
| Password RMEM slots 132-500 (369 slots) | main/tropic_slot_map.h:59-60 | VERIFIED |
| Entries stored in TROPIC01 secure-element R-memory | components/mod_password/src/PasswordStore.cpp:72-81, 133-143 (getSecureElementInstance + rmemWriteWithHeader/rmemReadWithHeader) | VERIFIED |
| R-memory write stores header + plaintext payload (checksum on header only, no payload encryption in this path) | components/cdc_hal/src/Tropic01Element.cpp:829-867 | VERIFIED |
| Payload written via libtropic lt_r_mem_data_write | components/cdc_hal/src/Tropic01Element.cpp:742 | VERIFIED |
| Writes blocked in alarm/lock mode | components/cdc_hal/src/Tropic01Element.cpp:832 | VERIFIED |
| Password at-rest encryption scheme NOT stated in headers/source | (absence) | GAP |
| TOTP slot link is optional (TOTP_SLOT_NONE = 0xFF) | components/mod_password/include/mod_password/PasswordStore.h:38 | VERIFIED |
| TOTP slot accepted range 0-254 | components/mod_password/src/PasswordModule.cpp:256, 354, 738 | VERIFIED |
| TOTP slot stored & displayed only (number); no code generation/typing in password module | components/mod_password/src/PasswordModule.cpp:563-566 (display), grep shows no getCode/2fa call | VERIFIED |
| Module is in MAIN_MENU titled "Passwords" | components/mod_password/src/PasswordModule.cpp:36, 908-920; IModule.h:18 | VERIFIED |
| List sorted alphabetically by title (case-insensitive) | components/mod_password/src/PasswordStore.cpp:281-283, 233-244 | VERIFIED |
| List hint: [Y] View [3] Menu [N] Back | components/mod_password/src/PasswordModule.cpp:53 | VERIFIED |
| Add via wizard: Title->Username->Password->URL->TOTP->Notes | components/mod_password/src/PasswordModule.cpp:652, 678-757 | VERIFIED |
| Random 16-char password generator (charset a-zA-Z0-9$!%=) via "x" input | components/mod_password/src/PasswordModule.cpp:158-184, 692, 702-709 | VERIFIED |
| Context menu (key 3): View / Edit / Delete | components/mod_password/src/PasswordModule.cpp:817-822 | VERIFIED |
| Delete shows confirmation dialog | components/mod_password/src/PasswordModule.cpp:793-798 | VERIFIED |
| Detail view "Type" via [Y] when keyboard connected | components/mod_password/src/PasswordModule.cpp:55, 529-540, 588-596 | VERIFIED |
| Type uses active IKeyboardProvider (core::getKeyboard) | components/mod_password/src/PasswordModule.cpp:531-534 | VERIFIED |
| Serial commands PASSWORD LIST/GET/ADD/EDIT/DEL | components/mod_password/src/PasswordModule.cpp:396-402 | VERIFIED |
| Backup export stores passwords plaintext (container encrypted) | components/mod_password/src/PasswordModule.cpp:931-933, 966-971 | VERIFIED |
| typeString(text) is the keyboard typing API | components/cdc_core/include/cdc_core/IKeyboardProvider.h:39 | VERIFIED |
| BLE HID default-enabled (mod_blehid = true) | main/module_defaults.h:24 | VERIFIED |
| USB HID default-disabled (mod_usbhid = false) | main/module_defaults.h:25 | VERIFIED |
| BLE HID provides keyboard service to other modules | components/mod_blehid/src/BleHidModule.cpp:274-277 | VERIFIED |
| BLE HID = HID Service 0x1812 with 5 characteristics (encrypted) | components/mod_blehid/src/BleHidKeyboard.cpp:236-317 | VERIFIED |
| BLE HID advertises as keyboard (appearance 0x03C1) | components/mod_blehid/src/BleHidKeyboard.cpp:66, 373 | VERIFIED |
| BLE HID menu under Settings: Status / Start-Stop Advertising / Unicode Method / Disconnect | components/mod_blehid/src/BleHidModule.cpp:157-183, 311-323 | VERIFIED |
| BLE HID status: README marks "Working" (NOT WIP) | README.md:31 | VERIFIED |
| BLE vCard & BLE Serial are WIP, untested (not BLE HID) | README.md:30, 33 | VERIFIED |
| USB HID registers HID interface on UsbManager Keyboard slot | components/mod_usbhid/src/UsbHidKeyboard.cpp:84-109 | VERIFIED |
| USB HID start fails if no free USB slot | components/mod_usbhid/src/UsbHidModule.cpp:186-192 | VERIFIED |
| USB HID takes over keyboard provider, restores previous on stop | components/mod_usbhid/src/UsbHidModule.cpp:194-217 | VERIFIED |
| USB budget: 5 IN endpoints incl EP0; CDC uses 2; at most 2 more USB interfaces active | README.md:273-276 | VERIFIED |
| USB-slot modules: fido2, gpg(CCID), usbhid, otphid | README.md:278-285 | VERIFIED |
| Default set (FIDO2+GPG) fills USB budget; disable one to enable usbhid | README.md:290-294 | VERIFIED |
| mod_usbhid and mod_otphid share one slot, mutually exclusive | README.md:294-295 | VERIFIED |
| mod_blehid uses no USB interface slot | README.md:287-288 | VERIFIED |
| Unicode methods: ASCII_ONLY / WINDOWS / LINUX / MACOS | components/cdc_keyboard/include/cdc_keyboard/KeyboardEngine.h:14-18 | VERIFIED |
| Unicode method default ASCII_ONLY | components/cdc_keyboard/include/cdc_keyboard/KeyboardEngine.h:127 | VERIFIED |
| Unicode method labels: ASCII only / Windows (Alt+Numpad) / Linux (Ctrl+Shift+U) / macOS (limited) | components/mod_blehid/src/BleHidModule.cpp:26-29; components/mod_usbhid/src/UsbHidModule.cpp:32-35 | VERIFIED |
| USB HID status menu under Settings: Status / Unicode Method | components/mod_usbhid/src/UsbHidModule.cpp:121-125, 219-231 | VERIFIED |
| 2FA menu types code via active HID keyboard | README.md:269 | VERIFIED |
| Toggling USB modules re-enumerates device | README.md:295-297 | VERIFIED |
