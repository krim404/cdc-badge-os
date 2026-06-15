---
title: Auto-type via HID keyboard
description: Type passwords and 2FA codes from the badge as a keyboard over BLE or USB.
sidebar:
  order: 6
---

The badge can act as a keyboard and type stored data into a connected device.
This is used to type a stored password from the
[password vault](/guide/password-vault/) and to type a 2FA code
from the 2FA menu. Two transports provide the keyboard: Bluetooth (BLE HID) and USB.

Whichever transport is active provides the keyboard to the rest of the
firmware, so the **Type** action and the 2FA menu use whatever keyboard is
currently connected.

## BLE keyboard (default)

The Bluetooth keyboard module (`mod_blehid`) is **enabled by default**. It
advertises the badge as a HID keyboard (the standard HID over GATT service) and
delivers keystrokes over an encrypted connection. It does not use a USB
interface slot, so it works alongside the USB FIDO2 and GPG functions.

To use it, open **Settings &rarr; BLE Keyboard**:

| Menu item | Action |
|-----------|--------|
| Status | Show connection state and the selected Unicode method. |
| Start / Stop Advertising | Make the badge discoverable as a keyboard, or stop. |
| Unicode Method | Choose how non-ASCII characters are typed (see below). |
| Disconnect | Disconnect the current host (only when connected). |

Pair from the host's Bluetooth settings while the badge is advertising. Once
connected, open a password entry and press <kbd>Y</kbd> to type it, or use the
2FA menu to type a code.

## USB keyboard

The USB keyboard module (`mod_usbhid`) is **disabled by default**. When enabled
and started, it registers a USB HID keyboard interface and takes over as the
active keyboard, restoring the previous keyboard (for example BLE) when it
stops.

Its menu lives under **Settings &rarr; USB Keyboard**:

| Menu item | Action |
|-----------|--------|
| Status | Show whether the interface is active, waiting for the host, or connected. |
| Unicode Method | Choose how non-ASCII characters are typed (see below). |

### USB interface budget

The badge enumerates as a single composite USB device. Alongside the always-on
serial console (CDC), the firmware arbitrates the optional HID-class and
smartcard (CCID) interfaces and allows **at most two of them active at the same
time**.

These modules each need one of those two interface slots:

| Module | USB interface | Default |
|--------|---------------|---------|
| FIDO2 | FIDO2 / U2F HID | enabled |
| GPG | OpenPGP smartcard (CCID) | enabled |
| USB keyboard (`mod_usbhid`) | USB keyboard | disabled |
| OTP HID (`mod_otphid`) | Yubico OTP challenge-response | disabled |

With the default set (FIDO2 + GPG) the USB budget is already full. To enable the
USB keyboard you must first disable one of the active USB modules, under
**Tools &rarr; Expert &rarr; Modules** or over serial
(`MODULE DISABLE <name>` / `MODULE ENABLE <name>`). Otherwise starting it fails
with "No free USB slot".

:::note
The USB keyboard and the OTP HID module share the same interface slot and cannot
run at the same time. Toggling USB modules re-enumerates the device; replug the
USB cable if the host does not pick up the new configuration.

The BLE keyboard uses **no** USB interface slot, so it is unaffected by the USB
budget.
:::

## Unicode method

Both keyboards offer the same setting for how non-ASCII characters (for example
accented letters) are typed. The default is **ASCII only**.

| Method | Behaviour |
|--------|-----------|
| ASCII only | Fallback transliteration (for example `ae`, `oe`, `ue`). Works everywhere. |
| Windows (Alt+Numpad) | Alt + numeric-keypad code sequences. |
| Linux (Ctrl+Shift+U) | Ctrl+Shift+U hex entry. |
| macOS (limited) | Option-based sequences (limited coverage). |

Pick the method that matches the operating system you are typing into. If in
doubt, **ASCII only** is the safe choice for passwords made of plain ASCII
characters.

## Typing a password

1. Connect a keyboard transport (pair over BLE, or enable and connect the USB
   keyboard).
2. Open the [password vault](/guide/password-vault/) and select an entry.
3. In the detail view, press <kbd>Y</kbd> to type the password into the focused
   field on the connected device.

If no keyboard is connected, the detail view shows "No keyboard connected"
instead of a Type action.
