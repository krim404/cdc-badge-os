---
title: First boot, PIN setup & unlocking
description: The lock screen, unlocking with the PIN, the status icons and the clock, date and battery display.
sidebar:
  order: 3
---

When the badge powers on it shows the **lock screen**. This is the home screen
of the device: it stays here until you unlock it, and you always return here
when you lock the badge or leave a menu.

## The default PIN

The badge does not run a first-time setup wizard. It boots straight to the lock
screen and is protected by a **default PIN of `123456`** out of the box. Unlock
with that PIN, then change it from **Settings → Change PIN** (see
[Lock screen & PIN](/guide/lock-and-pin/)).

:::caution
Change the default PIN before you store any keys or credentials on the badge.
The default is public knowledge.
:::

## Unlocking

From the lock screen, **press any key** to start unlocking. Any key except
<kbd>3</kbd> opens the PIN entry screen; <kbd>3</kbd> opens the lock-screen
menu instead (see below).

On the PIN entry screen:

- <kbd>0</kbd>–<kbd>9</kbd> enter PIN digits (shown as masked dots).
- <kbd>N</kbd> deletes the last digit, or cancels when no digit is entered.
- <kbd>Y</kbd> confirms and checks the PIN.

A correct PIN takes you to the main menu. The PIN must be at least the minimum
length before <kbd>Y</kbd> will accept it. Length limits, wrong-PIN behaviour
and lockout are described on the [Lock screen & PIN](/guide/lock-and-pin/) page.

## Reading the lock screen

The lock screen shows several elements:

- **Clock** (top left), in `HH:MM` format. It reads `--:--` until the clock is
  set, and updates once per minute.
- **Date** (below the clock), in `DD.MM.YYYY` format.
- **Battery indicator** (top right): a battery icon whose fill reflects the
  charge level. A small lightning bolt is drawn over it while charging, and a
  diagonal strike-through is shown when no battery is connected.
- **Status icons** (left of the battery): see the table below.
- **Name and info lines** (center): your configured badge name and two free
  text lines (editable via **Settings → Badge text**).
- **Footer hint** (bottom): a reminder that any key unlocks and <kbd>3</kbd>
  opens the menu.

## Lock-screen status icons

The icons drawn left of the battery, each verified against the rendering code:

| Icon | Meaning |
|------|---------|
| Padlock | Badge is locked |
| WiFi (stacked arcs) | WiFi is connected |
| Bluetooth rune | Bluetooth is enabled |
| USB trident | USB is connected |
| Sun | Backlight is on |
| `zzZ` | Deep sleep state |
| `z` | Light sleep state |
| Coffee cup | Sleep is inhibited (a plugin is keeping the badge awake) |
| Play triangle in a frame | A plugin is running in the background |

The charging bolt and the no-battery strike-through are drawn on the battery
icon itself rather than as separate status icons.

## The lock-screen menu

Pressing <kbd>3</kbd> on the lock screen opens a small **Actions** menu instead
of starting the unlock flow. It always offers a backlight toggle (**Light**)
and a WiFi on/off toggle, plus any extra entries contributed by installed
modules and plugins. Navigate it with <kbd>2</kbd>/<kbd>8</kbd>, choose with
<kbd>Y</kbd> and close with <kbd>N</kbd>.

## Sleeping the badge

Holding <kbd>N</kbd> on the lock screen for 5 seconds puts the badge into deep
sleep. The screen shows a short "Deep Sleep" message, then powers down. Release
the key during the message window; waking the badge restarts it and returns to
the lock screen.

## Next steps

- [Keypad & input methods](/start/keypad-navigation/) — the full button layout
  and every input view.
- [Lock screen & PIN](/guide/lock-and-pin/) — changing the PIN, lockout and
  auto-lock behaviour.
