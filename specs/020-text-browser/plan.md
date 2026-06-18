# Implementation Plan: Text Browser

**Branch**: `020-text-browser` | **Date**: 2026-06-18 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/020-text-browser/spec.md`

## Summary

Add a self-contained native module `mod_browser` that surfaces a **Browser** entry under the
Tools menu. Given a URL (typed via T9 or picked from a session recents list), it fetches the
page over HTTPS using `esp_http_client` in a **streaming read loop** (never buffering the whole
response), runs a **single-pass hand-rolled HTML tokenizer** that both captures head metadata
and extracts the main readable content with a **jusText-lite** density heuristic, and renders
the result through the existing `MarkdownView`. It prefers an advertised lighter source (text
mirror / RSS-Atom feed / AMP) over the bloated main page, collects in-page links into a numbered
`ListView`, keeps a session back-history, and repurposes the footer to show the current URL.
Recents and history live only in PSRAM RAM (never persisted). No host API or version changes.

See [research.md](./research.md) for the parser/heuristic/source-selection decisions and the
reuse map, and [data-model.md](./data-model.md) for the in-RAM structures.

## Technical Context

**Language/Version**: C++17 (ESP-IDF component, native firmware module)

**Primary Dependencies**: `esp_http_client` + `esp_crt_bundle` (streaming HTTPS), `cdc_hal`
WifiController, `cdc_views` (`MarkdownView`, `ListView`, `T9InputView`, `RenderHelpers::decodeWebText`),
`cdc_core` (`psramAlloc`/`PsramUniquePtr`, `cp437`), `cdc_ui` (`ViewStack`, `IView`/`ViewBase`),
`cdc_log`. No new third-party library (HTML tokenizer, jusText-lite extractor, and RSS/Atom
scanner are hand-rolled inside the module).

**Storage**: None persisted. Recents and back-history are session-only, held in PSRAM and
cleared on reboot / device lock. No NVS, no FAT, no flash writes.

**Testing**: Host unit tests (`test/host/`, native env) for the pure-logic core - tokenizer,
jusText-lite extractor, head-signal capture, and RSS/Atom scanner - mirroring `test_markdown/`.
Hardware verification via the on-device flow (Tools -> Browser).

**Target Platform**: ESP32-S3 (CDC Badge v1.0/v1.1), E-Paper 296x128, 12-key keypad.

**Project Type**: Firmware feature module (`components/mod_browser/`).

**Performance Goals**: First readable content within ~8 s on typical WiFi for a typical article
(SC-002). Streaming throughput bounded by network, not parsing.

**Constraints**: Internal SRAM is the bottleneck - tokenizer state + open-tag stack (depth <=32)
+ per-block counters stay in internal RAM (a few KB); the scanned window, stoplist, JSON-LD
capture, link table, and the MarkdownView source (64 KB) live in PSRAM. Raw response scanned is
**capped at 512 KB** (FR-005); extracted source capped at MarkdownView's 64 KB. Working memory
is independent of page size (never buffer the whole response).

**Scale/Scope**: One foreground module + one page view + supporting parser/extractor/fetcher
units; link count capped (e.g. 128); recents capped (e.g. 16); back-history depth bounded.

## Constitution Check

*GATE: re-checked after Phase 1 design. All principles pass; no Complexity Tracking needed.*

- **I. Module Isolation & Self-Containment**: PASS. Everything lives in `components/mod_browser/`
  (module class, page/links/recents views, tokenizer, extractor, feed scanner, fetcher,
  module-local i18n table). Added with one `main/CMakeLists.txt` MODULES entry +
  `mod_browser_register()`. Reuses only shared `cdc_*` building blocks; core gains no reference
  to the module. The offline path shows a message and does **not** call `cdc_os_ui` internals.
- **II. Memory Discipline: PSRAM-First**: PASS. Streaming fetch + tokenizer never holds the full
  page; all growing buffers via `psramAlloc`/`PsramUniquePtr`; only the small fixed tokenizer
  state/stack/counters sit in internal RAM (< 256 bytes of static state per the rule).
- **III. Security & Sandbox Integrity**: PASS. No TROPIC01 access, no secrets, no plugin/sandbox
  or `host_*` change. TLS server-cert validation is inherited unchanged (full CA bundle). The new
  attack surface is parsing untrusted HTML/XML - mitigated by a strictly bounded, lenient,
  non-crashing parser (FR-017) with no code execution and hard size caps. Recents/history are
  RAM-only (no browsing trace persisted).
- **IV. Simplicity & Surgical Change**: PASS. Reuses existing views/helpers; new code is limited
  to the fetcher, the streaming tokenizer, the jusText-lite extractor, the feed scanner, and the
  module/view glue. Source-selection boosters (host-map, AMP) are small and additive; the two
  load-bearing paths (feed + main-extraction) plus the OG fallback are the core.
- **V. Versioning Discipline & Pre-1.0 Data Freedom**: PASS. No version bumps. No `host_api.h`
  change, so no plugin-repo sync. No persisted format, so no migration concern.

## Project Structure

### Documentation (this feature)

```text
specs/020-text-browser/
├── plan.md              # This file
├── research.md          # Phase 0 decisions (parser, heuristic, sources, reuse map)
├── data-model.md        # Phase 1: in-RAM entities
├── quickstart.md        # Phase 1: end-to-end validation guide
├── contracts/           # Phase 1: internal unit contracts (tokenizer / extractor / feed / fetcher)
│   ├── html-tokenizer.md
│   ├── content-extractor.md
│   └── feed-and-fetch.md
└── tasks.md             # Phase 2 (/speckit-tasks - NOT created here)
```

### Source Code (repository root)

```text
components/mod_browser/
├── CMakeLists.txt                       # REQUIRES: cdc_core cdc_ui cdc_views cdc_hal cdc_log
│                                        #           esp_http_client esp-tls mbedtls
├── include/mod_browser/
│   └── BrowserModule.h                  # ModuleBase subclass; getMenuItems() -> TOOLS_MENU
└── src/
    ├── BrowserModule.cpp                # registration, i18n table, Tools entry factory
    ├── BrowserSession.{h,cpp}           # current page + back-history + recents (PSRAM, RAM-only)
    ├── BrowserView.{h,cpp}              # entry screen: URL input (T9) + recents (ListView)
    ├── BrowserPageView.{h,cpp}          # : public MarkdownView; footer=URL; '3'=links, 'N'=back
    ├── HtmlTokenizer.{h,cpp}            # streaming SAX-style tokenizer + bounded tag stack
    ├── ContentExtractor.{h,cpp}         # jusText-lite body extraction + head-signal capture
    ├── FeedParser.{h,cpp}               # streaming RSS/Atom item scanner
    ├── PageFetcher.{h,cpp}              # esp_http_client streaming read loop -> tokenizer sink
    └── SourceSelector.{h,cpp}           # host-map / feed / amp / canonical / OG decision

test/host/test_html_extract/            # host unit tests (native env), mirrors test_markdown/
├── CMakeLists.txt
└── test_html_extract.cpp               # tokenizer + extractor + feed-parser fixtures

website/src/content/docs/               # docs updated in the same change (see below)
```

**Structure Decision**: Single self-contained module under `components/mod_browser/`, matching
the established module pattern (template: `components/mod_sao/`). Pure-logic units
(`HtmlTokenizer`, `ContentExtractor`, `FeedParser`, `SourceSelector`) are free of ESP-IDF/GFX
dependencies so they compile and run under the native host-test environment; the views and
fetcher are the only hardware-coupled parts.

## Source-selection pipeline (the core algorithm)

1. **Pre-fetch**: if the URL host matches the built-in text-mirror map, rewrite to the mirror.
2. **Open + stream head**: `PageFetcher` opens the URL (TLS-validated) and streams bytes into
   `HtmlTokenizer`; `ContentExtractor` captures head signals: `<title>`, `og:title/description`,
   `meta description`, `rel=canonical`, `rel=amphtml`, feed `rel=alternate` links.
3. **Decide source** at end of `<head>`:
   - feed advertised -> fetch feed, `FeedParser` extracts the item; full-text item -> render & stop;
     summary-only -> keep summary, continue to main extraction;
   - else amphtml advertised -> fetch the AMP page and extract from it;
   - else -> **continue the same stream** into body extraction (no re-fetch).
4. **Body extraction** (jusText-lite): emit kept blocks as a Markdown-ish source with inline
   `[n]` markers, building the parallel link table; stop at the 64 KB source cap or 512 KB scan
   cap (set truncated flag).
5. **Fallbacks**: extracted body too small -> OG card (title + description); still empty ->
   "requires JavaScript / no readable content" notice with raw-view option.
6. **Render**: `BrowserPageView` (a `MarkdownView`) shows the source; footer = current URL;
   key `3` opens the links `ListView`; selecting a link resolves it (relative vs absolute, vs
   canonical) and loads it (push history); `N` pops history or exits at the root.

## Documentation updates (same change, per constitution)

- New Tools entry: add **Browser** to the relevant Tools/guide page under
  `website/src/content/docs/` (search for the page documenting the Tools menu / Bluetooth beacon
  entry and add the Browser entry beside it).
- Describe behaviour: URL entry + recents, source preference (text-mirror/feed/AMP/main/OG),
  links list, footer-shows-URL deviation, JS-SPA limitation, and that recents/history are
  session-only. No serial command is added (nothing for `dev/proto/serial-commands.md`).

## Complexity Tracking

No constitution violations. Section intentionally empty.
