# T-HIL08 — On-device backup export/import round-trip; wrong passphrase rejected; SE keys excluded

**Status: non-blocking (hardware verification)**

**FRs covered**: FR-060, FR-061, FR-062, FR-063

Verifies the encrypted backup container on hardware: an export covering 2FA, password-vault,
vCard and OS/WiFi settings can be re-imported with the same passphrase and reports an accurate
per-section tally, a wrong passphrase (or altered file) is rejected, and no secure-element private
keys (FIDO2, GPG/SSH) ever appear in the backup. Maps to Success Criteria SC-007 and SC-008.

## Prerequisites

### Hardware
- One provisioned CDC Badge v1.0/v1.1 with a working TROPIC01.
- USB-C cable to the host.

### Host tools
- Serial terminal at 115200 baud (`/dev/cu.usbmodem*`).
- `tools/backup.py` (or `BACKUP EXPORT`/`BACKUP IMPORT` serial commands) and a way to pull the
  `backup.cdcbak` file off the plugins partition (`VFAT`/`PLUGIN` file commands or `tools`).
- `base64` + a hex/text viewer on the host to inspect the container framing.

### Build profile
- Any beta build with `mod_2fa`, `mod_password`, `mod_vcard` enabled (default set).
- Badge PIN known (dev: `0000`) for the serial `AUTH` gate if the secure-serial gate is active.

## Procedure

1. **Seed restorable data**: create recognisable records: ≥ 1 TOTP account (note its account
   name), ≥ 1 password entry (note its title), ≥ 1 vCard (note its exact text), and set a
   distinctive OS setting (e.g. badge text line, timezone). Also create ≥ 1 FIDO2 credential and
   ≥ 1 GPG key set so the SE-exclusion check has SE material present.
2. **Export (FR-060/FR-061)**: run a backup export with a non-empty passphrase, e.g.
   `BACKUP EXPORT correctpass` (or `tools/backup.py`). Confirm `backup.cdcbak` is written to the
   plugins partition.
3. **Inspect container framing (FR-061)**: pull `backup.cdcbak` to the host, base64-decode it, and
   inspect the binary layout. Confirm it begins with magic `CDCBAK`, followed by version,
   kdf_iters, a 16-byte salt, a nonce, ciphertext and a tag (AES-256-GCM, PBKDF2-HMAC-SHA256,
   200,000 iters, header as AAD).
4. **SE-key exclusion (FR-062 / SC-008)**: decrypt the container with the correct passphrase
   (`tools/backup.py` or import in a controlled view) and inspect the plaintext payload. Confirm
   it contains the 2FA/password/vCard/settings sections and contains **no** secure-element private
   keys (no FIDO2 private keys, no GPG/SSH private keys). Grep the decrypted plaintext for any key
   material; it must be absent.
5. **Wrong passphrase rejected (FR-063 / SC-007)**: run `BACKUP IMPORT wrongpass`. Confirm the
   import fails with a clear error and changes nothing on-device.
6. **Altered file rejected (FR-063 / SC-007)**: flip one byte in the ciphertext (or tamper the
   tag) of a copy, write it back, and import with the correct passphrase. Confirm the GCM tag
   check fails and the import is refused.
7. **Modify then restore (round-trip, FR-063 / SC-007)**: change one restorable record on the
   badge (e.g. rename the TOTP account, edit the password title). Then `BACKUP IMPORT correctpass`
   with the original good container. Confirm:
   - TOTP is upserted by account name, password by title, vCard by exact text.
   - A per-section tally (imported / failed / skipped) is reported and is accurate against what
     was in the backup.
   - The restored records match the originals from step 1.
8. **Newer host-API level refused (FR-063)** (optional): if a container with a host-API level
   newer than the running firmware is available, attempt import and confirm it is refused.

## Pass criteria

- Step 2: `backup.cdcbak` is written with a non-empty passphrase.
- Step 3: container framing matches `CDCBAK ‖ version ‖ kdf_iters ‖ salt(16) ‖ nonce ‖ ciphertext
  ‖ tag`, with PBKDF2 iters = 200,000 (FR-061).
- Step 4: the decrypted payload contains 2FA/password/vCard/settings and **no** secure-element
  private keys (FIDO2, GPG/SSH) — 0 SE keys present (SC-008 / FR-062).
- Step 5: a wrong passphrase is rejected 100% of the time with a clear error; nothing changes
  on-device (SC-007).
- Step 6: an altered/tampered container fails the AES-256-GCM tag check and is refused (SC-007).
- Step 7: import upserts by identity (TOTP by name, password by title, vCard by exact text),
  reports an accurate imported/failed/skipped tally per section, and restores the original
  records (FR-063 / SC-007).
- Step 8 (if exercised): a backup with a newer recorded host-API level is refused.

## Notes

- **Flash conservation**: requires **no firmware flashing** — all steps are serial commands / file
  transfers against the currently flashed image. If a reflash is unavoidable, use the serial
  `AUTH <pin>` then `BOOTLOADER` path.
- **Not destructive to SE keys**: import is best-effort upsert and does not erase the secure
  element. However, step 7 overwrites the modified TOTP/password records back to their originals —
  that is intentional; do not run step 7 against records you want to keep modified.
- **Export auth (open item B5)**: whether export requires an unlocked device in addition to the
  passphrase is undocumented; while running, note whether the badge had to be unlocked to export
  and record it against B5.
- Keep the decrypted plaintext from step 4 off any shared location and delete it after the check —
  it contains password-vault entries.
