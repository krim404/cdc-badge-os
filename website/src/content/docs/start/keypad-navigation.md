---
title: Keypad & input methods
description: The 12-button keypad layout, navigation keys and every on-device input view (T9 text, slider, date, time, PIN).
sidebar:
  order: 4
---

The badge is driven entirely by a **12-button keypad**. There is no
touchscreen. This page lists the button layout, the conventions that apply
across every screen, and the specialized input views.

## Button layout

The keys are arranged in a phone-style 4×3 grid:

| | | |
|---|---|---|
| <kbd>1</kbd> | <kbd>2</kbd> | <kbd>3</kbd> |
| <kbd>4</kbd> | <kbd>5</kbd> | <kbd>6</kbd> |
| <kbd>7</kbd> | <kbd>8</kbd> | <kbd>9</kbd> |
| <kbd>N</kbd> | <kbd>0</kbd> | <kbd>Y</kbd> |

The two special keys sit in the bottom row:

- <kbd>Y</kbd> = **confirm / OK / save / select**
- <kbd>N</kbd> = **cancel / back / backspace**

## Navigation conventions

These meanings are shared across the standard list and menu screens:

| Key | Action |
|-----|--------|
| <kbd>2</kbd> | Move selection up |
| <kbd>8</kbd> | Move selection down |
| <kbd>Y</kbd> | Select / open the highlighted item |
| <kbd>N</kbd> | Go back (leave the current screen) |
| <kbd>3</kbd> | Open the context menu, where one is offered |

In lists, holding <kbd>2</kbd> jumps to the first item and holding <kbd>8</kbd>
jumps to the last item.

### Context menu

The context menu (opened with <kbd>3</kbd>) is a small popup. Move with
<kbd>2</kbd>/<kbd>8</kbd>, choose with <kbd>Y</kbd>, and dismiss with
<kbd>N</kbd>. It closes itself automatically after 60 seconds of inactivity.

## Anti-block instant lock

If a screen ever becomes stuck, **hold <kbd>N</kbd> and <kbd>Y</kbd> together**.
This reserved rescue chord forces the badge back to a clean locked state: it
unloads plugins, dismisses every open dialog and view, and returns to the lock
screen, all without a hardware reset.

## Input views

Several screens replace the standard navigation with a dedicated input method.

### T9 text entry

Text is entered phone-style using multi-tap on the number keys.

- <kbd>0</kbd>–<kbd>9</kbd> cycle through the characters assigned to each key.
  Press the same key repeatedly to step through its characters; pause, or press
  a different key, to commit the current character and start the next.
- A character is committed automatically after a **2-second** pause.
- Long-press <kbd>0</kbd>–<kbd>9</kbd> to insert that literal digit.
- <kbd>N</kbd> deletes the last character; long-press <kbd>N</kbd> cancels.
- <kbd>Y</kbd> confirms the text.

Key-to-character mapping (lowercase first, then uppercase, then the digit;
accented variants follow on some keys):

| Key | Characters |
|-----|------------|
| <kbd>0</kbd> | space, `0` |
| <kbd>1</kbd> | `.` `@` `?` `!` `,` and other punctuation, then `1` |
| <kbd>2</kbd> | a b c A B C 2 |
| <kbd>3</kbd> | d e f D E F 3 |
| <kbd>4</kbd> | g h i G H I 4 |
| <kbd>5</kbd> | j k l J K L 5 |
| <kbd>6</kbd> | m n o M N O 6 |
| <kbd>7</kbd> | p q r s P Q R S 7 |
| <kbd>8</kbd> | t u v T U V 8 |
| <kbd>9</kbd> | w x y z W X Y Z 9 |

### Slider

Used for numeric settings such as brightness.

- <kbd>4</kbd> decreases the value, <kbd>6</kbd> increases it.
- Holding <kbd>4</kbd> or <kbd>6</kbd> repeats the change automatically.
- <kbd>Y</kbd> saves, <kbd>N</kbd> cancels.

### Date input

Edits a date as three fields, **Day → Month → Year**.

- <kbd>0</kbd>–<kbd>9</kbd> type digits into the active field. After the second
  digit of day or month, focus advances to the next field automatically.
- <kbd>N</kbd> clears the active field, or cancels when it is already empty.
- <kbd>Y</kbd> confirms. Values are clamped to a valid range (year 2000–2099,
  month 1–12, day 1–31).

### Time input

Edits a time as two fields, **Hour → Minute**.

- <kbd>0</kbd>–<kbd>9</kbd> type digits; after the second hour digit, focus
  advances to the minute field.
- <kbd>N</kbd> clears the active field, or cancels when empty.
- <kbd>Y</kbd> confirms. Hours are limited to 0–23 and minutes to 0–59.

### Masked PIN entry

Used for unlocking and PIN verification. Entered digits appear as filled dots;
remaining positions are empty dots.

- <kbd>0</kbd>–<kbd>9</kbd> add a digit.
- <kbd>N</kbd> deletes the last digit, or cancels when none is entered.
- <kbd>Y</kbd> confirms and verifies the PIN.

See [Lock screen & PIN](/guide/lock-and-pin/) for PIN length limits and lockout
behaviour.
