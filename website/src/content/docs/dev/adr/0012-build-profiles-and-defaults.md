---
title: ADR-0012 — Build profiles and real feature-flag defaults
description: Secure-serial gate and DEBUG_MODE defaults, with the SC-013 release gate.
sidebar:
  order: 12
---

**Status**: accepted
**Source**: reverse-spec Discrepancy D1/D3; NFR-008; SC-013; `components/cdc_core/include/cdc_core/feature_flags.h`

## Context

`feature_flags.h` defines the security-relevant build flags. Documentation previously described
some of these defaults incorrectly (D1: serial AUTH gate; D3: what `DEBUG_MODE` disables). The
on-device behaviour is the ground truth. SC-013 defines what makes a build a release vs. a beta.

## Decision

The real compile-time defaults are recorded as follows, and documentation must match them:

- `FEATURE_SECURE_SERIAL` defaults **0** (off). It is 1 only when Kconfig `CONFIG_SECURE_SERIAL`
  is set. The serial AUTH gate is therefore off by default.
- `DEBUG_MODE` defaults **1** (on). It disables development lockouts and enables verbose logging;
  it does NOT bypass the self-recovering badge-PIN lockout (that recovery is identical in debug
  and release builds, per FR-003).
- `FEATURE_PLUGIN_AOT` defaults **0** (interpreted WASM only; AOT native code off — see ADR-0006).
- `FEATURE_NVS_EDIT` defaults **0** (destructive NVS-editor actions off).

Release gate (SC-013): a **release** is a build with firmware version ≥ 1.0 and MUST have
`DEBUG_MODE=0`, `FEATURE_SECURE_SERIAL=1`, `FEATURE_PLUGIN_AOT=0`, and `FEATURE_NVS_EDIT=0`.
Everything before 1.0 is **beta** and is NOT bound by this gate (`DEBUG_MODE` may remain on
during beta).

## Consequences

- Enables: a measurable release gate distinct from the permissive beta defaults; the
  `BUILD_PROFILE_BYTE` factory wipe (ADR-0005) fires whenever `DEBUG_MODE` or
  `FEATURE_SECURE_SERIAL` changes between flashes.
- Must hold: the documentation states these real defaults; release builds (≥ 1.0) are rejected
  by the gate if any flag deviates; beta builds are exempt.
- Cost: the default beta profile ships with verbose logging and an open serial console, so beta
  devices are not hardened until the release flags are set.
