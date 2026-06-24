---
title: Wi-Fi & time
description: Connecting the badge to Wi-Fi and synchronising the clock over NTP.
sidebar:
  order: 5
---

The badge can join a Wi-Fi network and use it to set its clock automatically.
Both are handled from **Tools → WiFi**.

The Wi-Fi menu has four entries:

1. **Connect** / **Disconnect** (or **No WiFi configured** when nothing is saved
   yet)
2. **WiFi Setup**
3. **Details**
4. **Sync Time**

## Setting up a network

Choose **WiFi Setup** to run the setup flow:

1. The badge scans for nearby networks and lists them, strongest signal first.
   Each entry shows the network name, a signal-strength indicator, and a padlock
   for secured networks. The scan times out after about 10 seconds.
2. Pick a network from the list, or choose **+ Add Manual** to type a network
   name (SSID) by hand.
   - When adding manually, you also pick the encryption type: **WPA2**,
     **WPA/WPA2**, **WPA3**, **WPA**, **Open**, or **WEP**.
3. For a secured network, enter the password (up to 64 characters). Open
   networks skip this step.
4. Choose the IP mode:
   - **DHCP (Auto)** — the network assigns an address automatically.
   - **Static IP** — you enter the IP address, gateway and netmask yourself
     (each must be a valid IPv4 address; the default netmask is
     `255.255.255.0`).
5. The badge saves the configuration and attempts to connect.

The saved network is remembered, so afterwards you can simply use **Connect**.

## Connecting and disconnecting

- **Connect** brings Wi-Fi up using the saved configuration. The connect attempt
  times out after 15 seconds by default.
- **Disconnect** turns Wi-Fi off.

The Wi-Fi intent (on or off) is remembered across restarts, and a Wi-Fi
indicator appears on the lock screen while connected.

## Checking the connection

**Details** shows the current status:

- **When connected:** status, network name (SSID), MAC address, IP address and
  signal strength in dBm.
- **When not connected but configured:** the saved network name and whether it
  uses DHCP or a static IP.
- **When nothing is configured:** a "No WiFi configured" message.

## Setting the time over the network (NTP)

**Sync Time** sets the badge clock from an internet time server (NTP).

- This step **requires Wi-Fi.** You need either an active connection or a saved
  network configuration. If the badge is not connected, it connects first.
- The badge queries `time.krim.dev` (a fully non-logging hardware NTP server),
  falling back to `pool.ntp.org` and then `time.google.com`, waiting up to about
  10 seconds.
- On success the badge confirms the time was synced and marks its clock as set;
  on failure it reports a sync timeout.

:::tip
You can also set the clock by hand with **Settings → Set Date** and
**Settings → Set Time**. See [Settings](/guide/settings/).
:::

## Why the clock matters

Time-based features depend on the badge having a correct clock. Keeping the time
accurate — by syncing over NTP or setting it manually — ensures these features
behave as expected.
