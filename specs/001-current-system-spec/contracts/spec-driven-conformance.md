# Contract: Spec-Driven Repository Conformance

This transition has no external API. Its "contracts" are the **conformance gates** the repository
must satisfy to be considered spec-driven. Each gate is observable and check-able (mostly in CI).
These define "done" for the transition and are the acceptance criteria the per-capability specs and
tasks are measured against.

## C1 — Specification coverage
- **Contract**: every active module/capability in the Spec Catalog has a feature spec under
  `specs/`, and every baseline FR is owned by exactly one spec.
- **Check**: a coverage script maps `specs/*/spec.md` ↔ Spec Catalog ↔ baseline FR refs; fails on
  any uncovered capability or orphaned FR.

## C2 — FR ⇄ code traceability
- **Contract**: each FR references a real code anchor (file/symbol) or is explicitly marked
  `[NEEDS CLARIFICATION]`; no FR invents behaviour absent from code.
- **Check**: spec review; anchors resolve in the tree. Code is the source of truth for *current*
  behaviour (docs lead long-term, once reconciled).

## C3 — Test contract
- **Contract**: every Tier-1 host Test Item builds and passes under `[env:native]` in CI without
  flashing; every Tier-2 HIL Test Item has a written procedure with explicit pass criteria.
- **Check**: CI runs the native test stage green; HIL plans exist for all FRs not coverable on host.
- **Non-blocking rule**: failing/absent HIL coverage does not block builds (constitution keeps
  firmware hardware-verified), but uncovered FRs must be listed, never silently dropped.

## C4 — Documentation reconciliation
- **Contract**: no unresolved Category-A doc-vs-code defect (D1–D4) remains once the transition
  completes; the `website/` docs match code and are restored as the long-term source of truth.
- **Check**: each of D1–D4 is either fixed in docs or captured by an accepted ADR (e.g. ADR-0004
  for D2, ADR-0012 for D1/D3).

## C5 — Architecture decisions recorded
- **Contract**: every load-bearing decision in the ADR Catalog exists as an accepted ADR under
  `website/src/content/docs/dev/adr/` and is referenced by the relevant spec/FR.
- **Check**: ADR files present and indexed; each ADR cites its source.

## C6 — No behaviour / no version drift
- **Contract**: the transition changes no firmware behaviour, bumps no version, adds no migration
  code; the `cdc_badge_usb` build output is unchanged by the doc/test scaffolding.
- **Check**: `pio run` (`cdc_badge_usb`) artifact unaffected by `test/host/**` and ADR/spec files;
  diff review confirms no version constant or on-device format touched.

## C7 — Risk visibility
- **Contract**: every open risk (Risk Register) has a severity, a mitigation, and an owning spec;
  WIP/hardware-unverified features are flagged as such in their specs.
- **Check**: Risk Register entries cross-reference a spec; WIP markers present.

## C8 — Refactor discipline
- **Contract**: every Refactoring Register entry carries `execute: NO` for this plan and a rationale;
  no refactor is implemented as part of the transition.
- **Check**: diff review shows no refactor commits; register is complete with rationales.

---

**Acceptance**: the transition is complete when C1–C8 hold. `/speckit-tasks` will turn these gates
plus the registries in `data-model.md` into an ordered, dependency-aware task list.
