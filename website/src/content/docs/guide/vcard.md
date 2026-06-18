---
title: vCard exchange
description: Edit your own contact card and exchange vCards badge-to-badge over Bluetooth.
sidebar:
  order: 9
---

The badge can store your own contact card as a **vCard 4.0** and exchange cards
with another badge over Bluetooth. It can also show your card as a QR code from
the lock screen, and import/export cards in encrypted backups.

You reach the feature from **Tools -> vCards**.

## The vCards menu

**Tools -> vCards** has four items:

- **My vCard** — view your stored card as rendered fields (or a hint that none is set).
- **Edit my vCard** — open the field-by-field editor.
- **Send vCard** — send your card to a nearby badge.
- **Received vCards** — browse the cards other badges have sent you.

## Editing your own card

Select **Edit my vCard** to step through the editor, one field per screen. The
fields, in order, are:

1. First name
2. Last name
3. Display name
4. Organization
5. Position
6. Email
7. Phone (Mobile)
8. Phone (Home)
9. Phone (Work)
10. Website
11. Telegram
12. Signal
13. Matrix
14. Threema
15. Social Profile
16. Note

Leave any field blank to omit it. When you finish, the badge stores the card and
shows **Saved**. The whole card is limited to 768 bytes.

### Setting a card over the serial console

You can also paste a full card over the serial console instead of typing it
field by field:

- `VCARD GET` prints the stored own card, or an empty 4.0 template if none is
  set. `VCARD GET <id>` prints a received card by its id.
- `VCARD LIST` lists received cards as `<id>  <name>`.
- `VCARD SET` enters paste mode for the own card; `VCARD SET <id>` overwrites a
  received card. Paste a vCard 4.0 and end with a line containing `---` (or
  `ABORT` to cancel). The session times out after 30 seconds of inactivity.
- `VCARD DELETE` removes the stored own card; `VCARD DELETE <id>` removes a
  received card by its id.

## Showing your card as a QR code

The badge registers a lock-screen quick action labelled **My vCard**. Trigger it
from the lock screen to display your card as a QR code (titled with your display
name or first/last name, subtitled with organization, position or email). If no
card is set yet, it shows a brief error toast instead.

## Sending your card to another badge

**Send vCard** hands your card to the message-transfer framework, which owns the
peer picker, consent, encryption and progress UI:

1. The other badge must have its **beacon** on (see
   [Bluetooth](/guide/bluetooth/#the-beacon-badge-to-badge-transfer)).
2. On your badge: **Tools -> vCards -> Send vCard**.
3. The badge scans and shows a list of nearby badges. Pick the target.
4. The badges connect and confirm a six-digit **numeric-comparison** code
   (the same code shows on both badges). Each side confirms.
5. The card transfers; a progress bar shows the percentage. Press <kbd>N</kbd>
   to cancel a send in progress.

On success both badges show **Transfer complete**. If you have no card stored
yet, **Send vCard** shows a "No vCard set" hint instead of scanning.

## Receiving a card

When another badge offers you a card, your badge shows an **Incoming transfer**
prompt with the sender's name, what is being sent (**Contact (vCard)**) and the
size in bytes. Choose:

- <kbd>Y</kbd> to accept — the badges confirm the numeric-comparison code, the
  card transfers, and it is stored in your contacts.
- <kbd>N</kbd> to decline.

The badge will not show an incoming-transfer prompt while it is locked; offers
are declined automatically until you unlock.

Received cards are deduplicated by exact text, so accepting the same card twice
does not create a duplicate. Browse them under **Tools -> vCards -> Received
vCards**.

## Managing received cards

**Received vCards** lists every card other badges have sent you, sorted by name.
Select a contact to view its parsed fields (name, organization, position, phone
numbers, email, website, messaging handles, note).

Press <kbd>3</kbd> in the list for:

- **Add** — create a new contact field by field (same editor as your own card).
- **Edit** — edit the selected contact.
- **Forward** — send the selected contact to a nearby badge (same flow as
  **Send vCard**).
- **Delete** — remove the selected contact after a confirmation.

Press <kbd>3</kbd> while viewing a contact for:

- **Edit** — edit this contact.
- **Forward** — send this contact to a nearby badge.
- **Show QR** — display this contact as a QR code to scan with a phone.
- **Delete** — remove this contact after a confirmation.

Over the serial console, `VCARD LIST` prints the stored cards with their ids,
`VCARD GET <id>` prints one card's raw vCard text, and `VCARD DELETE <id>`
removes one.

## Backups

Your own card and all received cards are included in the badge's encrypted
backup export, and restored on import. See the
[backup](/guide/settings/) workflow for how exports and imports work.
