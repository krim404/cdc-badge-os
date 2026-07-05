---
title: ADR-0014 — E-paper refresh escalation chain (PARTIAL → FAST → FULL)
description: Screen transitions render as partials; ghosting is bounded by a two-stage escalation to FAST and FULL refreshes.
sidebar:
  order: 14
---

**Status**: accepted
**Source**: user reports (full-refresh flashing, panel wear); Good Display GDEY029T94 datasheet guidance; `components/cdc_hal/src/EpaperDisplay.cpp` (`resolveRefresh`)

## Context

The GDEY029T94 panel is rated for 1,000,000 refreshes or 5 years. Its FULL refresh runs the
multi-flash OTP waveform (~2 s, visible black/white inversion cycles); a PARTIAL refresh is
flash-free but accumulates ghosting. Previously every view-stack transition (push/pop/replace,
modal dismiss) forced a FULL refresh, so normal menu navigation flashed constantly even though
the panel demonstrably tolerates dozens of consecutive partials without visible ghosting.

The SSD1680 also supports a FAST waveform (single flash, ~1 s, loaded via the temperature-register
override) that clears most ghosting but does not fully DC-balance the film.

## Decision

All ordinary renders — in-view updates and screen transitions alike — request PARTIAL. The HAL
escalates centrally in `resolveRefresh()`:

- After `FEATURE_EPD_MAX_PARTIALS_BEFORE_FAST` (default 50) consecutive PARTIALs, the next
  refresh is promoted to FAST.
- After `FEATURE_EPD_MAX_FASTS_BEFORE_FULL` (default 10) FAST refreshes, the next one is promoted
  to FULL. FAST never resets the FULL counter (it does not fully DC-balance the film).
- FULL resets both counters; `PARTIAL_LIGHT` never counts (ADR-0009 discipline unchanged).
- `FEATURE_EPD_FAST_REFRESH=0` maps FAST to FULL (hardware fallback).

Escape hatches guarantee clean panels regardless of the chain: the boot first render, the
lock/deep-sleep path and the lockdown handler stay FULL; a long-press on key '5' forces a manual
FULL refresh (`FEATURE_EPD_LONGPRESS_FULL_REFRESH`); views may demand a stronger first paint via
`IView::preferredEnterRefresh()` (ImageView → FULL, CanvasView → FAST) or escalate any render via
`ViewStack::forceRefresh(mode)`; plugins pass `refresh_mode 2 = FAST` (or 0 = FULL) to
`host_display_flush`.

## Consequences

- Enables: flash-free menu navigation; a FULL refresh only about every 500 partials during
  continuous use, cutting visible flashing and waveform wear.
- Must hold: any flush path forwards the refresh mode unchanged; the ghost counters live behind
  the panel mutex; modal dismissal relies on the whole-screen partial (`updateWindow(0,0,H,W)`)
  for erasure correctness, not on a FULL refresh.
- Cost: ghosting can accumulate for up to one escalation window; the thresholds are build-time
  knobs (`feature_flags.h`), and the long-press-5 gesture is the manual cleanup.
