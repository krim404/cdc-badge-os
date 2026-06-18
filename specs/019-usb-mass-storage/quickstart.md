# Quickstart / Validation: USB Mass Storage (vFAT file transfer)

End-to-end validation that the `mod_msc` feature meets the spec. Implementation details live
in `tasks.md`; this is a run/verify guide. See `contracts/` and `data-model.md` for the
behaviour being checked.

## Prerequisites

- CDC Badge v1.x on USB; dev PIN `0000`.
- Build env `cdc_badge_usb` (PlatformIO + ESP-IDF). Build: `~/.platformio/penv/bin/pio run`.
- A host computer with a normal file manager (Windows/macOS/Linux). No drivers.
- The `vfat` partition rename (R6) and the initial FAT image are flashed (users wipe storage
  on this data-breaking change — no migration).

## Build & host tests

```bash
~/.platformio/penv/bin/pio run                 # firmware builds clean
~/.platformio/penv/bin/pio test -e native      # host unit tests pass (block bounds + write gate)
```

Expected: both green. Host tests cover `read10`/`write10` LBA+offset bounds checking against
`wl_size()` and that the host-active gate refuses badge-side writes.

## Flash

Serial path (no BOOT-hold, auto-reboot): over the CDC port send `AUTH 0000` then `BOOTLOADER`,
flash with `pio run -t upload`, the chip auto-reboots into the new firmware as `BadgeV1`.

## Scenario 1 — Default off, no drive (P1 / US3, FR-002, SC-003)

1. Fresh badge, do **not** enable anything. Connect USB.
2. Host file manager: **no** removable drive appears (only the serial port enumerates).

✅ Pass: connecting a default badge exposes no storage drive.

## Scenario 2 — Enable, copy a file onto the badge (P1 / US1, FR-001/004/005/006, SC-001/002)

1. On the badge: Expert → Modules → `mod_msc` shows `[OFF]`. Toggle it → `[ON]`.
2. Within ~5 s the host shows a new removable drive with the existing user files
   (e.g. `demo.jpg`, `demo.md`); the `system` folder is hidden/read-only.
3. Copy an image or `.md` file onto the drive. Safely eject and unplug.
4. On the badge: main menu → **Files** lists the new file; open it → it renders.

✅ Pass: drive appears < 5 s; file round-trips and opens (identical to a serial upload).

## Scenario 3 — Copy a file off the badge (P2 / US2, SC-005)

1. With the drive mounted, copy a file from the badge to the host.
2. Compare to the badge copy (e.g. checksum via `VFAT GET` over serial).

✅ Pass: byte-identical, no truncation.

## Scenario 4 — Disable removes the drive; persistence (FR-008, SC-007)

1. With `mod_msc` `[ON]`, toggle it `[OFF]` → the host drive disappears within a few seconds.
2. Reboot the badge → `mod_msc` returns to the last chosen state (off after step 1; on if left on).

✅ Pass: drive disappears on disable; enable state survives reboot.

## Scenario 5 — Serial console coexists (FR-011, SC-006)

1. With the drive mounted, open the serial monitor (115200) and run `HELP`.
2. The console responds normally while the drive is mounted.

✅ Pass: CDC console usable concurrently with MSC.

## Scenario 6 — Concurrency / no corruption (FR-007, edge cases)

1. With a host mounted and writing, attempt a badge-side write (`VFAT PUT` or a plugin upload):
   it is refused/deferred (host-active gate).
2. Yank the cable mid-copy: reconnect/reboot → filesystem consistent, prior files intact;
   a partially copied file may be incomplete.
3. After a normal eject, the Files browser reflects the host's changes (remount-on-disconnect).

✅ Pass: no corruption; badge writes gated while host-active; Files view refreshes after eject.

## Scenario 7 — Endpoint budget honesty (R3)

1. Enable services that consume USB endpoints (e.g. CCID/GPG + USB keyboard) **and** `mod_msc`.
2. If the combination exceeds the endpoint budget, the last-enabled service shows `[FAIL]` in
   the Modules list (no crash, no silent drop).

✅ Pass: over-budget combination is rejected cleanly and surfaced.

## Done when

- Scenarios 1–7 pass on hardware.
- `pio run` and `pio test -e native` green.
- Website docs updated (USB services + storage tools pages).
