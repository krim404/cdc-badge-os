---
title: Two-factor codes (TOTP / HOTP / challenge-response)
description: Adding OATH accounts, viewing codes, and using HMAC challenge-response on the badge.
sidebar:
  order: 2
---

The 2FA module turns the badge into an OATH authenticator. It stores TOTP and
HOTP accounts, renders their codes on the E-Paper display, and can answer raw
HMAC challenge-response requests. The module is enabled by default.

Account secrets are held in the TROPIC01 secure element, not in plain flash.

## Account types

The module supports three OATH entry types:

| Type | Meaning |
| --- | --- |
| TOTP | Time-based one-time password (counter derived from the clock). |
| HOTP | Counter-based one-time password (counter advances per use). |
| CR | Raw HMAC challenge-response (no displayed code; answered over a transport). |

## Algorithms, digits and period

These limits are enforced when an account is saved.

| Setting | Supported values |
| --- | --- |
| Hash algorithm (TOTP / HOTP) | SHA1, SHA256, SHA512 |
| Hash algorithm (CR) | SHA1, SHA256 only |
| Digits (TOTP / HOTP) | 6, 7 or 8 (default 6) |
| TOTP period | 15 to 300 seconds (default 30) |

A zero digit count falls back to 6 digits; a zero TOTP period falls back to 30
seconds. Values outside the allowed range are rejected.

The on-device wizard offers a subset of these: digits 6/7/8, algorithm SHA1 /
SHA256 / SHA512 (SHA1 / SHA256 for CR), and period 30 s or 60 s. The serial
interface accepts any value inside the limits above.

## Capacity

The 2FA module owns 100 secure-element R-Memory slots (slots 32 to 131), so up
to 100 OATH accounts can be stored.

## Adding an account

There are two ways to add an account: on the badge itself or over the serial
console.

### On the badge

Open the 2FA app from the main menu and select **Add Account**. The wizard walks
through these steps:

1. **Type** — TOTP, HOTP or CR.
2. **Account Name** — the label shown in the list (up to 16 characters).
3. **Secret (Base32)** — the shared secret, Base32-encoded.
4. **Issuer (optional)** — only for TOTP/HOTP (skipped for CR).
5. **Digits** — 6, 7 or 8 (only for TOTP/HOTP).
6. **Algorithm** — SHA1 / SHA256 / SHA512 (SHA1 / SHA256 for CR).
7. **Period** — 30 s or 60 s (TOTP only).

For CR entries the wizard instead asks for a **Touch confirm** choice and a
**USB slot 2** designation (see below).

Text is entered with the T9 input view. The name and secret are both required;
an empty name or secret aborts the save.

### Over the serial console

Connect at 115200 baud and use the `TOTP` command group (the serial path is
PIN/AUTH-gated):

```text
TOTP ADD <type> <name> <secret> [issuer] [digits] [period] [algo] [counter]
TOTP LIST
TOTP GET <index>
TOTP DEL <index>
```

- `<type>` is `totp`, `hotp` or `cr`.
- `<secret>` is the Base32 secret.
- `<algo>` is `sha1`, `sha256` or `sha512`.
- `[counter]` sets the initial HOTP moving factor (ignored for TOTP).

`TOTP GET <index>` prints the current code. For HOTP it also advances and
persists the counter.

## Viewing codes

Select an account from the list to open the code view.

- **TOTP** shows the current code in large digits with a progress bar and a
  remaining-seconds countdown for the current step. The code refreshes once per
  second.
- **HOTP** shows the code for the current counter value and the counter that
  produced it. Press <kbd>5</kbd> to advance to the next code (this consumes and
  persists one counter step).
- **CR** entries have no displayable code; they are answered over a transport
  (see below).

If a USB keyboard is connected, press <kbd>Y</kbd> to type the displayed code on
the host. Press <kbd>3</kbd> to edit the entry, or <kbd>N</kbd> to go back.

## TOTP needs a valid clock

TOTP codes depend on the system time. The badge treats the clock as valid only
when the date is **2024 or later**. While the clock is unset, TOTP entries show
a "Time not set" message and dashes instead of a code; HOTP entries are
unaffected because they do not use the clock.

## Challenge-response (CR) entries

A CR entry stores an HMAC key and answers a raw `HMAC(secret, challenge)`
request without truncation. The response is 20 bytes for SHA1 or 32 bytes for
SHA256.

CR requests can be served over three paths:

- **Serial** — `CHALRESP <name> <hex-challenge>` computes the response for the
  named CR entry and prints it as hex. This path is AUTH-gated and does not ask
  for touch confirmation.
- **BLE** — a GATT service accepts a challenge and notifies the response.
- **USB OTP HID** — a Yubico-style slot-2 responder (requires the separate USB
  OTP HID module; see the developer protocol page).

### Per-entry touch confirmation

When you add a CR entry you can set **Touch confirm** to *Required*. When
required, the badge shows an on-device confirmation prompt before releasing the
response over the BLE and USB transports; the request is only answered after you
confirm. The serial `CHALRESP` path does not apply this gate.

### USB slot 2 designation

A CR entry can be designated as the single **USB slot 2** responder. The USB OTP
HID transport answers with whichever entry carries this designation. Designating
a new entry clears the designation from any previous one, so exactly one entry
is ever the USB responder.

:::note
The USB OTP HID module that consumes the USB-slot-2 entry is disabled by default
and must be enabled separately. Its challenge-response is SHA1-only. See the
[Yubico-style OTP HID challenge-response](/dev/proto/otp-hid-cr/)
protocol page.
:::
