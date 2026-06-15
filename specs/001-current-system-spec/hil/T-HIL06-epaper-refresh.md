# T-HIL06 — E-paper refresh: lock-screen clock uses PARTIAL_LIGHT, never promoted to FULL

**Status: non-blocking (hardware verification)**

**FRs covered**: FR-094

Verifies the e-paper refresh-mode discipline: the display renders CP437 text and the lock-screen
clock updates via a PARTIAL_LIGHT refresh that is **never** promoted to a FULL refresh. A FULL
refresh is the disruptive black/white flash of the whole panel; PARTIAL_LIGHT is the subtle,
flash-free local update. Maps to NFR-003.

## Prerequisites

### Hardware
- One provisioned CDC Badge v1.0/v1.1 with the GDEY029T94 e-paper panel.
- USB-C cable to the host (for serial observation).

### Host tools
- Serial terminal at 115200 baud (`/dev/cu.usbmodem*`).
- Optional: a phone/camera to record the panel at the minute boundary for frame-by-frame review
  (the FULL-refresh flash is brief).

### Build profile
- Any beta build. Display refresh logging is helpful — if the flashed image logs refresh modes
  (FULL / PARTIAL / PARTIAL_LIGHT) on serial, this plan can be verified by log alone; otherwise
  rely on visual observation of the panel.

## Procedure

1. **Reach the lock screen**: unlock and re-lock (or wait for auto-lock) so the badge shows the
   lock screen with the live clock.
2. **CP437 rendering (FR-094)**: confirm the lock-screen text (badge text lines, status icons,
   any umlauts if the language is German) renders correctly — i.e. CP437 glyphs are drawn, with
   no box/line-glyph corruption (a corruption symptom would be `ae`/`oe` rendering as box/line
   glyphs).
3. **Observe clock minute tick (FR-094)**: watch the clock across at least **three** minute
   boundaries (≥ 3 minutes total).
   - Visually: each minute update must be a **subtle local refresh of the time digits only**,
     with **no whole-panel black/white flash**.
   - On serial (if logged): each clock update is logged as `PARTIAL_LIGHT`, never `FULL`.
4. **Span a 10-minute / hour boundary** (optional but recommended): observe the clock crossing
   `:00` and an hour rollover to confirm even larger digit changes stay PARTIAL_LIGHT and are not
   promoted to FULL.
5. **Contrast with a legitimate FULL**: trigger an action that legitimately warrants a FULL
   refresh (e.g. navigate into a menu and back to a fully different screen). Confirm a FULL
   refresh (whole-panel flash) does occur there — establishing that the device *can* do FULL and
   that the clock's avoidance of FULL is deliberate, not a broken FULL path.
6. **Sustained idle on lock screen**: leave the badge on the lock screen for ~10 minutes
   (staying above the light-sleep idle if needed, or noting wake-driven redraws). Confirm no
   periodic FULL refresh of the clock occurs over the interval.

## Pass criteria

- Step 2: lock-screen text renders as correct CP437 glyphs (no box/line corruption of umlauts).
- Step 3: across ≥ 3 minute ticks the clock updates with a flash-free PARTIAL_LIGHT refresh; if
  refresh modes are logged, every clock update is `PARTIAL_LIGHT` and **none** is `FULL`.
- Step 4 (if exercised): minute/hour rollovers stay PARTIAL_LIGHT (not promoted to FULL).
- Step 5: a deliberate full-screen change *does* produce a FULL refresh — confirming the FULL
  path works and the clock's PARTIAL_LIGHT is an intentional discipline, not a defect.
- Step 6: over a ~10-minute lock-screen idle the clock never undergoes a FULL refresh.

## Notes

- **Flash conservation**: this plan requires **no flashing** — it is pure observation against the
  currently flashed image. If a build with refresh-mode logging is desired and not present, defer
  to visual observation rather than reflashing.
- **Non-destructive**: no secrets touched.
- **Observation aid**: the FULL-refresh flash is brief; if visual judgement is ambiguous, record a
  slow-motion video of the minute boundary and review frame-by-frame, and/or rely on the serial
  refresh-mode log if available.
- A momentary "stale" look between refreshes is expected and not a failure (spec Edge Cases).
