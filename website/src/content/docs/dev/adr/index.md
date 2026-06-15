---
title: Architecture Decision Records
description: Index of the load-bearing architecture decisions for CDC Badge OS, recorded as ADRs.
sidebar:
  order: 99
---

Architecture Decision Records (ADRs) capture the load-bearing technical decisions of CDC Badge OS:
the context, the decision, its status, and its consequences. New ADRs use the MADR-style template
(`_template.md` in this folder).

## Catalog

| ADR | Title | Source |
|-----|-------|--------|
| ADR-0001 | PSRAM-first memory model; internal SRAM reserved | Constitution II |
| ADR-0002 | Module isolation & single-point registration | Constitution I |
| ADR-0003 | TROPIC01 slot allocation (`tropic_slot_map.h` authoritative) | slot map |
| ADR-0004 | Two-KDF PIN hashing is protocol-driven (CTAP2 vs OpenPGP S2K) | reverse-spec D2 |
| ADR-0005 | No-migration policy + build-profile-byte factory wipe | Constitution V |
| ADR-0006 | Plugin WAMR sandbox & capability model; AOT default-off | spec 010 |
| ADR-0007 | `host_api.h` canonical + SDK byte-mirror + versioning | spec 010 |
| ADR-0008 | Single BLE controller via `IBluetoothController` | Constitution I |
| ADR-0009 | E-paper refresh-mode discipline (PARTIAL_LIGHT never promoted) | spec 014 |
| ADR-0010 | CP437 display pipeline (no `gfx->print` for i18n text) | spec 015/016 |
| ADR-0011 | Attestation-signed PIN record in R-Memory slot 0 | spec 002 |
| ADR-0012 | Secure-serial gate + `DEBUG_MODE` build profiles (real defaults) | reverse-spec D1/D3 |

ADR files (`NNNN-title.md`) are added by the US2 tasks in
`specs/001-current-system-spec/tasks.md`. This index is updated as each ADR lands.
