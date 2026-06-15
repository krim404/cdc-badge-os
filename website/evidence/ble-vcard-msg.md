# Evidence ledger: Bluetooth / cdc_msg message transfer / vCard

Tags: VERIFIED = exact source line supports the claim; GAP = not verifiable in source.

All paths are relative to the repo root `~/GIT/cdc-badge-os`.

## Bluetooth menu and controller (guide/bluetooth.md)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Bluetooth menu items: enable, pair, paired, status, scan, beacon, forget bonds | components/cdc_os_ui/src/BluetoothMenuUi.cpp:25-34 | VERIFIED |
| Enable toggle shows `*` + "Bluetooth ON" / "Bluetooth OFF" | components/cdc_os_ui/src/BluetoothMenuUi.cpp:149-153 | VERIFIED |
| Paired/Scan/Forget-bonds rows disabled while BLE off | components/cdc_os_ui/src/BluetoothMenuUi.cpp:155,157,164 | VERIFIED |
| Toggle calls enable()/disable() | components/cdc_os_ui/src/BluetoothMenuUi.cpp:263-272 | VERIFIED |
| Pair device pushes BlePairingView | components/cdc_os_ui/src/BluetoothMenuUi.cpp:207-211 | VERIFIED |
| Pairing view enables BLE if off, sets discoverable | components/cdc_os_ui/src/views/BlePairingView.cpp:24-27 | VERIFIED |
| Pairing view keeps awake / does not auto-lock | components/cdc_os_ui/src/views/BlePairingView.cpp:38-41 | VERIFIED |
| Pairing view shows advertised name, waiting/connected | components/cdc_os_ui/src/views/BlePairingView.cpp:82-93 | VERIFIED |
| Pairing view N = leave | components/cdc_os_ui/src/views/BlePairingView.cpp:54-56 | VERIFIED |
| Pairing uses numeric comparison, six-digit code, Y/N, auto-reject on timeout | components/cdc_os_ui/include/cdc_os_ui/views/BlePairingPromptView.h:8-18 | VERIFIED |
| Numeric-comparison default timeout 30000 ms | components/cdc_os_ui/include/cdc_os_ui/views/BlePairingPromptView.h:27 | VERIFIED |
| Numeric comparison is the controller pairing callback type | components/cdc_hal/include/cdc_hal/IBluetoothController.h:356,385 | VERIFIED |
| Paired list shows identity addr + pub/rnd, `*` if connected | components/cdc_os_ui/src/BluetoothMenuUi.cpp:666-675 | VERIFIED |
| Max 5 bonded devices | components/cdc_hal/include/cdc_hal/IBluetoothController.h:228 | VERIFIED |
| Forget one device (confirm) | components/cdc_os_ui/src/BluetoothMenuUi.cpp:686-705 | VERIFIED |
| Forget all bonds via clearAllBonds() with confirm | components/cdc_os_ui/src/BluetoothMenuUi.cpp:227-235 | VERIFIED |
| Scan timeout 8000 ms | components/cdc_os_ui/src/BluetoothMenuUi.cpp:40 | VERIFIED |
| Scan results sorted by RSSI descending | components/cdc_os_ui/src/BluetoothMenuUi.cpp:130-140,628 | VERIFIED |
| Scan row shows signal bars, name, RSSI | components/cdc_os_ui/src/BluetoothMenuUi.cpp:106-122 | VERIFIED |
| Unnamed devices show MAC, then async Device Name resolve (0x1800/0x2A00) | components/cdc_os_ui/src/BluetoothMenuUi.cpp:321-336,367-378 | VERIFIED |
| Name resolve does not bond (read-only) | components/cdc_os_ui/src/BluetoothMenuUi.cpp:413-424,558-571 | VERIFIED |
| BLE Status shows ON/OFF, MAC, connected+RSSI, name | components/cdc_os_ui/src/BluetoothMenuUi.cpp:300-318 | VERIFIED |
| Beacon row in Bluetooth menu toggles beacon, `*` when active | components/cdc_os_ui/src/BluetoothMenuUi.cpp:159-163,221-225 | VERIFIED |

## Beacon (guide/bluetooth.md, message-transfer.md)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Beacon submenu: toggle, name, scan | components/cdc_os_ui/src/MsgTransferUi.cpp:316-326 | VERIFIED |
| Beacon name default = badge name; custom persisted | components/cdc_msg/src/BeaconManager.cpp:52-58 | VERIFIED |
| Beacon advertises service UUID + Complete Local Name, no mfg data | components/cdc_msg/include/cdc_msg/BeaconManager.h:7-14; components/cdc_msg/src/BeaconManager.cpp:107-116 | VERIFIED |
| Beacon default ON | components/cdc_msg/src/BeaconManager.cpp:43-44 | VERIFIED |
| Enabling beacon auto-enables BLE | components/cdc_msg/src/BeaconManager.cpp:73-77 | VERIFIED |
| Beacon scan filters peers by service UUID | components/cdc_msg/src/MessageTransfer.cpp:403-405 | VERIFIED |

## cdc_msg framework (dev/proto/message-transfer.md)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Generic MIME-typed transfer, headless core, UI renders prompt | components/cdc_msg/include/cdc_msg/MessageTransfer.h:20-32 | VERIFIED |
| Ephemeral pairing (numeric comparison), forgotten after transfer | components/cdc_msg/include/cdc_msg/MessageTransfer.h:26-27 | VERIFIED |
| 128-bit random base UUID, discriminator at index 12, NUS-like | components/cdc_msg/include/cdc_msg/MessageProfile.h:5-13 | VERIFIED |
| Service UUID bytes (CDC50001-...) | components/cdc_msg/include/cdc_msg/MessageProfile.h:17-21 | VERIFIED |
| Control UUID (CDC50002-...) | components/cdc_msg/include/cdc_msg/MessageProfile.h:23-27 | VERIFIED |
| Status UUID (CDC50003-...) | components/cdc_msg/include/cdc_msg/MessageProfile.h:29-33 | VERIFIED |
| Data UUID (CDC50004-...) | components/cdc_msg/include/cdc_msg/MessageProfile.h:35-39 | VERIFIED |
| Control = plaintext WRITE; Status = plaintext NOTIFY; Data = encrypted WRITE | components/cdc_msg/src/MessageTransfer.cpp:131-163 | VERIFIED |
| Data permission WRITE_ENC | components/cdc_msg/src/MessageTransfer.cpp:156 | VERIFIED |
| Little-endian, bounds-checked, version 1 rejected on mismatch | components/cdc_msg/include/cdc_msg/MessageTypes.h:12-21 | VERIFIED |
| Control opcodes Offer 0x01 / Abort 0x02 | components/cdc_msg/include/cdc_msg/MessageTypes.h:24-27 | VERIFIED |
| Data opcodes Chunk 0x10 / Complete 0x11 with layouts | components/cdc_msg/include/cdc_msg/MessageTypes.h:30-33 | VERIFIED |
| Status opcodes Accept..Queued (0x01-0x07) with layouts | components/cdc_msg/include/cdc_msg/MessageTypes.h:36-44 | VERIFIED |
| Reason codes None(0)..PairFailed(9) | components/cdc_msg/include/cdc_msg/MessageTypes.h:47-58 | VERIFIED |
| OFFER header 8 bytes; CHUNK header 2; COMPLETE 6 | components/cdc_msg/include/cdc_msg/MessageTypes.h:86-91 | VERIFIED |
| OFFER fields: op,ver,u32 totalLen,mimeLen,nameLen,mime,name | components/cdc_msg/src/MessageTransfer.cpp:510-533,1146-1155 | VERIFIED |
| OFFER bounds validation (ver, mimeLen>0/<=63, nameLen<=31, fits frame) | components/cdc_msg/src/MessageTransfer.cpp:506-524 | VERIFIED |
| TooLarge if totalLen 0 or > 4096 | components/cdc_msg/src/MessageTransfer.cpp:521-524 | VERIFIED |
| Chunk subtraction-form bounds guard (no wrap) | components/cdc_msg/src/MessageTransfer.cpp:566-573 | VERIFIED |
| Complete requires exact totalLen, CRC verified in tick | components/cdc_msg/src/MessageTransfer.cpp:584-597,957-961 | VERIFIED |
| Progress throttled every 512 bytes (and final) | components/cdc_msg/src/MessageTransfer.cpp:34-35,579-583 | VERIFIED |
| Sender flow: connect->discover->offer->encrypt->stream | components/cdc_msg/src/MessageTransfer.cpp:1087-1219 | VERIFIED |
| Sender locates ctrl/status/data handles; CCCD = statusHandle+1 | components/cdc_msg/src/MessageTransfer.cpp:652-669 | VERIFIED |
| Sender enables notifications then writes OFFER | components/cdc_msg/src/MessageTransfer.cpp:1128,1156-1158 | VERIFIED |
| OFFER bounded to MTU + frame buf; name truncated; fail BadFrame if MIME won't fit | components/cdc_msg/src/MessageTransfer.cpp:1134-1145 | VERIFIED |
| Receiver declines NoHandler if no live/deferred handler | components/cdc_msg/src/MessageTransfer.cpp:535-538 | VERIFIED |
| On Accept sender calls initiateSecurity() (numeric comparison) | components/cdc_msg/src/MessageTransfer.cpp:1170-1178 | VERIFIED |
| Receiver allocates PSRAM buffer on encryption, moves to Receiving | components/cdc_msg/src/MessageTransfer.cpp:742-761 | VERIFIED |
| Chunks sized to MTU, clamped to stack frame buffer | components/cdc_msg/src/MessageTransfer.cpp:1050-1059 | VERIFIED |
| Complete carries CRC32 | components/cdc_msg/src/MessageTransfer.cpp:1075-1084 | VERIFIED |
| Delivery via registry or deferred resolver, notify Done, grace period | components/cdc_msg/src/MessageTransfer.cpp:962-996; :33 (kDoneGraceMs=700) | VERIFIED |
| Sender state enum | components/cdc_msg/include/cdc_msg/MessageTypes.h:60-63 | VERIFIED |
| Receiver state enum | components/cdc_msg/include/cdc_msg/MessageTypes.h:65-68 | VERIFIED |
| Ephemeral bond recorded on encrypt, forgotten on disconnect | components/cdc_msg/src/MessageTransfer.cpp:1265-1297,615-634 | VERIFIED |
| Bond table sized to connections; full table logged warning | components/cdc_msg/src/MessageTransfer.cpp:1283-1285 | VERIFIED |
| Max payload 4096 | components/cdc_msg/include/cdc_msg/MessageTypes.h:74 | VERIFIED |
| MIME max 63 (buf 64) | components/cdc_msg/include/cdc_msg/MessageTypes.h:77-79 | VERIFIED |
| Peer name max 31 (buf 32) | components/cdc_msg/include/cdc_msg/MessageTypes.h:82-84 | VERIFIED |
| Max registered handlers 8 | components/cdc_msg/include/cdc_msg/MessageHandlerRegistry.h:64 | VERIFIED |
| Offer queue depth 4 | components/cdc_msg/include/cdc_msg/MessageTypes.h:104 | VERIFIED |
| Timeouts: connect 10s, discovery 5s, consent 30s, encrypt 30s, idle 8s | components/cdc_msg/include/cdc_msg/MessageTypes.h:95-99 | VERIFIED |
| Global prompt budget 5 / 30 s | components/cdc_msg/include/cdc_msg/MessageTypes.h:113-114 | VERIFIED |
| Quiet cooldown 20 s after decline | components/cdc_msg/include/cdc_msg/MessageTypes.h:116; src/MessageTransfer.cpp:464 | VERIFIED |
| Per-connection rate: 3 offers / 10 s, table 8 | components/cdc_msg/include/cdc_msg/MessageTypes.h:119-121 | VERIFIED |
| Full queue replies Busy | components/cdc_msg/src/MessageTransfer.cpp:813-815 | VERIFIED |
| registerHandler(mime, descKey, deliver) signature | components/cdc_msg/include/cdc_msg/MessageTransfer.h:42 | VERIFIED |
| DeliverFn signature + untrusted/not-NUL contract | components/cdc_msg/include/cdc_msg/MessageHandlerRegistry.h:11-22 | VERIFIED |
| beginInteractiveSend / sendTo behaviour, PSRAM copy, refuse if busy | components/cdc_msg/src/MessageTransfer.cpp:262-323 | VERIFIED |
| Deferred handler resolver (canHandle + deferredDeliver) | components/cdc_msg/include/cdc_msg/MessageTransfer.h:48-58; src/MessageTransfer.cpp:241-246 | VERIFIED |
| Deferred resolver installed by plugin manager (plg_msg_init) | components/plugin_manager/src/host_api_msg.cpp:259-265 | VERIFIED |
| Events BLE_CONSENT_REQUEST / BLE_EXCHANGE_COMPLETE published | components/cdc_msg/src/MessageTransfer.cpp:928,977-978,1009,1239 | VERIFIED |
| UI subscribes to both events | components/cdc_os_ui/src/MsgTransferUi.cpp:367-374 | VERIFIED |
| Service inited before modules so they can register handlers | main/main.cpp:450-462 | VERIFIED |

## Plugin host API (message-transfer.md)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| host_msg_register_handler / unregister / consume / send_interactive / send | components/plugin_manager/include/plugin_manager/host_api.h:1360,1367,1380,1394,1408 | VERIFIED |
| HOST_MSG_PAYLOAD_MAX 4096 / HOST_MSG_MIME_MAX 64 | components/plugin_manager/include/plugin_manager/host_api.h:1344,1346 | VERIFIED |
| Plugin handler descKey = core.msg_text (text/*) else core.msg_data | components/plugin_manager/src/host_api_msg.cpp:203-204 | VERIFIED |

## vCard over cdc_msg (guide/vcard.md, dev/proto/vcard.md)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| MIME type text/vcard | components/mod_vcard/src/VcardModule.cpp:218,480 | VERIFIED |
| Handler registered in init with descKey mod_vcard.received | components/mod_vcard/src/VcardModule.cpp:479-481 | VERIFIED |
| descKey mod_vcard.received -> "Contact (vCard)" | components/mod_vcard/src/VcardModule.cpp:69 | VERIFIED |
| Handler unregistered in stop | components/mod_vcard/src/VcardModule.cpp:503-505 | VERIFIED |
| deliverVcard: reject empty / > VCARD_MAX_LEN, copy+NUL, store | components/mod_vcard/src/VcardModule.cpp:155-167 | VERIFIED |
| Store dedups on exact text (no-op upsert) | components/mod_vcard/src/VcardModule.cpp:614-626; include/mod_vcard/vcard_store.h:44-50 | VERIFIED |
| Send vCard via beginInteractiveSend("text/vcard",...) | components/mod_vcard/src/VcardModule.cpp:209-222 | VERIFIED |
| No own card -> "No vCard set" hint, no send | components/mod_vcard/src/VcardModule.cpp:213-216 | VERIFIED |
| VCARD_MAX_LEN 768, VCARD_MAX_CARDS 100 | components/mod_vcard/include/mod_vcard/vcard_store.h:6-7 | VERIFIED |
| vCards menu: My vCard / Edit my vCard / Send vCard | components/mod_vcard/src/VcardModule.cpp:136-141,172-177 | VERIFIED |
| Editor field list/order (16 fields) | components/mod_vcard/src/VcardModule.cpp:111-116; vcard_store.h:15-32 | VERIFIED |
| Editor field labels | components/mod_vcard/src/VcardModule.cpp:52-67 | VERIFIED |
| "Saved" on finish | components/mod_vcard/src/VcardModule.cpp:51,477 | VERIFIED |
| Serial VCARD SET/GET/DELETE; paste ends with --- / ABORT; 30 s idle | components/mod_vcard/src/VcardModule.cpp:284,327,342,366-374,434-438 | VERIFIED |
| Lock-screen quick action "My vCard" shows QR; error if none | components/mod_vcard/src/VcardModule.cpp:233-272,543-552 | VERIFIED |
| vCard 4.0 text; struct->vcard fields | components/mod_vcard/src/VcardModule.cpp:388-405; vcard_store.h:57-73 | VERIFIED |
| FN falls back to "given family" when empty; empty fields omitted | components/mod_vcard/include/mod_vcard/vcard_store.h:64-73 | VERIFIED |
| Backup own + received[], schema_ver 1, upsert by text | components/mod_vcard/src/VcardModule.cpp:562-666 | VERIFIED |

## Consent / progress UI (guide/vcard.md)

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| Incoming transfer prompt shows peer name, what (descKey), size | components/cdc_os_ui/src/MsgTransferUi.cpp:265-281 | VERIFIED |
| Y accept / N decline consent | components/cdc_os_ui/src/MsgTransferUi.cpp:245-255,279 | VERIFIED |
| Locked badge auto-declines offers | components/cdc_os_ui/src/MsgTransferUi.cpp:257-264 | VERIFIED |
| Progress view shows percentage bar; N cancels send | components/cdc_os_ui/src/MsgTransferUi.cpp:60-117 | VERIFIED |
| Peer picker owned by UI; confirmInteractiveTarget then progress | components/cdc_os_ui/src/MsgTransferUi.cpp:200-218 | VERIFIED |
| Completion toasts: ok / declined / unsupported / fail | components/cdc_os_ui/src/MsgTransferUi.cpp:296-306 | VERIFIED |

## WIP status

| Claim | Source (path:line) | Tag |
| --- | --- | --- |
| BLE vCard "WIP, untested on hardware" | README.md:30 | VERIFIED |
| BLE Serial "WIP, untested on hardware" | README.md:33 | VERIFIED |

## GAPs

| Topic | Note | Tag |
| --- | --- | --- |
| On-hardware maturity | Whole cdc_msg framework + vCard exchange not verified on hardware; README flags BLE vCard/Serial as WIP/untested. | GAP |
| Other modules using cdc_msg | Only mod_vcard registers a firmware handler (text/vcard); no other module found via registerHandler. Plugins may register via host API. | GAP |
| Service UUID canonical string | MessageProfile.h gives the byte array (LE) and a comment "CDC5000x-B0A7-4E3D-9C82-5A6F1B3E9D2C"; the per-characteristic canonical strings (CDC50002/3/4) are inferred from the discriminator comment, not spelled out byte-for-byte as strings. | GAP |
