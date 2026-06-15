# Documentation Reconciliation Tracker

Gate C4 instrument. Records the page-by-page reconciliation of `website/src/content/docs/**`
against the CODE (code is the ground truth; the docs were freshly generated). Status:
`fixed` (doc contradicted code, corrected), `matches` (verified against code), `logged`
(noted, not a contradiction or needs a code-side change out of this transition's scope).

Outcome: the freshly generated docs proved **highly faithful** to the code. Only a small number
of real contradictions were found and fixed; the rest verified as matching.

## Confirmed Category-A fixes (pre-identified)

| ID | Page | Code reference | Status | Note |
|----|------|----------------|--------|------|
| D1 | power/serial-console.md | feature_flags.h L18-24; ADR-0012 | fixed | Was "enabled by default" (3 spots); `FEATURE_SECURE_SERIAL` defaults 0/off unless Kconfig `CONFIG_SECURE_SERIAL`. |
| D2 | security/pin-lockout.md, secure-element-keys.md | PinManager.cpp; ctap2.cpp; ADR-0004 | fixed | Added "protocol-driven, not an inconsistency" note for the two PIN KDFs. |
| D3 | security/caveats.md | PinManager.cpp; feature_flags.h; ADR-0012 | fixed | Added ADR-0012 cite confirming DEBUG_MODE does not bypass badge-PIN lockout recovery. |

## Per-area sweep findings

| Page(s) | Code reference | Status | Note |
|---------|----------------|--------|------|
| dev/proto/backup-format.md | platformio.ini APP_VERSION | fixed | `fw_version` example `0.6.4` → `0.6.5`. |
| guide/auto-type.md | tusb_config.h; UsbManager.h::MAX_ACTIVE_HID | fixed | Corrected wrong USB-endpoint reasoning for the 2-HID-interface limit. |
| guide/settings.md | DateInputView::onKey | fixed | Removed bogus `4=Prev / 6=Next` date-field keys (only digits/N/Y handled). |
| start/overview.md, start/first-flash.md | README.md; WifiMenuUi.cpp | fixed | WiFi described as on-device+serial (not serial-only); hardware `v1.0` → `v1.0/v1.1`. |
| security/* (overview, attestation, duress, pin-lockout, secure-element-keys) | PinManager/FactoryReset/ISecureElement/ctap2; tropic_slot_map.h | matches | AAGUID, attestation cert, slot map, R-Mem 444-475, PIN limits, duress wipe all verified. |
| dev/* (architecture, build-system, secure-element, ui-framework, module-development, host-api, plugin-sdk, api-reference) | headers + host_api.h | matches | Host API level 0.7, 213/213 symbol bijection, partition table, ViewStack depths, capability table all verified. |
| dev/proto/* (fido2-ctap, openpgp-ccid, otp-hid-cr, vcard, message-transfer, gpg-cross-signing, serial-commands) | module sources | matches | AAGUID, VID/PID, framing limits, RFC 4880 flow, kMaxKeys 128 all verified. |
| guide/* (fido2, two-factor, password-vault, gpg-ssh, vcard, backup-restore, bluetooth, lock-and-pin, power-sleep, wifi-time) | module sources | matches | PIN 4-8 / default 123456 / 60s lockout, slot/entry limits (100/369/26), timeouts all verified. |
| power/* (plugins/*, expert-menu, languages, companion-tools, storage-tools) | PluginManifest/CapabilityChecker/GpioPolicy; tools/*.py | matches | WAMR 2.4.4, 2MB partition, mem 64/16-4096KB, GPIO block/whitelist, NVS rules all verified. |
| start/* (first-boot, keypad-navigation) | LockScreenView; TCA9535Keypad; T9InputView | matches | Default PIN, status icons, 12-key model, rescue chord, T9 timing verified. |

## Logged items (not fixed under this transition)

| Item | Where | Reason |
|------|-------|--------|
| `MAX_REGISTERED_SERVICES` | code vs MEMORY.md | Code is **7** (`IBluetoothController.h:157`); the prior MEMORY note said 4. Docs/code consistent; the stale note was corrected in MEMORY.md. |
| Duress wipe wording "every slot is erased" | security/duress.md | Loose wording: code only erases slots in use (`eccSlotUsed`/`rmemSlotUsed`). Functional scope identical; not a contradiction. |
| Doxyfile lists `components/mod_hid` | dev/api-reference.md | Doc faithfully transcribes the Doxyfile; the **Doxyfile** is stale (module is now `mod_blehid`/`mod_usbhid`). Firmware/config edit, out of this transition's scope → RF backlog. |
| Hardware v1.0 vs v1.0/v1.1 | start/* | No v1.0/v1.1 distinction exists in code; README is the authoritative source for the revision string. |
