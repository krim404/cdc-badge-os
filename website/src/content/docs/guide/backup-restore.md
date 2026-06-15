---
title: Encrypted backup & restore
description: Export an encrypted backup of your TOTP codes, passwords, vCards and OS settings, transfer it off the badge, and restore it - all protected by a passphrase you choose.
sidebar:
  order: 10
---

The badge can write a single, passphrase-encrypted backup file containing your
data (two-factor accounts, password vault, vCards) and your OS settings. The
file is encrypted on-device with a key derived from your passphrase, so it stays
protected even after you copy it off the badge.

:::caution[Your passphrase is the only key]
The backup is decryptable only with the passphrase you set at export time. There
is no recovery path: if you forget it, the backup is unrecoverable. Keep it
somewhere safe and separate from the backup file.
:::

## What is and is not backed up

| Backed up | Not backed up |
| --- | --- |
| Two-factor accounts (TOTP / HOTP / challenge-response), including the secret | FIDO2 / WebAuthn private keys |
| Password vault entries (title, username, password, URL, notes, linked TOTP) | GPG / SSH private keys |
| vCards (your own card and received cards) | Anything else held in the TROPIC01 secure element |
| OS settings: language, brightness, sleep interval, timezone, badge text, per-module enable state | |
| WiFi configuration, including the network password | |

The private keys that live in the secure element are **never** exported. By
design, only modules that opt in contribute a section, and the key-holding
modules (FIDO2, GPG) do not. The restore summary on the badge spells this out:
"No keys are backed up (FIDO2/GPG); only data."

:::note
WiFi and password-vault credentials *are* included. They are only ever present
inside the encrypted container, but treat the backup file with the same care as
the secrets it protects.
:::

## Where it lives in the menu

Open **Main menu → Tools → Expert**, then choose **Backup**. The submenu has
three actions:

| Action | What it does |
| --- | --- |
| **Export** | Collects a passphrase and writes an encrypted backup, overwriting any existing one. |
| **Import** | Restores from the on-device backup file. |
| **Delete** | Removes the on-device backup file (after a confirmation). |

The Expert menu shows a warning when you enter it; that is expected.

## Exporting a backup

1. Go to **Tools → Expert → Backup** and select **Export**.
2. Enter a passphrase (up to 64 characters). An empty passphrase is rejected.
3. Enter the same passphrase again to confirm. If the two do not match, the
   export is cancelled.
4. The badge encrypts everything and saves it. On success you see a
   "Backup saved" toast.

The backup is written to a file named `backup.cdcbak` on the badge's internal
`plugins` storage partition. Only one backup file exists at a time; exporting
again overwrites it. The file is stored as base64 text so it can be transferred
safely over the serial link.

The plaintext only ever exists briefly in RAM during export and is wiped
afterwards; only the encrypted form is written to storage.

## Restoring a backup

1. Make sure the backup file is present on the badge (export it first, or upload
   one with the tool described below).
2. Go to **Tools → Expert → Backup** and select **Import**.
3. Enter the passphrase you used at export time.
4. The badge decrypts the file and applies each section. A wrong passphrase (or
   a corrupted file) fails with "Wrong passphrase or corrupt file".

### Per-module result reporting

Restore is best-effort: each record is restored independently and a single
failure never aborts the whole operation. After a successful decrypt the badge
shows a summary:

| Field | Meaning |
| --- | --- |
| **Imported** | Records restored successfully across all sections. |
| **Failed** | Records skipped because of an error. |
| **Modules** | Number of module sections that were applied. |
| **Skipped** | Module sections in the file with no matching module on this badge. |
| **System Settings** | Whether the OS-settings section was present and applied. |

Importing **merges** into existing data rather than wiping first. An entry that
already exists is overwritten by the backup (matched by account name for
two-factor, by title for passwords, and by exact text for vCards).

Most settings apply immediately. A few - the badge display text and the WiFi
connect intent - take effect at the next boot.

:::note[Restoring onto a newer or older badge]
A backup records the firmware and host-API level it came from. Restore refuses a
backup that was made on a *newer* host-API level than the badge you are
restoring onto. Sections for modules that are not present on the target badge are
skipped and counted, not treated as errors.
:::

## Off-device transfer and crypto: `tools/backup.py`

The repository ships `tools/backup.py`, which drives the badge's AUTH-gated
`BACKUP` serial command, moves the file over the serial link, and can encrypt or
decrypt the container on your computer with just the passphrase. The script
reproduces the exact same container format as the firmware.

Most badge actions need the badge PIN for AUTH (`--pin`). Crypto-only modes work
entirely offline and need no badge.

```bash
# Trigger an export on the badge, then download the encrypted file
python tools/backup.py --export "correct horse" --pin 123456
python tools/backup.py --download backup.cdcbak --pin 123456

# Inspect the contents on your computer (decrypt to JSON)
python tools/backup.py --decrypt backup.cdcbak --out backup.json --pass "correct horse"

# Re-encrypt an edited JSON, upload it, and restore on the badge
python tools/backup.py --encrypt backup.json --out backup.cdcbak --pass "correct horse"
python tools/backup.py --upload backup.cdcbak --pin 123456
python tools/backup.py --import "correct horse" --pin 123456

# Remove the on-device backup
python tools/backup.py --delete --pin 123456
```

The serial port is auto-detected; pass `--port` to choose one explicitly. The
badge modes need `pyserial`; the `--decrypt` / `--encrypt` modes need the
`cryptography` package.

For the exact byte layout of the container, see the developer reference at
[/dev/proto/backup-format/](/dev/proto/backup-format/).
