---
title: Power, battery & sleep
description: How the badge saves power — light sleep, deep sleep, wakeup, battery display and keeping it awake.
sidebar:
  order: 4
---

The badge has two sleep modes plus a normal active mode. Both sleep modes are
driven from the lock screen, so they only take effect once the badge is locked.

## Light sleep

When the lock screen has been idle for a while, the badge enters **light
sleep**. In light sleep the CPU is paused but the badge wakes again quickly.

- **Idle delay:** light sleep starts after **2 minutes** of inactivity on the
  lock screen.
- A small light-sleep indicator is shown on the lock screen while it is active.
- Light sleep is only entered from the lock screen; it never starts while you are
  inside a menu.

### What stops light sleep

The badge will not enter (or stay in) light sleep when:

- **USB is connected.** The idle timer is reset so the badge stays responsive.
- **Sleep is inhibited** (see [Keeping the badge awake](#keeping-the-badge-awake)
  below).

### Waking from light sleep

Light sleep can wake from two sources:

| Wakeup | What happens |
|--------|--------------|
| Key press | The light-sleep indicator is removed, the display refreshes, and the badge returns to the normal lock screen. |
| Timer | The badge briefly wakes to refresh the clock, then goes back to sleep. |

The timer wakeup interval is the **Sleep Interval** value in
[Settings](/guide/settings/). If USB is connected while the badge
is in light sleep, it leaves light sleep.

## Deep sleep

Deep sleep is a much deeper power-down. Waking from deep sleep restarts the
badge (it boots fresh).

- **Trigger:** on the lock screen, hold the <kbd>N</kbd> key for **5 seconds**.
- The badge shows a deep-sleep screen, turns the backlight off, waits about two
  seconds (so you can release the key), then powers down.
- **Wakeup:** deep sleep only wakes on a key press, and waking causes a reset
  (the badge boots up again rather than resuming).

The deep-sleep gesture only works on the lock screen.

## Ship mode

Ship mode is the deepest power-off: it disconnects the battery from the system
(the charger's BATFET opens), so the badge draws no battery current at all. It is
meant for shipping and long-term storage.

- **What happens:** the battery is electrically disconnected. With no USB
  attached the badge powers off completely; while USB is connected the badge
  keeps running from USB and the battery stays disconnected.
- **Wake / exit:** connect USB, or press the power-on button, to bring the
  badge back. Unlike deep sleep this is a full power-down, not a timed sleep.

There are three ways to enter ship mode:

| Way in | How |
|--------|-----|
| Flash button | Hold the **Flash button** (the same button used to enter flash mode) for **3 seconds** during normal operation. This is not the power-on button, which wakes the badge back up. |
| Expert menu | **Main menu → Tools → Expert → Shipping Mode**, then confirm the prompt. |
| Serial console | Send `SHIPMODE` after authenticating with `AUTH <pin>`. |

:::caution
Ship mode disconnects the battery. If no USB is attached the badge switches off
immediately, so only enter it when you are ready to put the badge away.
:::

## Battery and charging

The lock screen shows the current battery level and power status.

- **Battery level** is shown as a fill level from 0 to 100 %.
- The level is re-sampled at most **every 30 seconds**, so the reading stays
  steady rather than flickering with small voltage changes.
- If **no battery** is connected, the badge shows an empty battery outline with
  a diagonal strike-through.
- A **charging** indicator appears while the battery is charging.
- A **USB** indicator appears while USB is connected.

These indicators are refreshed each time the lock screen redraws.

## Keeping the badge awake

Some activities hold the badge awake so it does not drop into light sleep. While
at least one such "keep awake" reason is active, a caffeinated (coffee-cup) icon
is shown on the lock screen and light sleep is suppressed. Up to eight such
reasons can be active at once.

## Auto-lock

Separately from sleep, the badge locks itself after a period of inactivity while
you are inside the menus:

- After **5 minutes** without a key press, the badge returns to the lock screen.
- Auto-lock is held off while a plugin that requests to prevent sleep is in the
  foreground.

## At a glance

| Mode | Trigger | Wake |
|------|---------|------|
| Active | Normal use | — |
| Light sleep | Lock screen idle for 2 minutes | Any key (timer wakeups refresh the clock and return to sleep) |
| Deep sleep | Hold <kbd>N</kbd> for 5 seconds on the lock screen | Any key (badge resets) |
| Ship mode | Hold the Flash button 3 s, Expert menu, or `SHIPMODE` | Connect USB or press the power-on button |
