"""On-device test harness for CDC Badge OS.

Drives the finished, shipped firmware on a real badge over the USB-CDC serial
console. The release under test is flashed exactly once; every test reaches its
state at runtime (serial commands, power-cycle, buttons) and never flashes a
test-only or alternate-profile image.

Tests are split into two catalogs:
  - catalog_auto: fully automatic, serial-only, unattended, self-cleaning.
  - catalog_semi: semi-automatic, run last, prompting the operator for button
    presses, host-protocol actions (CTAP2/CCID), a second badge, or a look at
    the display.

Both catalogs are optional: this harness is a developer/QA tool, not a CI gate.
"""
