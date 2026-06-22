---
title: Bluetooth
description: Enabling Bluetooth, pairing a host, managing bonded devices, scanning and the beacon for badge-to-badge transfer.
sidebar:
  order: 8
---

The badge has a single Bluetooth Low Energy (BLE) controller that several
features share: the **BLE keyboard** (auto-type), badge-to-badge **transfer**
(the beacon, used by [vCard exchange](/guide/vcard/)), and the **BLE serial**
console. They all run on top of the same controller, so turning Bluetooth on or
off affects all of them at once.

You reach everything from **Tools -> Bluetooth**.

## Turning Bluetooth on and off

The first item in the Bluetooth menu toggles the controller. When it is on the
row shows a `*` marker and reads **Bluetooth ON**; when off it reads
**Bluetooth OFF**. While Bluetooth is off, the **Paired devices**, **Scan
Devices** and **Forget all bonds** rows are disabled.

Some features turn Bluetooth on by themselves when you start them:

- Entering **Pair device** enables the controller if it was off.
- Enabling the **beacon** (for badge-to-badge transfer) enables the controller
  if it was off.

## Pairing a host (Pair device)

Select **Pair device** to put the badge into pairing mode. This screen makes the
badge **discoverable** so a computer or phone can start bonding, and shows the
badge's advertised name. While the screen is open the badge stays awake and
does not auto-lock, so a pairing prompt is never rejected because the badge
locked.

The pairing screen shows **Waiting for device...** until a host connects, then
**Connected**. Press <kbd>N</kbd> to leave pairing mode.

### Confirming the code (numeric comparison)

Pairing uses **numeric comparison**. When the host requests it, the badge shows
a six-digit confirmation code. Check that it matches the code shown on the host,
then:

- <kbd>Y</kbd> accepts the pairing.
- <kbd>N</kbd> rejects it.

If you do nothing, the prompt rejects automatically after a timeout (30 seconds).

:::note
This is the same numeric-comparison confirmation that badge-to-badge transfer
uses, just initiated by a host instead of another badge.
:::

## Paired devices (bonds)

Select **Paired devices** to list the hosts the badge has bonded with. Each row
shows the device's identity address and whether it is `pub` (public) or `rnd`
(random); a connected device is marked with `*`.

The badge keeps at most **5** bonded devices.

To remove a single bond, select it and confirm **Forget this device?**. To wipe
every bond at once, use **Forget all bonds** from the Bluetooth menu and confirm.

## Scanning for nearby devices (Scan Devices)

**Scan Devices** runs an 8-second BLE scan and lists what it finds, sorted by
signal strength (strongest first). Each row shows a signal-bar icon, the device
name and its RSSI in dBm.

Devices that do not advertise a name initially show their MAC address. While the
results are on screen the badge tries, one device at a time, to connect and read
each unnamed device's GATT *Device Name*, then updates the row in place. This is
read-only: it does not pair with or bond to the scanned devices.

## The beacon (badge-to-badge transfer)

The badge-to-badge transfer **beacon** has its own submenu, reached from the
Tools menu. When on, the badge advertises the message-transfer service so other
badges can find it and send it a contact card or other typed payload. The beacon
keeps advertising while you scan and resumes automatically after a transfer, so
the badge stays discoverable without a manual reset.

The beacon submenu has three items:

- **Beacon** toggle (on/off) — shows **Beacon: on** (with a `*` when actively
  advertising) or **Beacon: off**.
- **Beacon name** — the name other badges see; defaults to the badge name.
- **Beacon scan** — a read-only scan that continuously lists other badges
  advertising the transfer service, with their signal strength. It runs as a
  multi-role scan: the badge keeps advertising its own beacon while scanning, so
  two badges with the scan open still discover each other. The list updates live
  as badges come and go, and shows **Searching for badges...** while none are in
  range.

Most transfers confirm the numeric-comparison code on every send. Some, such as
a back-and-forth messenger, remember the pairing for the current session: you
confirm the code once, and further sends to the same badge go through without a
prompt. This trust lasts only until the badge reboots or Bluetooth is turned
off.

See [vCard exchange](/guide/vcard/) for the full send/receive flow, and the
developer page [cdc_msg message-transfer protocol](/dev/proto/message-transfer/)
for the wire details.

## BLE status

**BLE Status** shows a read-only summary: whether Bluetooth is ON or OFF, the
badge's MAC address, whether a device is connected (with the connected RSSI in
dBm), and the badge's advertised name.

## Features that use Bluetooth

- **BLE keyboard** for [auto-type](/guide/auto-type/) — pair via **Pair device**.
- **Badge-to-badge transfer** (the beacon) — used by
  [vCard exchange](/guide/vcard/).
- **BLE serial** console.
