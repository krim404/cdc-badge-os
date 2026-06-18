# Phase 0 Research: Text Browser

Feature: 020-text-browser. Device: ESP32-S3, ESP-IDF/FreeRTOS, C++17. Internal SRAM is the
bottleneck; PSRAM is the default pool. No JS engine, no CSS, no DOM.

Each decision below resolved the open technical questions for the plan.

## D1. HTML parsing strategy

**Decision**: A hand-rolled **streaming tag tokenizer** (finite state machine over the HTML
tokenization states + a small bounded open-tag stack, depth <= 32). No DOM tree is ever
materialized. The tokenizer drives both head-metadata capture and body extraction in a
single pass over the streamed bytes.

**Rationale**: Constant, predictable RAM independent of page size (the hard requirement from
FR-005). The open-tag stack answers every structural question the heuristics need ("inside
`<a>`?", "inside `<nav>/<header>/<footer>/<aside>`?", "inside `<article>/<main>`?") in O(1)
without a tree. RAWTEXT/RCDATA states give "drop `<script>`/`<style>` content" for free. State
+ stack + per-block counters fit in a few KB of internal SRAM.

**Alternatives considered**:
- **lexbor**, **google/gumbo-parser**: rejected. Both build a fully-formed DOM tree (one node
  object per element); peak RAM scales with document size. Designed to hold the parsed
  document, not to stream under fixed RAM. Disqualified on internal-SRAM grounds.
- **htmlstreamparser**: right shape (char-driven, caller-provided fixed buffers, no DOM) but
  dormant/unmaintained with unclear licensing. Used as an **API design reference**, not a
  vendored dependency.

## D2. Content-extraction heuristic

**Decision**: A streaming **jusText-lite** classifier. Per text block, accumulate total text
chars, chars inside `<a>` (link chars), and a stopword hit count against a small ASCII/CP437
stoplist. Classify with jusText-style thresholds (link-density gate ~0.2; length thresholds)
and a single-block look-behind to reclassify short/near-good blocks. Suppress text inside
`script/style/noscript/svg/nav/header/footer/aside/form/select`. **Boost/prefer** blocks whose
tag stack contains `<article>` or `<main>`; when such content exists, ignore the rest.

**Rationale**: The discriminating signals (text density, link density, stopword density) are
all computable per block in one pass; only the published systems' ML model and whole-document
view are expensive, and those are dropped. jusText is purely heuristic and already per-block,
so it ports cleanly to streaming.

**Alternatives considered**:
- **Mozilla Readability**: rejected. Requires a DOM (scores elements, propagates scores to
  ancestors, merges article-like siblings) - fundamentally tree operations.
- **Boilerpipe**: keep its density *features*, drop its trained decision tree.
- **CETR** (text-to-tag ratio + histogram clustering): rejected; needs the whole histogram
  resident and is heavier to implement for no quality gain here.

## D3. Source selection / "lighter variant" preference

**Decision**: After scanning the streamed `<head>`, choose the content source in this order:

1. **Static text-mirror host map** (small built-in table, e.g. `cnn.com -> lite.cnn.com`,
   `npr.org -> text.npr.org`): if the host matches, rewrite and fetch the mirror. Decided from
   the URL host *before* fetching. Low-priority booster (few known hosts).
2. **RSS/Atom feed** (autodiscovered via `<link rel="alternate" type="application/rss+xml|atom+xml">`):
   the highest general-availability clean source. If the feed item carries full text
   (`content:encoded` / Atom `<content>`), render it and stop; if summary-only, keep the
   summary as a fallback and fall through to step 4.
3. **AMP variant** (`<link rel="amphtml">`): clean, JS-free, allowlisted markup, easy to strip.
   Taken only when actually advertised.
4. **Main-page extraction** (D2), after following `<link rel="canonical">` to normalize the
   URL. The unavoidable workhorse that fires on the majority of pages.
5. **Open Graph card fallback** (`og:title` + `og:description`): when extraction yields too
   little (paywall / JS-rendered SPA / near-empty body), render a readable summary card.

To avoid a re-fetch in the common case, the tokenizer captures head signals during the same
stream; if no preferred variant is advertised, body extraction continues on the same stream.
A preferred variant triggers one additional fetch of that variant.

**Rationale**: Maximizes "article body with zero chrome" while minimizing bytes and parsing.
Honest availability (typical content site): feed autodiscovery is common (full-text maybe half
of those); AMP is sub-1% and shrinking; OG card covers ~two-thirds as a safety net; text
mirrors only for a handful of known hosts. So **feed + main-extraction are the load-bearing
paths**; host-map, AMP, and OG card are cheap opportunistic boosters. AMP must not drive the
architecture.

**Alternatives considered**: relying on AMP as primary (rejected: too rare/declining);
relying on JSON-LD `articleBody` (rejected: present on a minority of pages); Gemini protocol
(out of scope - different protocol, not advertised from HTTP pages).

## D4. Structured-metadata capture

**Decision**: During the head scan, capture `<title>`, `og:title`/`og:description`,
`<meta name="description">`, `<link rel="canonical">`, the feed/amphtml links, and
opportunistically JSON-LD `"articleBody"` (cap the captured `<script type="application/ld+json">`
block, e.g. 16-32 KB in PSRAM, and do a string-level scrape - no full JSON parser). Title + OG
description form the always-available header/summary.

**Rationale**: All are grabbable by the same tokenizer scanning attributes; near-zero RAM.
`<title>` is near-universal, OG ~66% prevalence. JSON-LD `articleBody`, when present, is clean
gold and lets body heuristics be skipped, but it is usually absent so it stays opportunistic.

## D5. RSS/Atom parsing

**Decision**: A hand-rolled **streaming XML tag scanner** (no XML library). Track the current
element; capture text inside `item`/`entry` -> `title`, `link`, `description`,
`content:encoded`/`<content>`/`<summary>`. Handle CDATA sections, the 5 predefined XML
entities + numeric entities, and Atom's `link` as an attribute (`<link href>`) vs RSS's `link`
as element text. Feed item bodies (which contain bounded, chrome-free HTML) back through the
same HTML text pass.

**Rationale**: RSS/Atom is shallow and regular; a ~200-line streaming scanner in PSRAM-backed
buffers is leaner and dependency-free, matching the codebase style. expat/saxml are available
but unnecessary.

## D6. Networking (streaming fetch)

**Decision**: The native module uses `esp_http_client` directly in a **streaming read loop**
(`esp_http_client_open` -> `esp_http_client_fetch_headers` -> `esp_http_client_read` loop ->
`esp_http_client_cleanup`), feeding each ~4 KB chunk straight into the tokenizer and discarding
consumed bytes. Reuse the proven config from `host_api_http.cpp`: `crt_bundle_attach =
esp_crt_bundle_attach` (full CA bundle, TLS validated), auto-redirect on, `buffer_size = 4096`.
Read `Content-Type` and `Location`/`canonical` via `esp_http_client_get_header`. Hard cap the
scanned bytes at 512 KB.

**Rationale**: The existing plugin `host_http_*` path **accumulates the entire response** into
a PSRAM buffer (up to 1 MB) via its event handler, which violates FR-005's "never buffer the
whole response / bounded RAM". A native streaming read loop keeps working memory bounded and
independent of page size. TLS trust is inherited unchanged (no new trust decisions).

**Alternatives considered**: reusing `host_http_read_chunk` (rejected: it reads from an
already-fully-buffered body, defeating the memory goal).

## D7. Reuse map (firmware building blocks)

| Need | Reuse | Location |
|------|-------|----------|
| Body rendering (scroll/wrap/fonts) | `MarkdownView::init(title, src, len)` (MAX_SOURCE 64 KB) | `cdc_views/.../MarkdownView.h` |
| Entity decode + UTF-8->CP437 (one call) | `render::decodeWebText(in, out, size, DisplayTarget::Cp437)` (55+ entities, numeric) | `cdc_views/.../RenderHelpers.h:80`, `Cpp:395` |
| CP437 helpers | `cdc::core::cp437::fromUtf8` | `cdc_core/.../Cp437.h:24` |
| Links list / recents list | `ListView::init/setOnSelect/insertItem/removeItem` (key 3 = menu) | `cdc_views/.../ListView.h` |
| URL entry | `T9InputView::init(title, initial, maxLen)` + `setOnSave/setOnCancel` | `cdc_views/.../T9InputView.h` |
| Footer = URL | override `getFooterHint()` / `setFooterHint(url)`; `render::drawFooterBar(gfx,w,h,prefix,hint,true)` | `IView.h:117/191`, `RenderHelpers.cpp:118` |
| Tools entry | module `getMenuItems()` -> `ModuleMenuItem{label, prio, getView, ..., MenuLocation::TOOLS_MENU}` | `cdc_core/.../IModule.h:29`, `AppUi.cpp:496/574` |
| WiFi state | `hal::getWifiControllerInstance()` -> `isConnected()/connect()/getWifiState()` | `cdc_hal/.../IWifiController.h:54` |
| Module skeleton | `ModuleBase`, `mod_<name>_register()`, `main/CMakeLists.txt` MODULES list | template: `components/mod_sao/` |
| Bounded buffers | `cdc::core::psramAlloc<T>(n)` / `PsramUniquePtr<T>` | `cdc_core/.../Raii.h:51` |
| View lifecycle / nav | `ViewStack::push/pop`, `ViewBase` (`onEnter/onExit/onResume/render/onKey`) | `cdc_ui/.../ViewStack.h`, `IView.h` |

**Body rendering note**: `MarkdownView` renders styled text only and has **no link support**.
The Browser therefore builds a Markdown-ish source string with inline `[n]` markers and a
**parallel link table** (`[n] -> absolute URL`); a `BrowserPageView : public MarkdownView`
overrides `onKey` to open the links `ListView` and handle history-back, and `setFooterHint(url)`
to show the URL in the footer.

**Offline path note**: to respect module isolation (core/modules must not depend on
`cdc_os_ui` internals like `showWifiMainMenu()`), the offline state shows an actionable message
directing the user to WiFi settings; it does not call OS-UI internals directly.

## D8. Honest limitations (documented, not bugs)

- **JS-rendered SPAs**: the fetched HTML is a near-empty shell; nothing to extract. Detect
  "tiny/zero extracted content" and surface a "page requires JavaScript / no readable content"
  message + raw-view option, rather than emitting garbage.
- **URL entry on a 12-key keypad** is inherently slow (multi-tap, punctuation); the session
  recents list is the primary mitigation.
- **Heuristic misses**: jusText-lite will occasionally keep a boilerplate block or drop a short
  paragraph; the `<article>/<main>` preference and link-density gate keep this acceptable for
  the target article/blog/news pages.

## Sources

- jusText algorithm + defaults: https://github.com/miso-belica/jusText/blob/main/doc/algorithm.rst
- Boilerpipe shallow features: https://downloads.webis.de/publications/papers/bevendorff_2023c.pdf
- Mozilla Readability (needs DOM): https://github.com/mozilla/readability
- WHATWG tokenization state machine: https://html.spec.whatwg.org/multipage/parsing.html
- lexbor (fully-formed DOM): https://lexbor.com/articles/part-1-html/
- gumbo-parser (full parse tree): https://github.com/google/gumbo-parser
- htmlstreamparser (design reference): https://github.com/arjunchitturi/htmlstreamparser
- AMP spec (restricted markup, no author JS): https://amp.dev/documentation/guides-and-tutorials/learn/spec/amphtml
- AMP prevalence ~0.37%, declining: https://almanac.httparchive.org/en/2024/seo
- RSS autodiscovery: https://www.rssboard.org/rss-autodiscovery
- RSS full-text vs summary (content:encoded): https://www.rssboard.org/rss-profile
- Open Graph ~66% prevalence: https://ahrefs.com/blog/open-graph-meta-tags/
- JSON-LD usage / articleBody often omitted: https://w3techs.com/technologies/details/da-jsonld
- ESP32 prior art (litehtml, WIP renderer): https://github.com/leopck/microbrowser
- Text-only mirrors (no generic discovery): https://github.com/localjo/awesome-text-only-news
