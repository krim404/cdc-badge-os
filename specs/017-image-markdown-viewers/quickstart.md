# Quickstart: Validate Image & Markdown Content Viewers

End-to-end validation guide. Details of the structures and host calls live in
[data-model.md](./data-model.md) and [contracts/host-api-content-viewers.md](./contracts/host-api-content-viewers.md).

## Prerequisites

- Badge on USB (`/dev/cu.usbmodem*`, enumerates as `BadgeV1`).
- Build tool: `~/.platformio/penv/bin/pio`.
- Test assets: a small PNG, JPEG, and (animated) GIF each ≤ 512 KB and ≤ ~1 MP, one oversized image (> limits), one `.md` with headings/lists/code/quote, and a > 64 KB `.md`.
- Upload tool: `tools/upload.py` (or the explorer's add-file flow) to place files on the `/plugins` partition.

## Build & flash

```bash
~/.platformio/penv/bin/pio run                 # must be clean (confirms esp_jpeg builds on IDF 5.5)
# AUTH <pin> then BOOTLOADER over CDC, then:
~/.platformio/penv/bin/pio run -t upload
```

## Footprint check (frugality gate — do this before committing the deps)

```bash
idf.py size-components        # inspect cdc_image + pngle/AnimatedGIF/esp_jpeg flash
```

Expected order of magnitude (research.md Decision 9): JPEG ~0–5 KB, PNG ~30–40 KB, GIF ~20–40 KB flash. If any format costs materially more than budgeted, set its `FEATURE_IMG_*` flag to `0` (or drop it) and rebuild. Also confirm internal-RAM (`.dram`) growth is negligible — only the ~1.2 KB dither error rows should be in SRAM.

## Scenario 1 — Image from the explorer (US1 / SC-001..003)

1. Open the vFAT explorer, select the PNG → renders scaled-to-fit, centered, dithered; header shows the file name; appears within 5 s.
2. Press the actual-size key → image shown 1:1; directional keys pan; press again → back to fit.
3. Repeat for the JPEG and the GIF (GIF shows the first frame only).
4. Open the oversized image → readable "too large" message, no crash. Open a truncated/garbage `.png` → readable error, badge responsive.
5. Press back → returns to the explorer list at the previous position.

## Scenario 2 — Markdown from the explorer (US2 / SC-004)

1. Select the `.md` with headings/lists/code/quote → opens in the scrollable viewer **formatted**: headings in larger bold fonts, list items bulleted/numbered, code monospaced, quote/rule distinct; no raw `#`/`*`/`` ` `` shown.
2. Verify heading sizing: a doc using only `#` does NOT use the largest font; a doc using `#`,`##`,`###` shows three ascending sizes (smallest assigned first).
3. Scroll with up/down; verify position indicator. Open the > 64 KB doc → truncation indicator, scroll still works.
4. Open a `.txt` → unchanged plain text (no Markdown formatting).

## Scenario 3 — Plugin-triggered display (US3 / SC-006)

Using a test plugin (build against the SDK mirror once the 4 calls are added):

1. `host_fs_view_image("pic.png")` (plugin bundles `pic.png`, has `vfat`) → image viewer opens.
2. `host_ui_view_image(buf, len)` with image bytes in WASM memory (no `vfat`) → image viewer opens.
3. `host_ui_view_markdown(buf, len)` with Markdown bytes → rendered viewer opens.
4. `host_fs_view_markdown("doc.md")` → rendered viewer opens.
5. Negative: call with a file outside the sandbox, or a `(ptr,len)` exceeding linear memory → rejected with an error, no crash.
6. Press back from any → returns to the plugin's view.

## Scenario 4 — Memory stability (SC-007)

1. Open and close an image and a Markdown doc 20+ times each.
2. Watch heap (serial `MEM`/diagnostics or `heap_caps_get_free_size`) → free PSRAM returns to baseline after each close; badge stays responsive; no internal-RAM creep.

## Host-side unit tests

```bash
~/.platformio/penv/bin/pio test -e native
```

Cover the pure logic: Markdown parse → styled-line model (each element kind + degrade-to-plain), heading-level → font mapping (smallest-first, distinct-count), format magic-byte detection, and Floyd-Steinberg output for a known gradient. Rendering and decode-on-target are verified by Scenarios 1–4.
