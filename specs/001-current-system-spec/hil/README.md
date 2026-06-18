# On-device test plans

These plans verify the **installed release as-is** on a real badge. The release is flashed once;
every state is reached at runtime (serial commands, power-cycle, factory-reset/duress, button input).
No test-only or alternate-profile firmware is flashed per test or per state.

On-device tests are split into two categories, both **optional and non-blocking**:

- **Automatic** — driven end-to-end over the USB-CDC serial console, unattended and self-cleaning.
  Defined and run by `tools/ondevice/` (catalog `A-*`). Run them with:

  ```bash
  ~/.platformio/penv/bin/python tools/ondevice/run.py --pin 0000            # automatic only
  ~/.platformio/penv/bin/python tools/ondevice/run.py --pin 0000 --mutating --slow
  ~/.platformio/penv/bin/python tools/ondevice/run.py --pin 0000 --semi     # append semi (last)
  ```

- **Semi-automatic** — need an operator: a button press / user-presence touch, a host CTAP2/CCID
  stack, a second badge, or a look at the display. These run last. The `T-HIL01..08` files here are
  the written procedures with prerequisites, steps and explicit pass criteria; the `S-*` entries in
  `tools/ondevice/catalog_semi.py` drive their serial-checkable parts and prompt the operator for the
  rest.

The full catalog (`A-*` automatic, `S-*` semi-automatic, mapped to these plans) is in
[../data-model.md](../data-model.md) section 2.

| Plan | Capability | Category |
|------|-----------|----------|
| T-HIL01 | FIDO2 register + assert over USB | semi (S-FIDO2) |
| T-HIL02 | OpenPGP CCID sign/decrypt/SSH | semi (S-OPENPGP) |
| T-HIL03 | BLE numeric-comparison vCard transfer | semi (S-BLE-XFER) |
| T-HIL04 | Duress wipe | semi (S-DURESS) |
| T-HIL05 | PIN lockout + recovery | semi (S-PINUI); serial half automatic (A-LOCK) |
| T-HIL06 | E-paper PARTIAL_LIGHT clock | semi (S-EPAPER) |
| T-HIL07 | Plugin sandbox rejection | semi (S-PLUGINUI) |
| T-HIL08 | Backup export/import round-trip | automatic (A-BACKUP) |
