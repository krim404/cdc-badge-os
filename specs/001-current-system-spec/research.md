# Phase 0 Research: Spec-Driven Repository Transition

Method decisions for how to make the repo spec-driven, plus the current-state survey that grounds
the registries in [data-model.md](./data-model.md). No product-behaviour research — this is about
process, tooling and documentation.

## Current-state survey (verified facts)

- **Specs**: exactly one — the reverse-spec baseline `specs/001-current-system-spec/spec.md`. No
  per-capability specs exist.
- **Tests owned by the project**: **none**. `test/` directories exist only under `third_party/`
  and `managed_components/` (libtropic, tinyusb, qrcode). Unity appears only inside `.pio/build/`
  (pulled by ESP-IDF), not as a project test target. `platformio.ini` has a single
  `[env:cdc_badge_usb]` and no `test_*` config. No `tools/test_*.py`.
- **CI**: `.github/workflows/build.yml` (+ GitLab/Forgejo mirrors) runs `pio run` (build only) and
  packages artifacts. **No test step.**
- **Architecture records**: none as ADRs. Decisions are implicit in code + captured prose in
  `CLAUDE.md`, the constitution (v1.0.1), the `website/` docs, and `MEMORY.md`.
- **Docs**: `website/src/content/docs/` (Astro Starlight), newly generated, intended long-term
  source of truth, not yet reconciled with code (see baseline Discrepancies D1–D4).
- **CLAUDE.md** has `<!-- SPECKIT START/END -->` markers (lines 430–433) for the plan pointer.

## Decision: spec granularity

- **Decision**: keep the baseline as the system-level reference; add **one feature spec per
  capability/module** under `specs/NNN-<capability>`.
- **Rationale**: the baseline is too coarse to drive `/speckit-tasks` per area; per-capability specs
  map 1:1 to modules (matches Principle I isolation) and let each be clarified/planned independently.
- **Alternatives considered**: (a) one giant spec — rejected (untestable granularity, merge
  conflicts); (b) spec-per-FR — rejected (too fine, high overhead).

## Decision: test strategy on embedded firmware

- **Decision**: two tiers. Tier 1 = **host `native` unit tests** (PlatformIO `[env:native]` + Unity)
  for pure logic with no ESP-IDF/hardware dependency. Tier 2 = **documented HIL test plans**
  (manual/semi-automated over USB CDC/HID/CCID + BLE) for hardware/protocol behaviour.
- **Rationale**: most security-critical *logic* (KDFs, CRCs, base64, CBOR encoders, framing, bounds,
  capability/GPIO policy) is portable and testable on host in CI with zero flash cost; behaviour that
  needs the secure element, USB stack, or radio cannot run in CI and is covered by repeatable HIL
  procedures instead. Honors the flash-conservation rule and the constitution's "tests OPTIONAL,
  hardware-verified" stance (tests are additive, non-blocking).
- **Alternatives considered**: (a) on-target Unity via `pio test` — rejected as the default because
  it requires a flash per run and a connected badge; kept as an option for SE-dependent units later.
  (b) QEMU ESP32-S3 — rejected for now (no TROPIC01/peripheral models; high setup cost).
- **CI-scope caveat (logged as a risk)**: the TROPIC01 secure element, USB and BLE stacks are **not**
  available on host, so attestation, key generation, CCID, and radio paths stay HIL-only.

## Decision: ADR format and location

- **Decision**: lightweight MADR-style ADRs (`Context / Decision / Status / Consequences`) under
  `website/src/content/docs/dev/adr/NNNN-title.md`, indexed from the dev docs.
- **Rationale**: keeps architecture records inside the single documentation system that the
  constitution names as runtime guidance, so they ship with the docs site and stay discoverable.
- **Alternatives considered**: top-level `docs/adr/` — rejected (the old `docs/` tree was deleted and
  replaced by `website/`); `.specify/memory/` — rejected (that is Spec Kit governance, not dev docs).

## Decision: handling doc-vs-code discrepancies

- **Decision**: fold the baseline's Category-A discrepancies (D1–D4) into this plan as **doc-fix
  tasks** and Category-B items as **clarification tasks** (resolve by reading code / HIL, then
  document). D2 is already resolved as by-design (two-KDF, protocol-driven) and becomes an ADR.
- **Rationale**: the user's stated end state is docs-lead; reconciling these is the precondition.
- **Alternatives considered**: change code to match docs — rejected (code is leading; docs are the
  defect).

## Decision: refactorings are documentation-only

- **Decision**: capture desirable refactors in a **Refactoring Register** (data-model.md) with an
  explicit `execute: NO` flag and rationale; do not modify code under this plan.
- **Rationale**: the user explicitly scoped refactors to "documented, not implemented"; matches
  Principle IV (surgical change) and avoids behaviour risk on a security device.
- **Alternatives considered**: execute low-risk refactors now — rejected (out of scope by request).

## Decision: what counts as "non-behavioural code change"

- **Decision**: the only code/config edits permitted are: adding `test/host/**`, adding
  `[env:native]` to `platformio.ini`, adding a CI test stage, and adding ADR/spec markdown. None
  touch the `cdc_badge_usb` firmware build output or any runtime path.
- **Rationale**: keeps the firmware bit-for-bit unchanged while enabling spec-driven verification.

## Open items intentionally NOT resolved here

The baseline's Category-B clarifications (B2–B15) are research *inputs* for the per-capability specs,
not resolved in this transition plan; each is assigned to its owning spec in data-model.md.
