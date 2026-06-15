---
title: Yubico-style OTP HID challenge-response
description: The HMAC-SHA1 challenge-response feature-report protocol emulated by the USB OTP HID module.
sidebar:
  order: 10
---

The `mod_otphid` module emulates the device side of a YubiKey slot-2 HMAC-SHA1
challenge-response exchange over a USB HID feature-report channel. The HMAC
itself is delegated to the 2FA module's `IChallengeResponder` service; this
module is transport only and never hashes.

:::caution[Disabled by default]
`mod_otphid` is **default-disabled** in `main/module_defaults.h`
(`X("mod_otphid", false)`). It must be enabled before it registers a USB
interface.
:::

## USB interface and identity

On start the module acquires the UsbManager **Keyboard** interface slot and
registers a vendor-defined HID interface (HID usage page `0xFF00`, not a typing
keyboard). It presents OnlyKey's VID/PID so host tools recognize a
slot-configured OTP token:

| Field | Value |
| --- | --- |
| VID | `0x1D50` |
| PID | `0x60FC` |
| HID interface protocol | None (vendor-defined) |
| Endpoint / report size | 8 bytes |

The status view labels this identity as `OnlyKey 1D50:60FC`.

Because it takes the single Keyboard HID slot, it is mutually exclusive with
`mod_usbhid`, and the shared USB HID budget (`MAX_ACTIVE_HID = 2`) makes it
exclusive with CCID/GPG. `start()` returns cleanly when no slot is free.

## Report descriptor

The descriptor declares one 8-byte input report and one 8-byte feature report
under the vendor usage page. All status and challenge-response traffic flows
over the **feature report** channel; the input report mirrors the real device
and is unused for CR.

| HID item | Value |
| --- | --- |
| Usage Page | Vendor-defined `0xFF00` |
| Report Size | 8 bits |
| Report Count | 8 |
| Input | Data, Variable, Absolute |
| Feature | Data, Variable, Absolute |

## Status probe (GET_FEATURE, idle)

While idle, a `GET_FEATURE` request returns an emulated YubiKey status structure
(`status_st`, little-endian):

| Field | Value |
| --- | --- |
| version (major.minor.build) | 2.4.0 |
| pgmSeq | 1 |
| touchLevel | `CONFIG1_VALID \| CONFIG2_VALID \| CONFIG2_TOUCH` = `0x01 \| 0x02 \| 0x08` |

The `CONFIG2_*` bits advertise a touch-configured slot 2, which is the slot host
tools probe by default.

## Challenge write (SET_FEATURE)

The host writes a 70-byte Yubico frame as a sequence of 8-byte feature reports.
Each report carries 7 data bytes plus a trailing sequence/flag byte.

Frame layout (`FRAME_SIZE = 70`):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 64 | payload (challenge) |
| 64 | 1 | slot |
| 65 | 2 | CRC16 (little-endian) |
| 67 | 3 | filler |

Per-report trailing byte:

- Bit 7 (`SLOT_WRITE_FLAG = 0x80`) must be set for a write block. A report
  without this flag is treated as a dummy/reset and clears all CR state.
- The low 7 bits are the block index; the data goes to
  `index * 7` within the frame.
- Block 0 supersedes any half-assembled or stale frame.

When the final block (covering the slot/CRC tail) arrives, the frame is
validated:

1. **CRC16** over the 64-byte payload only is compared against the transmitted
   CRC stored little-endian after the slot byte. A mismatch resets the state.
2. The **slot byte** must equal `SLOT_CHAL_HMAC2 = 0x38` (slot-2 HMAC
   challenge-response); any other slot resets the state.

A valid frame copies the 64-byte payload as the challenge and moves to the
`PENDING` phase.

## CRC16

The CRC is CRC16 ISO13239 (`crc16` in `OtpHidCr.cpp`):

- Polynomial `0x8408`, initial value `0xFFFF`, reflected, processed LSB-first.
- The frame CRC covers the 64-byte payload.
- The residual check for the response side is the ISO13239 residual `0xF0B8`
  (referenced in the source comment).

## Response computation and touch gate

Crypto and UI run on the main task in `tick()`, never in the TinyUSB callbacks
(which only buffer state).

1. On `PENDING`, `tick()` requests the `IChallengeResponder` service and calls
   `challengeResponseUsbSlot()` with the full 64-byte payload. This resolves the
   2FA entry flagged as the USB-CR slot-2 responder.
2. The Yubico OTP-HID protocol is **SHA1-only**: the computed digest must be
   exactly 20 bytes. A non-20-byte result (for example a SHA256 entry) is
   rejected and the state returns to idle.
3. The 22-byte response is framed as the 20-byte digest plus a 2-byte CRC. The
   CRC appended is the **one's-complement** of `crc16(digest)`, stored
   little-endian, so the host's residual check over digest+CRC yields `0xF0B8`.
4. The phase moves to `WAITING`.

Touch gating uses the per-entry touch-required flag reported by the responder:

- If touch is required, the badge shows an E-Paper confirmation
  (`mod_otphid.cr_confirm`, "Allow challenge-response?"). The response is
  released to `READY` only on confirm; on cancel the state returns to idle.
- If touch is not required, the response is released immediately.

## Pending and readback (GET_FEATURE, busy)

While `PENDING` or `WAITING` (computing or awaiting touch), `GET_FEATURE`
returns a zeroed frame whose trailing byte sets `RESP_PENDING_FLAG = 0x40` with
sequence 0, so the host keeps polling — matching how a real YubiKey reports a
pending touch.

Once `READY`, each `GET_FEATURE` streams the next 7 bytes of the 22-byte
response:

- Data is copied from `seq * 7` within the response.
- The trailing byte is `RESP_PENDING_FLAG | (seq & SEQ_MASK)` where
  `SEQ_MASK = 0x1F`.
- When the offset reaches the end, a final zeroed frame (sequence 0, no pending
  flag) signals completion and the state returns to idle.

## KeePassXC / ykchalresp compatibility

The module header and source state that this implements the device side of the
YubiKey slot-2 challenge-response that KeePassXC / ykchalresp speak over the OTP
HID feature-report channel, and that KeePassXC uses a fixed 64-byte challenge
input (which is why the full 64-byte payload is HMACed). The protocol references
cited in the source are the `yubikey-personalization` (`ykdef.h`, `ykcore.c`)
and `yubico-c` (`ykcrc.c`) projects.

## GAPs

- **Host-side verification.** Compatibility with KeePassXC / ykchalresp /
  ykinfo is asserted by the source comments and the emulated VID/PID + status
  structure; this page does not include an observed end-to-end test capture.
- **`isConnected()` semantics.** `isConnected()` returns true once the interface
  is registered and `usb_hid_ready()` reports ready; the exact host-enumeration
  condition behind `usb_hid_ready()` is outside `mod_otphid` and not documented
  here.
- **Input report use.** The descriptor declares an 8-byte input report that the
  source describes as mirroring the real device and unused for CR; no behaviour
  is defined for it in this module.
