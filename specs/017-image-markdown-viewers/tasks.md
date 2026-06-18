---

description: "Task list for Image & Markdown Content Viewers"
---

# Tasks: Image & Markdown Content Viewers

**Input**: Design documents from `/specs/017-image-markdown-viewers/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/host-api-content-viewers.md, quickstart.md

**Tests**: Host unit tests for pure-logic parts are included because plan.md explicitly calls for them (`pio test -e native`: Markdown parser, dither, format detection, heading-font mapping). On-target rendering/decoding is hardware-verified, not unit-tested.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: US1 / US2 / US3 (maps to spec.md user stories)
- Exact file paths are included in each task.

## Path notes

ESP-IDF/PlatformIO firmware. Components under `components/`, modules under `components/mod_*`, host tests under `test/host/` (project-relative `../` includes only). The plugin SDK mirror lives in the sibling repo `~/GIT/cdc-badge-plugins/`.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create the decode/dither component, pull/vendor decoders, add gating flags, baseline the footprint.

- [ ] T001 Create the `components/cdc_image/` skeleton: `CMakeLists.txt`, `include/cdc_image/`, `src/`; `REQUIRES cdc_core cdc_log`. (Skeleton + pure sources DONE; the `idf_component.yml` esp_jpeg dep lands with T003.)
- [X] T002 [P] Add per-format compile flags `FEATURE_IMG_JPEG`, `FEATURE_IMG_PNG`, `FEATURE_IMG_GIF`, `FEATURE_MARKDOWN` (default `1`, `#ifndef` pattern) to `components/cdc_core/include/cdc_core/feature_flags.h`.
- [ ] T003 Add dependencies WITHOUT mirroring source: `pngle` and `AnimatedGIF` as git submodules under `third_party/` (pinned to release tags), `espressif/esp_jpeg` via `components/cdc_image/idf_component.yml` (component manager). List the submodule sources in `cdc_image/CMakeLists.txt` behind `FEATURE_IMG_PNG`/`GIF` and require `esp_jpeg` behind `FEATURE_IMG_JPEG`. (Submodules `third_party/pngle`@v1.1.0 + `third_party/AnimatedGIF`@2.2.0 added; `esp_jpeg` v1.3.1 fetched via component manager. CMakeLists source wiring lands with T009.)
- [ ] T004 Verify clean build pulls `esp_jpeg` on ESP-IDF 5.5 and record the baseline per-component flash with `idf.py size-components`; build via `~/.platformio/penv/bin/pio run` (footprint gate, research.md Decision 9). (esp_jpeg fetch + `jpeg_decoder.c` compile on IDF 5.5 VERIFIED, firmware green; `size-components` footprint measurement pending → T029.)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Wire the new component into core views so any view referencing it compiles.

**⚠️ CRITICAL**: Blocks US1 and US3 (image path); US2 view code also lands in `cdc_views`.

- [X] T005 Wired `cdc_views` to `cdc_image` (REQUIRES + `ImageView.cpp`/`MarkdownView.cpp`/`MarkdownParser.cpp` in SRCS); firmware builds green.

**Checkpoint**: Foundation ready — user stories can begin.

---

## Phase 3: User Story 1 - View an image file from the file explorer (Priority: P1) 🎯 MVP

**Goal**: Select a PNG/JPEG/GIF in the vFAT explorer and see it dithered on the e-paper, fit-to-screen with an actual-size pan mode; errors never crash.

**Independent Test**: Copy a PNG, JPEG and (animated) GIF plus one oversized image to the partition; open each from the explorer; verify recognizable rendering, fit↔1:1 pan toggle, oversized/corrupt → readable error, back → explorer.

### Tests for User Story 1 (host, pure logic)

- [X] T006 [P] [US1] Host test for image format magic-byte detection (PNG/JPEG/GIF/unknown) in `test/host/test_image_engine/test_image_engine.cpp` (consolidated with T007). PASSES via `pio test -e native`.
- [X] T007 [P] [US1] Host test for Floyd-Steinberg grayscale→1-bit dither (all-black/all-white/monotonic) in `test/host/test_image_engine/test_image_engine.cpp`. PASSES.

### Implementation for User Story 1

- [X] T008 [P] [US1] Define `MonoBitmap` (1-bpp bitmap) and Floyd-Steinberg `Dither` (two caller-owned `int16_t[width+2]` error rows) in `components/cdc_image/include/cdc_image/MonoBitmap.h`, `Dither.h` and `components/cdc_image/src/Dither.cpp`. Host-verified.
- [X] T009 [US1] Implemented `cdc::image::decodeToGray` (+ `downscaleGrayBox`, `ditherImage`) in `components/cdc_image/`: magic-byte dispatch; **PNG via `espressif/libpng`** simplified API (`PNG_FORMAT_GRAY`, white bg); **JPEG via vendored IJG libjpeg** (`JCS_GRAYSCALE`, scale_denom, setjmp, baseline **and progressive**); ≤1 MP cap; PSRAM (libjpeg routed via `jmem_psram.c`); fail-closed. GIF dropped. Builds + links.
- [X] T010 [US1] Implemented `ImageView` (fit default + actual-size pan via key 5, dir-key pan, back) + `showImage(title, data, len)` in `components/cdc_views/{include/cdc_views/ImageView.h,src/ImageView.cpp}`; builds fit + native 1-bit bitmaps, blits via `gfx->drawBitmap`. On-device visual tuning pending.
- [X] T011 [US1] Added `isImage()` (`png/jpg/jpeg`) dispatch in `VfatExplorerView::openEntry()` before `isViewable()`; binary `readText` (512 KB cap) → `cdc::ui::showImage`, toast on read failure.
- [X] T012 [US1] Added i18n keys `core.img_too_large`, `core.img_unsupported`, `core.img_decode_failed`, `core.hint_image` to `cdc_ui/src/I18n.cpp` (EN) AND `assets/i18n/lang_de.json` (DE).
- [X] T013 [P] [US1] Updated `website/src/content/docs/power/storage-tools.md` (image viewing: formats, fit/pan keys, limits, baseline+progressive) and added `dev/third-party-licenses.md` (IJG/libpng/zlib).

**Checkpoint**: Explorer image viewing fully functional and independently testable (MVP).

---

## Phase 4: User Story 2 - Read a rendered Markdown file from the file explorer (Priority: P2)

**Goal**: Open a `.md` from the explorer and see it formatted in the scrollable viewer (headings in larger bold fonts assigned smallest-first by distinct-level count; lists, code, quotes, rules distinct); `.txt` unaffected.

**Independent Test**: Open a `.md` with headings/lists/code/quote → formatted, no raw markup; verify a single-`#` doc is not largest font and a `#`/`##`/`###` doc shows three ascending sizes; scroll; >64 KB doc truncates with indicator; `.txt` stays plain.

### Tests for User Story 2 (host, pure logic)

- [X] T014 [P] [US2] Host test for `MarkdownParser` → `StyledLine` model: block kinds + degrade-to-plain (tables), in `test/host/test_markdown/test_markdown.cpp` (consolidated with T015). PASSES.
- [X] T015 [P] [US2] Host test for heading-level → font mapping (smallest-first, fonts-used == distinct-levels, gaps ignored) in `test/host/test_markdown/test_markdown.cpp`. PASSES.

### Implementation for User Story 2

- [X] T016 [P] [US2] Implement `MarkdownParser` (source → `StyledLine` via sink: headings, ordered/unordered lists, fenced/indented code, block quote depth-1, horizontal rule; unsupported→plain; truncate at ~64 KB; dynamic heading-font map) in `components/cdc_views/include/cdc_views/MarkdownModel.h`, `MarkdownParser.h` + `src/MarkdownParser.cpp`. Block-level done + host-verified; inline emphasis/links/`code` spans are applied by `MarkdownView` at draw time (T017).
- [X] T017 [US2] Implement `MarkdownView` (IView) with variable line heights, per-line `render::drawText` + indent/marker/inverse, scroll + position/truncation indicator, back, plus `showMarkdown(title, src, len)` push helper, in `components/cdc_views/include/cdc_views/MarkdownView.h` + `src/MarkdownView.cpp`. Compiles + links into firmware (`pio run` green); inline emphasis markers stripped at draw time; on-device visual tuning (baseline/blit) pending hardware.
- [X] T018 [US2] Route `.md`/`.markdown` in `components/mod_vfat/src/VfatExplorerView.cpp` `openEntry()` to `cdc::ui::showMarkdown` (64 KB read cap) while other text-like extensions stay on `InfoView`; added `markdown` to the viewable whitelist. `pio run` green.
- [X] T019 [US2] Added `core.md_truncated` to `components/cdc_ui/src/I18n.cpp` (`kCoreStrings[]`, EN) AND `assets/i18n/lang_de.json` (DE); footer reuses existing `core.hint_scroll_back`.
- [X] T020 [P] [US2] Updated `website/src/content/docs/power/storage-tools.md` with a "Viewing files" section (element set, dynamic smallest-first heading sizing, 64 KB cap, fallback).

**Checkpoint**: Explorer image AND Markdown viewing both work independently.

---

## Phase 5: User Story 3 - A plugin displays a referenced image or Markdown document (Priority: P3)

**Goal**: Four host calls let a plugin open the image/Markdown viewer from a sandboxed file (fs family, `vfat`) or an in-memory buffer (ui family, no capability); back returns to the plugin.

**Independent Test**: With a test plugin, exercise all four (file/buffer × image/Markdown), plus negative cases (file outside sandbox; `(ptr,len)` exceeding linear memory) → rejected, no crash; back → plugin view.

> Depends on US1 (`showImage`) and US2 (`showMarkdown`) being implemented.

### Implementation for User Story 3

- [X] T021 [US3] Declared the four calls in `host_api.h` (backslash-Doxygen). Links into firmware.
- [X] T022 [US3] Mirrored `host_api.h` byte-identically to `~/GIT/cdc-badge-plugins/sdk/host_api.h` (`diff -q` clean).
- [X] T023 [US3] Implemented `host_fs_view_image`/`host_fs_view_markdown` in `host_api_fs.cpp` (`resolvePath`+`vfat`, PSRAM read, `showImage`/`showMarkdown`, `toDisplay` title/body).
- [X] T024 [US3] Implemented `host_ui_view_image`/`host_ui_view_markdown` in `host_api_ui.cpp` (no capability; `showImage`/`showMarkdown`; markdown via `toDisplay`).
- [X] T025 [US3] Registered four WAMR wrappers + symbols in `WamrImports.cpp` (`($)i`,`($)i`,`(*~)i`,`(*~)i`; ui wrappers assert `wbuf_ok`). Builds + links.
- [X] T026 [P] [US3] Added the four calls to `website/src/content/docs/dev/host-api.md` (fs row = `vfat`, ui row = none).
- [ ] T027 [US3] DEFERRED (do NOT execute without explicit user instruction, Principle V): bump `HOST_API_LEVEL_MINOR`/`STR` (0.7→0.8) in `host_api.h` and the SDK mirror in the same commit.

**Checkpoint**: All three user stories independently functional.

---

## Phase 6: Polish & Cross-Cutting Concerns

- [ ] T028 [P] Run host unit tests `~/.platformio/penv/bin/pio test -e native` (T006/T007/T014/T015 green); fix project-relative include issues.
- [ ] T029 Footprint verification with `idf.py size-components`: confirm JPEG/PNG/GIF flash within budget (research.md Decision 9); set any over-budget `FEATURE_IMG_*` default to `0` and re-run.
- [ ] T030 Memory stability: 20+ open/close cycles per viewer; confirm PSRAM free returns to baseline and internal RAM (`.dram`) does not creep (SC-007).
- [ ] T031 Execute quickstart.md Scenarios 1–4 on hardware; record results.
- [ ] T032 [P] Docs consistency pass across `website/src/content/docs/` (explorer + host-api + plugin-sdk); confirm no new serial command/menu entry was introduced (none planned).
- [ ] T033 Security review of the decoders against `website/src/content/docs/security/`: untrusted-input fail-closed, pre-decode size caps, `wbuf_ok`/sandbox confinement (Principle III).

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: no dependencies.
- **Foundational (Phase 2 / T005)**: depends on Setup; blocks views referencing `cdc_image`.
- **US1 (Phase 3)**: depends on Foundational. Independently testable (MVP).
- **US2 (Phase 4)**: depends on Foundational. Independent of US1.
- **US3 (Phase 5)**: depends on US1 (`showImage`) and US2 (`showMarkdown`).
- **Polish (Phase 6)**: depends on the stories being implemented.

### Within Each User Story

- Host tests (where present) precede the implementation they cover.
- `cdc_image` decode/dither (T008/T009) before `ImageView` (T010) before explorer dispatch (T011).
- `MarkdownParser` (T016) before `MarkdownView` (T017) before explorer routing (T018).
- View show-helpers (T010/T017) before the host calls that reuse them (T023/T024).
- `host_api.h` declaration (T021) + SDK mirror (T022) before WAMR registration (T025).

### Parallel Opportunities

- T002 alongside T001/T003.
- US1: T006 ∥ T007 (tests); T008 ∥ T013 (different files).
- US2: T014 ∥ T015 (tests); T016 ∥ T020.
- US3: T026 alongside the impl tasks.
- Polish: T028 ∥ T032.
- After Foundational, US1 and US2 can be built in parallel by different developers; US3 follows both.

---

## Parallel Example: User Story 1

```bash
# Host tests for US1 together:
Task: "Host test for image format detection in test/host/test_image_detect.cpp"
Task: "Host test for Floyd-Steinberg dither in test/host/test_dither.cpp"

# Independent files together:
Task: "Define MonoBitmap + Dither in components/cdc_image/.../Dither.{h,cpp}"
Task: "Update explorer image-viewing website docs"
```

---

## Implementation Strategy

### MVP First (User Story 1 only)

1. Phase 1 Setup → 2. Phase 2 Foundational → 3. Phase 3 US1 → 4. STOP and validate the explorer image viewer on hardware (quickstart Scenario 1) → demo.

### Incremental Delivery

1. Setup + Foundational → foundation ready.
2. US1 (explorer image viewer) → test → demo (MVP).
3. US2 (Markdown rendering) → test → demo.
4. US3 (plugin host calls, reuse both viewers) → test → demo.
5. Polish: footprint gate, memory stability, security review, quickstart, docs.

### Notes

- [P] = different files, no incomplete-task dependency.
- Per-format flags let an over-budget decoder be dropped without code edits (frugality requirement).
- T027 (API-level bump) stays DEFERRED until the user instructs; the SDK mirror commit goes with it.
- Website docs are part of done (FR-023), not a follow-up.
- No on-device migration code (pre-1.0).
