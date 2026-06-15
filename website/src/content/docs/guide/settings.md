---
title: Settings
description: The badge Settings menu — brightness, language, timezone, sleep interval, badge text, date, time and PIN.
sidebar:
  order: 3
---

The Settings menu collects the badge's adjustable options. Open it from
**Main Menu → Settings**.

The menu always contains the same eight items, in this order:

1. Brightness
2. Language
3. Timezone
4. Sleep Interval
5. Badge Text
6. Set Date
7. Set Time
8. Change PIN

Each item is described below, including the type of control it opens and the
range of values you can pick.

## Brightness

Opens a slider that controls the display backlight.

- **Range:** 0 to 100, shown with a `%` unit.
- The change is previewed live as you move the slider, and is saved when you
  confirm.

| Key | Action |
|-----|--------|
| <kbd>4</kbd> | Decrease |
| <kbd>6</kbd> | Increase |
| <kbd>Y</kbd> | Save |
| <kbd>N</kbd> | Cancel |

## Language

Opens a list of the available interface languages.

- The list always starts with **English**, which is built into the firmware.
- Each additional language is read from a translation file (`lang_<code>.json`)
  packaged on the badge. Every such file appears as one entry, labelled with the
  language's own name (for example, the German file shows up as "Deutsch").
- If no translation files are present, only English is shown.

Selecting a language applies it immediately and remembers your choice across
restarts. Up to 16 languages can be listed.

## Timezone

Opens a slider that sets the UTC offset used for the clock.

- **Range:** -12 h to +14 h, shown with an `h` unit.
- Saving updates the lock-screen clock right away.

The slider uses the same keys as Brightness (<kbd>4</kbd>/<kbd>6</kbd> to
adjust, <kbd>Y</kbd> to save, <kbd>N</kbd> to cancel).

## Sleep Interval

Opens a slider that sets the light-sleep timer interval used on the lock screen.

- **Range:** 0 to 60, in minutes (`min`).
- A value of **0** is shown as **Never**.

:::note
This is the interval at which the badge briefly wakes during light sleep (for
example to refresh the clock), not the idle delay before light sleep starts.
See [Power, battery & sleep](/guide/power-sleep/) for how light
sleep and the idle delay work.
:::

## Badge Text

Starts a short wizard that edits the three text lines shown on the lock screen,
one after another:

1. **Name**
2. **Info**
3. **Info 2**

Each line is typed with the on-screen T9 keyboard and can be up to 64
characters. The values are stored on the badge and persist across restarts.

## Set Date

Opens a date editor with separate day, month and year fields. Confirming
applies the date to the badge's system clock.

| Key | Action |
|-----|--------|
| <kbd>0</kbd>–<kbd>9</kbd> | Enter digits (focus advances to the next field automatically) |
| <kbd>N</kbd> | Clear current field (or cancel if empty) |
| <kbd>Y</kbd> | Confirm |

## Set Time

Opens a time editor with separate hour and minute fields. Confirming applies the
time to the badge's system clock. The keys are the same as for Set Date.

:::tip
To set the clock automatically over the network instead of by hand, use
**Tools → WiFi → Sync Time**. See [Wi-Fi & time](/guide/wifi-time/).
:::

## Change PIN

Opens the PIN-change wizard.

- The badge PIN is between **4 and 8** digits.

For details on locking, unlocking and PIN behaviour, see
[Lock screen & PIN](/guide/lock-and-pin/).
