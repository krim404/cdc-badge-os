---
title: ADR-0009 — E-paper refresh-mode discipline
description: PARTIAL_LIGHT is never promoted to a full refresh.
sidebar:
  order: 9
---

**Status**: accepted
**Source**: spec 014 (display); NFR-003; FR-094; `components/cdc_hal/include/cdc_hal/IDisplay.h` (`RefreshMode`)

## Context

The 2.9" monochrome e-paper updates only on content change. `RefreshMode` has three values:
`FULL` (slow, clears ghosting), `PARTIAL` (fast, may ghost; periodically promoted to FULL to
clear ghosting), and `PARTIAL_LIGHT` (partial refresh that is never promoted to FULL, for tiny
low-churn updates such as the lock-screen clock). An async flush must carry the full
`RefreshMode`; collapsing `PARTIAL_LIGHT` into `PARTIAL` would reintroduce periodic full
refreshes on the lock-screen clock.

## Decision

The display pipeline preserves refresh-mode discipline. The lock-screen clock uses
`PARTIAL_LIGHT`, and `PARTIAL_LIGHT` is never promoted to a full refresh.

- An async flush carries the full `RefreshMode` end-to-end; it is not downgraded or collapsed.
- `PARTIAL` may be periodically promoted to `FULL` to clear ghosting; `PARTIAL_LIGHT` may not.
- The busy-wait must outlast the full-refresh waveform so back-to-back full refreshes do not
  abort each other.

## Consequences

- Enables: a low-churn lock-screen clock that updates without the visual disruption and wear of
  repeated full refreshes.
- Must hold: any flush path that forwards a refresh mode must forward `PARTIAL_LIGHT` unchanged;
  the per-second clock tick stays `PARTIAL_LIGHT`.
- Cost: `PARTIAL_LIGHT` regions accumulate ghosting over time because they are never cleared by
  a full refresh; only content that tolerates this should use it.
