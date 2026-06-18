# Implementation Plan: Image & Markdown Content Viewers

**Branch**: `017-image-markdown-viewers` | **Date**: 2026-06-18 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/017-image-markdown-viewers/spec.md`

## Summary

Add two reusable on-device content viewers and surface them in both the vFAT file explorer and the WASM plugin host API:

1. **Image viewer** — decode PNG/JPEG/GIF, dither to the 1-bit e-paper, fit-to-screen by default with an actual-size pan mode.
2. **Markdown renderer** — parse a Markdown source into a styled-line model and render it scrollably with larger bold fonts for headings (heading sizes assigned dynamically, smallest-first, by the count of distinct levels present).

Both are reached from `VfatExplorerView` (file selection) and from four new host calls (file-source via the `vfat`-gated fs family; in-memory buffer via the capability-free ui family). Plain-text viewing (`InfoView`, `host_fs_view`) is unchanged.

## Technical Context

**Language/Version**: C++17 (firmware); C99 third-party decoders.

**Primary Dependencies**: ESP-IDF 5.5.0, Adafruit-GFX, CalEPD (Gdey029T94 / SSD1680). New: `espressif/esp_jpeg` (JPEG, TJpgDec), `pngle` (PNG), `AnimatedGIF` (GIF first frame). See [research.md](./research.md).

**Storage**: FAT-FS `plugins` partition (existing); all decode/render buffers in PSRAM.

**Testing**: Host unit tests (`pio test -e native`) for the pure-logic parts — Markdown parser → styled-line model, Floyd-Steinberg dither, format magic-byte detection, heading-level→font mapping. Rendering and end-to-end flows verified on hardware (`quickstart.md`).

**Target Platform**: ESP32-S3, 2.9" e-paper 296×128, 1-bit monochrome (`setMonoMode(true)`), 12-button keypad.

**Project Type**: Single embedded firmware project (components + modules).

**Performance Goals**: Open a within-limit image and show it ≤ 5 s (SC-003); render Markdown up to ~64 KB; 20+ open/close cycles with no memory growth (SC-007).

**Constraints**: Image source ≤ 512 KB / decoded ≤ ~1 megapixel (FR-007); Markdown source ≤ ~64 KB (truncate beyond). Internal SRAM is the scarce pool — only the ~1.2 KB Floyd-Steinberg error rows may sit in SRAM; everything else PSRAM. Untrusted input: decoders parse attacker-controlled files and MUST fail closed. **Library footprint is itself budgeted**: each format is behind a compile flag (`FEATURE_IMG_JPEG/PNG/GIF`, `FEATURE_MARKDOWN`, default on), measured with `idf.py size-components`, and dropped/default-off if its flash is not worth it (see [research.md](./research.md) Decision 9 — verified: JPEG ~0–5 KB, PNG ~30–40 KB, GIF ~20–40 KB flash; none "hundreds of KB").

**Scale/Scope**: Single-user device; two viewers, one decode/dither library component, one Markdown parser, four host calls, one explorer dispatch change.

## Constitution Check

*GATE: must pass before Phase 0 and re-checked after Phase 1.*

- **I. Module Isolation** — PASS. New viewers live in core `cdc_views`; the decode/dither engine is a new leaf component `cdc_image` (no module references). `mod_vfat` already depends on `cdc_views`; `plugin_manager` already pushes `cdc::ui::InfoView`, so both entry points depend on `cdc_views` already. No core→module reference is introduced. `cdc_image` is deletable (only `cdc_views` would reference it).
- **II. PSRAM-First** — PASS. Decoded grayscale buffers, 1-bit bitmaps, Markdown source, and the styled-line model all use `cdc::core::psramAlloc` / `MALLOC_CAP_SPIRAM`. Decoder working memory (pngle inflate window, AnimatedGIF LZW dictionary) is forced to PSRAM. Only the two Floyd-Steinberg error rows (~1.2 KB) may stay in SRAM for speed.
- **III. Security & Sandbox (NON-NEGOTIABLE)** — PASS with required care. The fs-family calls reuse `resolvePath` (sandbox confinement + `vfat`). The ui-family buffer calls require no capability (display is unprivileged per spec) but MUST `wbuf_ok` the full `(ptr,len)` extent. Decoders run on untrusted input: enforce the size limits BEFORE decode, cap decoded dimensions, bound every allocation, and treat any decode error as a clean failure. Security docs (`website/src/content/docs/security/`) updated.
- **IV. Simplicity & Surgical Change** — PASS. Reuse `host_fs_view`/`host_ui_push_info` patterns, `ViewStack`, `Fonts.h`/`drawText`, `psramAlloc`. `InfoView` is left untouched: its monolithic single-font in-place wrap cannot express per-line fonts/heights, so a focused new `MarkdownView` with a styled-line model is the simplest correct approach, not a speculative abstraction.
- **V. Versioning & Pre-1.0** — PASS. New host calls grow the API surface; per Principle V the `HOST_API_LEVEL_*` bump is **deferred to an explicit user instruction** (flagged below). `host_api.h` and the SDK mirror are edited byte-identically in the same change. No on-device format is introduced, so no migration code.

**No violations → Complexity Tracking is empty.**

> **Open item for the user (versioning):** these four host calls would normally imply `HOST_API_LEVEL` 0.7 → 0.8. Per Principle V this is NOT done automatically. Confirm the bump (and the matching SDK mirror commit) when ready.

## Project Structure

### Documentation (this feature)

```text
specs/017-image-markdown-viewers/
├── plan.md              # This file
├── research.md          # Phase 0: decoder/dither/parser decisions
├── data-model.md        # Phase 1: entities (bitmap, styled-line model, requests)
├── quickstart.md        # Phase 1: end-to-end validation guide
├── contracts/
│   └── host-api-content-viewers.md   # Phase 1: the 4 new host calls + behavior
└── checklists/
    └── requirements.md  # Spec quality checklist (from /speckit-specify)
```

### Source Code (repository root)

```text
components/
├── cdc_image/                         # NEW leaf component: decode + dither → 1-bit bitmap
│   ├── CMakeLists.txt                 #   REQUIRES: cdc_core, cdc_log + decoder deps
│   ├── idf_component.yml              #   espressif/esp_jpeg (component manager, not mirrored)
│   ├── include/cdc_image/
│   │   ├── ImageDecoder.h             #   decode any of PNG/JPG/GIF (auto-detect) → grayscale rows
│   │   └── Dither.h                   #   Floyd-Steinberg grayscale→1-bit
│   └── src/
│       ├── ImageDecoder.cpp           #   dispatch by magic bytes; streaming row callback
│       └── Dither.cpp
third_party/                           #   pngle, AnimatedGIF as git submodules (pointer only)
├── cdc_views/                         # EXISTING core views
│   ├── include/cdc_views/
│   │   ├── ImageView.h                #   NEW: fit/actual-pan viewer over a 1-bit bitmap
│   │   ├── MarkdownView.h             #   NEW: scrollable styled-line renderer
│   │   └── MarkdownParser.h           #   NEW: Markdown source → StyledLine model
│   ├── src/
│   │   ├── ImageView.cpp              #   NEW
│   │   ├── MarkdownView.cpp           #   NEW
│   │   └── MarkdownParser.cpp         #   NEW
│   └── CMakeLists.txt                 #   add new srcs; REQUIRES += cdc_image
├── mod_vfat/src/VfatExplorerView.cpp  # EDIT: isImage()/isMarkdown() dispatch in openEntry()
└── plugin_manager/
    ├── include/plugin_manager/host_api.h   # EDIT: declare 4 new calls (mirror to SDK)
    └── src/
        ├── host_api_fs.cpp            # EDIT: host_fs_view_image / host_fs_view_markdown
        ├── host_api_ui.cpp            # EDIT: host_ui_view_image / host_ui_view_markdown
        └── WamrImports.cpp            # EDIT: 4 wrappers + NativeSymbol entries

assets/i18n/lang_de.json               # EDIT: German strings for new keys
components/cdc_ui/src/I18n.cpp          # EDIT: English fallbacks (kCoreStrings[])
website/src/content/docs/...            # EDIT: explorer + host-api + plugin-sdk pages (FR-023)
platformio.ini                         # EDIT (if needed): pull esp_jpeg via component manager
~/GIT/cdc-badge-plugins/sdk/host_api.h  # EDIT: byte-identical mirror of the 4 declarations
```

**Structure Decision**: A new leaf component `cdc_image` isolates the heavy, third-party decoders (keeping `cdc_views` lean and the decoders droppable), while the two views live in core `cdc_views` so that both `mod_vfat` and `plugin_manager` reach them without a module dependency. The Markdown parser is pure logic and stays in `cdc_views` next to its only consumer (`MarkdownView`); it is host-testable. Per-format compile flags (`FEATURE_IMG_JPEG/PNG/GIF`, `FEATURE_MARKDOWN`) are added to `components/cdc_core/include/cdc_core/feature_flags.h` (default `1`, `#ifndef` pattern) so `cdc_image` compiles in only the decoders a build wants and any over-budget format can be removed without code edits.

## Complexity Tracking

No constitution violations. Section intentionally empty.
