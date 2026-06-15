---
title: Password vault
description: Store, browse, edit and auto-type login credentials on the badge.
sidebar:
  order: 5
---

The password vault stores login credentials directly on the badge's secure
element. Each entry holds a title, username, password, URL, optional notes, and
an optional reference to a 2FA slot. Entries can be typed into a computer or
phone over a HID keyboard connection (see [Auto-type](/guide/auto-type/)).

## Where entries are stored

Entries live in the TROPIC01 secure element's R-memory, one entry per slot. The
vault is assigned R-memory slots 132 through 500, so it can hold up to **369
entries**.

Each write goes through the secure element's headered R-memory API: a metadata
header (with an integrity checksum) plus the entry payload. Writes are rejected
while the badge is locked or in alarm mode.

:::caution[Storage at rest]
The firmware writes the entry payload (including the password) into R-memory
together with a header and a header checksum. The visible source does not
describe an application-level encryption of the stored fields, so this guide
does not claim one. Treat physical access to an unlocked badge as access to the
stored credentials.
:::

## Entry fields and limits

Each entry has the following fields. The limits are the maximum number of
characters each field can hold.

| Field | Max length | Notes |
|-------|-----------:|-------|
| Title | 24 | Used as the entry's name and list label. |
| Username | 64 | Optional. |
| Password | 64 | Optional; can be auto-generated (see below). |
| URL | 96 | Optional. |
| TOTP slot | 0-254 | Optional reference to a 2FA slot. |
| Notes | 173 | Optional free text. |

The notes limit (173 characters) is what remains of a R-memory slot after the
header and the fixed fields, so it depends on the other fixed-size fields, not
on how much you typed elsewhere.

## Opening the vault

From the main menu, open **Passwords**.

The list shows **New Entry** first, then every stored entry sorted
alphabetically by title (case-insensitive).

Footer hint: <kbd>Y</kbd> View &nbsp; <kbd>3</kbd> Menu &nbsp; <kbd>N</kbd> Back

## Adding an entry

1. Open **Passwords** &rarr; select **New Entry**.
2. Step through the wizard, one field per screen, in this order:
   **Title** &rarr; **Username** &rarr; **Password** &rarr; **URL** &rarr;
   **TOTP slot** &rarr; **Notes**.
3. Confirm the last step to save. A "Saved" toast appears and you return to the
   list.

### Generating a random password

On the **Password** step, the hint reads `x=Random Y=OK N=Back`. Entering a
single `x` generates a random 16-character password from the set
`a-z A-Z 0-9 $ ! % =`. The random bytes come from the secure element's hardware
RNG, falling back to the ESP32 RNG if the secure element is unavailable.

## Viewing an entry

Select an entry (or press <kbd>Y</kbd>) to open its detail view, which shows the
title, username, password, URL, the linked TOTP slot number (or empty), and
notes.

If a HID keyboard is connected, the detail view shows a
<kbd>Y</kbd> Type hint and the footer
<kbd>Y</kbd> Type &nbsp; <kbd>2</kbd>/<kbd>8</kbd> Scroll &nbsp; <kbd>N</kbd> Back.
See [Auto-type](/guide/auto-type/).

## Editing and deleting

On the list, highlight an entry and press <kbd>3</kbd> to open its action menu:

- **View** - open the detail view.
- **Edit** - re-run the wizard pre-filled with the existing values.
- **Delete** - asks for confirmation, then removes the entry.

The action menu on **New Entry** offers only **New Entry**.

## Linking a 2FA entry

The **TOTP slot** field stores the slot number of a
[2FA](/guide/two-factor/) entry as a reference (0-254, or none). It is a
plain numeric reference: the detail view shows the linked slot number, but the
password vault does not itself generate or type the 2FA code. Generate and type
2FA codes from the 2FA menu.

## Serial commands

The vault is also scriptable over the serial console:

| Command | Purpose |
|---------|---------|
| `PASSWORD LIST` | List entries (sorted by title) with their slot numbers. |
| `PASSWORD GET <slot>` | Show one entry. |
| `PASSWORD ADD <slot\|x> <title> <user\|x> <pw\|x> <url\|x> <totp\|-> [notes]` | Add an entry; `x` for next free slot, `x` for password to generate one. |
| `PASSWORD EDIT <slot> <field> <value>` | Edit one field (`title`, `username`, `password`, `url`, `totp`, `notes`). |
| `PASSWORD DEL <slot>` | Delete an entry. |

:::note
Inside a serial field, use `\ ` (backslash space) to include spaces.
:::

## Backup

Vault entries are included in the badge's encrypted backup export and restore.
On import, an existing entry with the same title is overwritten; otherwise a new
slot is allocated.
