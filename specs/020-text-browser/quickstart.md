# Quickstart & Validation: Text Browser

How to build, host-test, flash, and validate `mod_browser` end-to-end. References
[contracts/](./contracts/) and [data-model.md](./data-model.md) instead of repeating detail.

## Prerequisites

- Toolchain: `~/.platformio/penv/bin/pio` (ESP-IDF env `cdc_badge_usb`).
- A 2.4 GHz WiFi network the badge can join (WiFi configured via the existing connectivity
  settings). The Browser requires an active connection (FR-004).
- Dev device PIN `0000`; serial bootloader entry via `AUTH 0000` then `BOOTLOADER`.

## 1. Host unit tests (pure logic - run first, no hardware)

The tokenizer, extractor, feed parser, and source selector are host-testable (no GFX/IDF deps),
mirroring `test/host/test_markdown/`.

```bash
~/.platformio/penv/bin/pio test -e native -f test_html_extract
```

Expected: all fixtures pass - split-chunk invariance, `<script>/<style>` suppression, lenient
malformed HTML (no crash), jusText-lite keeps article prose and drops nav/footer, `<article>`
preference, head-signal capture (og/description/canonical/amphtml/feed), RSS full-text vs
summary-only, CDATA/entity bodies, and `SourceSelector` branch selection. See the contract files
for the exact cases.

## 2. Build firmware

```bash
~/.platformio/penv/bin/pio run
```

Expected: clean build. Deleting `components/mod_browser/` and removing its `main/CMakeLists.txt`
MODULES entry must still build (module isolation, Principle I).

## 3. Flash

```bash
# with the badge on USB and authenticated:  AUTH 0000  then  BOOTLOADER  over CDC
~/.platformio/penv/bin/pio run -t upload
```

Auto-reboots into the new firmware (no manual reset needed via the serial bootloader path).

## 4. On-device validation (maps to Success Criteria)

Open **Menu -> Tools -> Browser**.

1. **Offline guard (FR-004)**: with WiFi off, opening Browser (or loading a URL) shows an
   actionable "connect WiFi" message, not a hang.
2. **Read an article (US1 / SC-001, SC-003)**: connect WiFi, enter a Wikipedia article URL
   (T9). Expect the article title + main prose, wrapped to the display, vertical scroll only
   (keys 2/8); navigation column / edit links / footer chrome suppressed. Footer shows the URL
   (FR-021).
3. **Source preference (SC-008)**: load a blog/news URL that advertises an RSS feed; confirm the
   feed/clean source is used (verify via serial log line naming `sourceKind`). Load a page with
   `rel=amphtml`; confirm the AMP variant is preferred when no feed is present.
4. **Follow links (US2 / SC-004)**: press `3` to open the links list, select a link, confirm the
   target loads; press `N` to return to the previous page; `N` at the first page exits Browser.
5. **Recents (US3 / SC-007)**: leave and re-enter Browser; the visited URL appears in recents and
   reopens in one selection. Reboot -> recents are gone (session-only, FR-019).
6. **Big-page memory safety (SC-005)**: load a deliberately huge HTML page (e.g. a large
   single-file HTML dump > 1 MB). Expect bounded memory (no OOM/crash), content scanned up to
   512 KB, and a truncation indicator. Watch the serial log / `tools/coredump.py` for any panic.
7. **Failure modes (SC-006)**: bad host (DNS fail), unreachable host (timeout), an `https://`
   URL with an invalid cert (TLS fail), a 404, a non-HTML URL (e.g. a PDF or image) -> each shows
   a readable on-screen message; `text/plain` URLs render as scrollable text. A back press during
   a slow load cancels it (FR-014).
8. **JS-SPA limitation (documented)**: a JS-rendered SPA shows the OG card or a "requires
   JavaScript / no readable content" notice, not garbage.

## 5. Memory check

During step 6, confirm internal-SRAM free heap stays stable across loads (no growth proportional
to page size). The tokenizer/extractor working set is a few KB internal; scan window, source
(64 KB), link table, and recents are PSRAM.

## 6. Docs

Confirm the Tools page under `website/src/content/docs/` documents the Browser entry, behaviour,
the footer-URL deviation, source preference, and the session-only recents (per constitution).
