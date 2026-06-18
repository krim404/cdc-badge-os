# Phase 1 Data Model: Image & Markdown Content Viewers

All structures are transient (live only while a viewer is open) and allocated in PSRAM unless noted. No on-device persisted format is introduced.

## Memory budget (frugality is a hard requirement)

The whole feature is designed so that **no full RGB frame is ever held** and the largest persistent buffer is a 1-bit bitmap. Decoders stream (per-pixel / per-block / per-line) directly into the dither stage.

| Buffer | Where | Peak size | Lifetime |
|--------|-------|-----------|----------|
| Floyd-Steinberg error rows (2 × `int16_t[width+2]`) | SRAM (speed) | ~1.2 KB | during decode only |
| Decoder working memory (pngle inflate window ≈ 43 KB / AnimatedGIF LZW dict ≈ 20 KB / esp_jpeg ≈ 3 KB) | PSRAM | ≤ ~43 KB | during decode only |
| Source bytes when read from a file (fs calls) | PSRAM | ≤ 512 KB | during decode only, freed before view shows |
| **Persistent 1-bit bitmap** — fit mode | PSRAM | 296×128/8 ≈ 4.7 KB | while ImageView open |
| **Persistent 1-bit bitmap** — actual-size mode | PSRAM | ≤ 1 MP/8 ≈ 131 KB | while ImageView open |
| Markdown source | PSRAM | ≤ 64 KB | while MarkdownView open |
| Styled-line model | PSRAM | ~16–48 bytes/line; a 64 KB doc ≈ tens of KB | while MarkdownView open |

Rules enforced:
- Size caps (FR-007: 512 KB / ~1 MP; 64 KB Markdown) are checked **before** any decode/parse allocation; oversize → reject, allocate nothing.
- Streaming decoders are preferred; the non-streaming fallbacks (lodepng/gifdec) are used only if a primary fails to build and their full-frame buffer still lives in PSRAM and is freed immediately after dithering.
- All allocations via `cdc::core::psramAlloc` / `PsramUniquePtr` (RAII free on view exit). Only the ~1.2 KB error rows may use SRAM.
- On `onExit()` every viewer frees its bitmap / model / source so repeated open/close returns heap to baseline (SC-007).

## Entities

### ImageSource
Where image bytes come from. Discriminated:
- `kind`: `File` (sandboxed path, fs calls / explorer) or `Buffer` (validated `(ptr,len)` in plugin linear memory, ui calls).
- `data`/`len` or `path`.
- Validation: `Buffer` must pass `wbuf_ok(env, ptr, len)`; `File` must pass `resolvePath` (sandbox + `vfat`). `len`/file size ≤ 512 KB.

### ImageFormat
Auto-detected from leading magic bytes: `Png` (`89 50 4E 47`), `Jpeg` (`FF D8 FF`), `Gif` (`47 49 46 38`), else `Unknown` → reject.

### MonoBitmap (persistent, PSRAM)
The dithered result the `ImageView` renders.
- `width`, `height` (intrinsic, ≤ ~1 MP).
- `bits`: `PsramUniquePtr<uint8_t>`, packed 1-bpp, MSB-first per row, stride = `(width+7)/8`.
- Produced by the streaming decode→dither pipeline; the only buffer kept alive while viewing.

### ImageView (IView, state)
- `bitmap`: `MonoBitmap`.
- `mode`: `Fit` (default) | `Actual`.
- `panX`, `panY`: window origin into `bitmap` in `Actual` mode (clamped).
- Keys: scroll/pan (directional), toggle mode, back (`REQUEST_POP`). Renders by blitting a 296×128 window (drawPixel/`drawBitmap`) and `updateWindow` for pan.

### MarkdownDocument → StyledLine[] (PSRAM)
Parser output consumed by the view (reusable per FR-011).
- `StyledLine`: `{ FontId font; uint8_t indentCols; char marker; bool inverted; const char* text; uint16_t len; uint8_t heightPx; }`.
  - `font` ∈ `Fonts.h` `FontId` (Builtin/Bold9/12/18pt); `marker` for list bullet/number/quote bar; `inverted` for emphasis/code background; `heightPx` derived from the font.
- `headingFontMap`: computed mapping `distinct-heading-level → FontId`, smallest-first (FR-012). Built in one pass: collect present levels → sort → assign ascending fonts.
- Block kinds recognized: heading, paragraph, unordered/ordered list item, code (inline span / fenced / indented block), block quote (depth 1), horizontal rule. Unsupported → plain paragraph text (FR-010).

### MarkdownView (IView, state)
- `lines`: the `StyledLine[]` model + `lineCount`.
- `scrollLine`, `topPixel`: variable line heights, so scrolling advances by line and tracks pixel offset.
- `truncated`: set when source exceeded 64 KB → footer/indicator (edge case).
- Keys: up/down scroll, back. Renders via `render::drawText(gfx, line.text, kFonts[line.font])` with per-line indent/marker/inverse; reuses chrome helpers (header/footer).

### DisplayRequest (control flow, not stored)
What the explorer or a host call constructs: `{ contentKind: Image|Markdown|Text; ImageSource/MdSource }`. Routes to `ImageView`, `MarkdownView`, or the existing `InfoView` (text).

## Validation & failure rules (FR-018/019)

- Any decode/parse error, unknown/zero-length input, or empty file → readable i18n error (toast/info), all buffers freed, badge responsive. Never a partial/garbled render.
- Animated GIF → first frame only (`playFrame()` once); transparency composited onto white before dithering (FR-004/005).
- Markdown > 64 KB → truncate at limit, set `truncated`, still render+scroll the shown portion.
