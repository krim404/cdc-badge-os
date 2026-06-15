---
title: Lock screen & PIN
description: Locking and unlocking the badge, changing the PIN, lockout behaviour and automatic locking.
---

The badge protects access with a numeric **PIN**. While locked it shows the
lock screen; unlocking requires the correct PIN. This page covers locking and
unlocking, changing the PIN, and the lockout and auto-lock behaviour.

## Locking and unlocking

**Unlock:** from the lock screen, press any key (except <kbd>3</kbd>, which
opens the lock-screen menu) to open PIN entry, type your PIN with
<kbd>0</kbd>–<kbd>9</kbd>, and press <kbd>Y</kbd>. A correct PIN opens the main
menu. The badge ships with a default PIN of `123456`.

**Lock:** press <kbd>N</kbd> repeatedly to back out of all menus to the lock
screen. The badge also locks itself automatically (see
[Automatic locking](#automatic-locking)).

## Changing the PIN

Open **Settings → Change PIN**. This runs a three-step wizard:

1. **Current PIN** — enter your existing PIN. It is verified before you can
   continue. The screen also shows the number of remaining retries.
2. **New PIN** — enter the new PIN.
3. **Confirm PIN** — re-enter the new PIN. It must match step 2.

In the wizard:

- <kbd>0</kbd>–<kbd>9</kbd> add a digit, <kbd>Y</kbd> confirms the step.
- <kbd>N</kbd> deletes the last digit. On an empty first step it cancels the
  whole wizard; on a later step with no digits entered it goes back one step.
- If the confirmation does not match, the wizard returns to the New PIN step.

## PIN length

The badge PIN must be between **4 and 8 digits** long. <kbd>Y</kbd> will not
accept a PIN shorter than the minimum, and entry stops at the maximum length.

## Wrong PIN and lockout

The badge allows **3 attempts**. Each wrong PIN clears the entry and decrements
the remaining retries (shown on screen). When the retries reach zero the badge
enters a **60-second** lockout: PIN entry is blocked and the screen shows a
countdown. After the lockout expires the retry counter is restored and you can
try again.

:::note
The retry counter is held in RAM and the lockout timer runs from boot, so a
power-cycle during the lockout does not permanently brick the badge PIN.
:::

## Automatic locking

After **5 minutes** of inactivity, the badge automatically returns to the lock
screen, requiring the PIN again on the next use. (A foreground plugin that
requests to keep the badge awake suppresses this auto-lock while it is active.)

For the instant rescue lock, see the anti-block chord under
[Keypad & input methods](/start/keypad-navigation/#anti-block-instant-lock):
holding <kbd>N</kbd> and <kbd>Y</kbd> together forces the badge straight back to
a clean locked state.

## Duress PIN

In addition to the normal PIN, the badge supports an optional **duress PIN**
that wipes the device when entered. It is set up separately and is disabled by
default. See [Duress PIN](/security/duress/) for details.
