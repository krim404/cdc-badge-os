# Quickstart: Validating the Spec-Driven State

How to verify the repository has reached the spec-driven state defined by this plan. This is a
validation/run guide, not implementation. It exercises the conformance gates in
[contracts/spec-driven-conformance.md](./contracts/spec-driven-conformance.md) against the
registries in [data-model.md](./data-model.md).

## Prerequisites

- PlatformIO available at `~/.platformio/penv/bin/pio`.
- Repo checked out; submodules initialised (`git submodule update --init --recursive`).
- No hardware required for Tier-1 / gate checks; a CDC Badge on USB only for the HIL section.

## 1. Specs exist and cover every capability (gate C1)

```bash
ls specs/                      # expect 001 (baseline) + per-capability 002..016
```
Expected: one spec directory per Spec-Catalog row; the baseline remains the system reference.
Validate that every baseline FR is owned by exactly one spec (no orphans, no gaps).

## 2. FR ⇄ code traceability (gate C2)

For a sampled spec, confirm each FR cites a resolvable code anchor or is marked
`[NEEDS CLARIFICATION]`. Spot-check anchors exist in the tree (e.g. `PinManager::computeBadgeHash`,
`ctap2.cpp` ClientPIN compare).

## 3. Host unit tests build and pass without flashing (gate C3, Tier 1)

```bash
~/.platformio/penv/bin/pio test -e native
```
Expected: the `native` environment compiles `test/host/**` and all Tier-1 items (T-H01..T-H09)
pass. No badge connected, no flash performed. Known-answer vectors (PIN KDFs, CRCs, base64,
backup container, CP437) match references.

## 4. Firmware build is unchanged by the scaffolding (gate C6)

```bash
~/.platformio/penv/bin/pio run -e cdc_badge_usb
```
Expected: firmware still builds; the `[env:native]` and `test/host/**` additions do not affect the
`cdc_badge_usb` output. No version constant changed; no on-device format touched.

## 5. ADRs recorded and discrepancies reconciled (gates C4, C5)

```bash
ls website/src/content/docs/dev/adr/      # expect ADR-0001..ADR-0012
```
Expected: each ADR-Catalog decision exists and is accepted; D1/D3 captured by ADR-0012, D2 by
ADR-0004; no unresolved Category-A discrepancy remains (D4 doc reference repointed or noted).

## 6. Risks and refactors are visible and disciplined (gates C7, C8)

- Risk Register: every entry has severity + mitigation + owning spec; WIP features flagged in specs.
- Refactoring Register: every entry is `execute: NO`; `git log`/diff shows no refactor was applied.

## 7. CI runs the test stage

Confirm the CI workflow(s) invoke `pio test -e native` (and doc/spec checks) in addition to
`pio run`, and that the stage is green.

## 8. HIL verification (optional, hardware required)

Run the Tier-2 HIL procedures (T-HIL01..T-HIL08) per their written steps with a badge on USB/BLE.
These are **non-blocking** for the build but must each have a documented procedure and pass
criteria; record outcomes. Conserve flashes (prefer the serial `BOOTLOADER` path).

---

**Done when**: steps 1–7 pass (gates C1–C8 hold). Step 8 is verification depth, scheduled
separately and not a build gate.
