# Risk Register: Spec-Driven Repository Transition

This is the living risk register for the spec-driven repository transition. It tracks the risks
identified while decomposing the reverse-spec baseline (`001-current-system-spec`) into
per-capability specs, ADRs, and tests. It reproduces R-01..R-09 from
[data-model.md](./data-model.md) and cross-references the owning per-capability spec (002–016) or
ADR that carries each mitigation. Entries are reviewed as their owning spec/ADR/test items land;
all are currently `open`.

Severity uses low / med / high. Area uses security / process / CI / hardware.

| ID | Risk | Severity | Area | Mitigation | Owning spec/ADR | Status |
|----|------|----------|------|-----------|-----------------|--------|
| R-01 | Unverified on hardware: GPG cross-sign send path, BLE serial console (cdc_msg + BLE vCard + BLE HID verified 2026-06-18) | Med | hardware | T-HIL03 passed 2026-06-18; T-HIL07 pending for GPG cross-sign | spec 007 (T-HIL07) | open |
| R-02 | Doc-vs-code drift (D1–D4) until reconciled | Med | process | ADR-0004/0012 + doc-fix tasks; conformance gate C4 | ADR-0004, ADR-0012 | open |
| R-03 | `DEBUG_MODE` defaults ON → sensitive logging if shipped | High | security | ADR-0012; release checklist gate; document in security docs | ADR-0012 | open |
| R-04 | `credProtect` parsed/stored but not enforced at assertion (B2) | Med | security | spec 003 records as known gap; RF-04 design note | spec 003 (RF-04) | open |
| R-05 | Zero automated test coverage today | High | CI | Tier-1 host tests T-H01..09 in CI | spec 001 (T-H01..09) | open |
| R-06 | Secure element / USB / BLE absent in CI → crypto-on-SE & protocol paths uncoverable on host | Med | CI | explicit Tier-2 HIL scope; logged, not hidden | spec 001 (Tier-2 HIL) | open |
| R-07 | Flash wear during HIL verification | Low/Med | hardware | batch HIL runs; serial `BOOTLOADER` path; conserve flashes | spec 001 (Tier-2 HIL) | open |
| R-08 | OpenPGP PW3 terminal lockout → wipe-only recovery (user footgun) | Med | security | document prominently in spec 006 + security docs | spec 006 | open |
| R-09 | Spec/code divergence over time without traceability | Med | process | conformance gate C1 (every capability has a spec, FRs traceable) | spec 001 (gate C1) | open |
