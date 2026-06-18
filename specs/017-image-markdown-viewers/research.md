# Phase 0 Research: Image & Markdown Content Viewers

All open technical unknowns from the spec are resolved here. Versions are current as of 2026-06-18 and MUST be re-confirmed against the official source before the dependency is pinned (project rule: verify versions at use time).

## Decision 1 — JPEG decoder

**Decision**: `espressif/esp_jpeg` v1.3.1 (TJpgDec wrapper), Apache-2.0, from the Espressif Component Registry.

**Rationale**: Registry component built for ESP-IDF; per-block streaming output callback so each MCU is dithered without a full RGB buffer; scale-on-decode (1/1, 1/2, 1/4, 1/8) lets oversized photos be reduced toward the panel before dithering; can link the ESP32-S3 ROM TJpgDec to save ~5 KB flash; tiny working RAM (~3 KB).

**Alternatives considered**: `espressif/esp_new_jpeg` v0.6.1 (SIMD-accelerated on S3, but no GRAY output, block-mode and scale are mutually exclusive, and its MIT-style license is restricted to Espressif products) — kept as a fallback if decode speed becomes the bottleneck. Hardware JPEG: confirmed **not** present on plain ESP32-S3 (only ESP32-P4).

## Decision 2 — PNG decoder

**Decision**: `pngle` v1.1.0 (kikuchan), MIT, added as a git submodule under `third_party/pngle` (not on the ESP registry). Its `pngle.c` is compiled by `cdc_image`; no source is mirrored into this repo's history.

**Rationale**: True per-pixel streaming callback `(x, y, rgba)` — dither straight into the 1-bit framebuffer with no full-image buffer; bundles `miniz` for inflate (no external zlib dependency); C99, ESP32-proven. Working RAM ~43 KiB (inflate window + scanline), placed in PSRAM.

**Alternatives considered**: `libspng` v0.7.4 + miniz (BSD-2, row-progressive, more standards-complete but heavier) — fallback; `lodepng` (zlib license, single file, but decodes a full in-RAM image — weakest fit for the RAM constraint).

## Decision 3 — GIF decoder (first frame only)

**Decision**: `AnimatedGIF` (bitbank2), Apache-2.0, added as a git submodule under `third_party/AnimatedGIF` (not on the ESP registry); call `playFrame()` exactly once. Compiled by `cdc_image`; no source mirrored into this repo.

**Rationale**: Per-scanline `GIFDRAW` callback delivering 8-bit palette indices (+ palette) — streaming-dither friendly; no malloc, no external deps; file read/seek callbacks suit the 512 KB FS source cap. LZW dictionary ~20 KB in PSRAM (skip the optional 32 KB Turbo mode).

**Alternatives considered**: `gifdec` (public domain, tiny) but non-streaming (renders a full RGB frame into PSRAM) — fallback only.

## Decision 4 — Color reduction / dithering

**Decision**: Floyd-Steinberg error diffusion, grayscale → 1-bit, panel stays in mono mode (`setMonoMode(true)`).

**Rationale**: Best perceived tonal range on a 2-color panel ("so gut wie möglich", FR-003). Memory pattern is two error rows of `int16_t[width+2]` (~1.2 KB total at width 296), sized by panel width and constant regardless of source size; these may stay in SRAM for speed. Each decoded/scaled row is dithered directly into the packed 1-bit framebuffer (296×128/8 ≈ 4.7 KB). The 4-level gray mode (`setMonoMode(false)`) is intentionally **not** used (FR: monochrome; avoids gray-waveform refresh complexity).

**Alternatives considered**: ordered (Bayer) dithering — simpler but visibly worse on photos; 4-level grayscale panel mode — rejected to keep refresh behavior and the spec's monochrome model intact.

## Decision 5 — Image render strategy (fit vs actual-size pan)

**Decision**: Produce one persistent 1-bit bitmap in PSRAM. Fit mode dithers a downscaled image (≤ panel size, ~4.7 KB). Actual-size mode keeps the native-resolution 1-bit bitmap (≤ 1 MP / 8 ≈ 131 KB) and `ImageView` blits a 296×128 window of it via `updateWindow`, moving the pan offset with the directional keys. Toggle between modes on a dedicated key.

**Rationale**: 1-bit at native resolution is small (≤131 KB), so both modes are cheap; downscaling for fit uses the decoder's scale-on-decode (JPEG) or accumulation during the streaming callback (PNG/GIF). Avoids re-decoding when toggling by keeping the native bitmap and deriving the fit view from it where practical.

## Decision 6 — Markdown rendering architecture

**Decision**: A standalone `MarkdownParser` turns the source into a PSRAM `StyledLine` model; a new `MarkdownView` scrolls and renders it. `InfoView` is left unchanged.

**Rationale**: `InfoView` wraps in place into a single buffer and renders char-by-char with one font and a fixed `LINE_HEIGHT` — it cannot express per-line fonts or variable heights. Retrofitting it would be invasive and risk regressing every existing info screen. A dedicated styled-line model (per line: font id, indent, list marker, inverted flag, text span, height) cleanly supports headings/quotes/code/lists and is reusable by any view that consumes it (FR-011). Drawing reuses `cdc::ui::render::drawText(gfx, text, font)` and `Fonts.h`.

**Heading font assignment** (FR-012, clarified): collect the distinct heading levels present; sort by prominence; assign the smallest heading font to the least-prominent level and step up one font per higher level, so the number of fonts used equals the number of distinct levels. Available bold fonts ascending: `Bold9pt < Bold12pt < Bold18pt`. (`Bold24pt` reserved; on a 6-line panel three heading sizes are the practical maximum, deeper levels clamp to the largest assigned.)

**Markdown element scope** (FR-009/FR-010): headings, ordered/unordered lists, inline code + fenced/indented code blocks, block quotes, horizontal rules, bold/italic emphasis, links (visible text only). Unsupported constructs (tables, HTML, footnotes, nested quotes > 1, inline images → alt text) degrade to plain text.

**Alternatives considered**: extend `InfoView` (rejected, Decision rationale above); pull a third-party Markdown library (rejected — oversized for the basic element set and the CP437/font pipeline is bespoke).

## Decision 7 — Host API surface & capability mapping

**Decision**: Four new host calls, mirroring existing patterns:

| Call | Family / file | Capability | WAMR sig |
|------|---------------|-----------|----------|
| `host_fs_view_image(name)` | fs / `host_api_fs.cpp` | `vfat` (via `resolvePath`) | `($)i` |
| `host_fs_view_markdown(name)` | fs / `host_api_fs.cpp` | `vfat` | `($)i` |
| `host_ui_view_image(data, len)` | ui / `host_api_ui.cpp` | none (`wbuf_ok` full extent) | `(*~)i` |
| `host_ui_view_markdown(data, len)` | ui / `host_api_ui.cpp` | none (`wbuf_ok` full extent) | `(*~)i` |

**Rationale**: The family split encodes the clarified capability model directly — fs-family already gates on `vfat` and confines to the sandbox; ui-family is unprivileged (display is not a privilege) and only validates buffer bounds. Image calls auto-detect format from magic bytes (PNG `89 50 4E 47`, JPEG `FF D8 FF`, GIF `47 49 46 38`), so no format argument is needed. Each call reads/validates the source, decodes-or-parses, and pushes `ImageView`/`MarkdownView` via `ViewStack::instance().push(...)`; back returns to the plugin exactly as `host_ui_push_info` does today.

**Alternatives considered**: a single `host_view_content(kind, source...)` multiplexer (rejected — less discoverable, mixes the capability boundary); a dedicated `content_view` capability (rejected per the spec clarification — display is unprivileged).

**Deferred**: the `HOST_API_LEVEL_*` bump (Principle V — explicit user instruction required) and the byte-identical SDK-mirror commit are done together at implementation time on the user's go-ahead.

## Decision 8 — Component & build placement

**Decision**: New leaf component `components/cdc_image` (decoders + dither, no UI deps, `REQUIRES cdc_core cdc_log`). Dependencies are pulled, never mirrored into git: `espressif/esp_jpeg` via the ESP component manager (`idf_component.yml` → `managed_components/`, gitignored, like `qrcode`/`tinyusb`); `pngle` and `AnimatedGIF` as git submodules under `third_party/` (the existing pattern for `third_party/libtropic` / `components/CalEPD`), with their `.c`/`.cpp` listed in `cdc_image/CMakeLists.txt` behind the `FEATURE_IMG_*` flags. Views go in `cdc_views` (`REQUIRES += cdc_image`).

**Rationale**: Isolates heavy third-party C from core views, keeps decoders droppable, respects Module Isolation (leaf lib, no module refs), and keeps third-party source out of this repo's history (pointer/registry only). IDF-5.5 build of `esp_jpeg` is unverified upstream → confirm with a clean `pio run` before committing the dependency.

## Decision 9 — Library footprint budget & per-format gating

**Decision**: Gate each image format behind its own compile flag — `FEATURE_IMG_JPEG`, `FEATURE_IMG_PNG`, `FEATURE_IMG_GIF` (and `FEATURE_MARKDOWN`) in `cdc_core/feature_flags.h`, default `1` via the existing `#ifndef` pattern. `cdc_image` compiles a decoder only when its flag is on; the explorer/host dispatch reports "unsupported format" for a disabled one. Measure real cost with `idf.py size-components` (or the `pio run` size report) and **drop or default-off any format whose measured flash is not worth it.**

**Rationale**: Directly answers the frugality requirement at the library level — a format is included only if it earns its flash. Verified footprints (re-confirm by build):

| Format | Library | Transient RAM (PSRAM) | Flash (code+rodata) | Keep? |
|--------|---------|------------------------|---------------------|-------|
| JPEG | esp_jpeg / TJpgDec | ~3 KB | ~4–5.5 KB (lib) or ~0 (ROM tjpgd) | Always — cheapest, ROM-backed |
| PNG | pngle + bundled miniz | ~43 KiB | ~30–40 KB | Default on; re-evaluate vs measured flash |
| GIF | AnimatedGIF | ~20 KB | ~20–40 KB | Default on; re-evaluate vs measured flash |

None are hundreds of KB, so all three start enabled; the flags + size measurement give a clean drop path if a build shows otherwise. JPEG is retained unconditionally (smallest and most common). The two error rows (~1.2 KB) are the only SRAM cost; all decoder working memory is forced to PSRAM.

**Alternatives considered**: shipping all decoders unconditionally (rejected — no drop path, pays flash for unused formats); a single runtime config (rejected — runtime cost with no flash saving; compile-gating actually removes the code).

## Open risks

- **IDF 5.5 + esp_jpeg**: not upstream-declared; verify by clean build (fallback `esp_new_jpeg` or ROM tjpgd).
- **Untrusted decode**: enforce size caps pre-decode and bound allocations; fuzz-sized inputs must fail closed (Principle III).
- **Internal-RAM creep**: keep only the ~1.2 KB dither error rows in SRAM; everything else PSRAM (Principle II).
