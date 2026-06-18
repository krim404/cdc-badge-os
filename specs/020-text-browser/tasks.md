---

description: "Task list for Text Browser (mod_browser)"
---

# Tasks: Text Browser

**Input**: Design documents from `/specs/020-text-browser/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Tests**: Host unit tests ARE included for the pure-logic engine (tokenizer, extractor, feed
parser, source selector) - the highest-risk part of this feature - per plan.md "Testing" and
the existing `test/host/` pattern (`test_markdown`). View/fetcher layers are hardware-verified
via quickstart.md, not host-tested.

**Organization**: Tasks grouped by user story for independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: US1/US2/US3 maps to spec.md user stories
- Exact file paths included

## Path Conventions

Firmware module under `components/mod_browser/`; host tests under `test/host/test_html_extract/`;
docs under `website/src/content/docs/`. Pure-logic units have no ESP-IDF/GFX deps so they build
in the native host-test env. Host-test files MUST use project-relative `../` includes of the
module sources (never absolute paths) so CI `pio test -e native` passes.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Module skeleton, build wiring, i18n, test scaffold.

- [X] T001 Create module skeleton and build wiring: `components/mod_browser/CMakeLists.txt` (REQUIRES `cdc_core cdc_ui cdc_views cdc_hal cdc_log esp_http_client esp-tls mbedtls`), `components/mod_browser/include/mod_browser/BrowserModule.h` and `components/mod_browser/src/BrowserModule.cpp` (minimal `BrowserModule : core::ModuleBase("mod_browser")` + `extern "C" void mod_browser_register()`), and add `mod_browser` to the `main/CMakeLists.txt` MODULES list. Verify `~/.platformio/penv/bin/pio run` builds and the module registers.
- [X] T002 [P] Add module i18n: register the English `mod_browser.*` table in `components/mod_browser/src/BrowserModule.cpp` (via `I18n::registerEnglishTable`) and mirror every key in `assets/i18n/lang_de.json` (real UTF-8 umlauts).
- [X] T003 [P] Create host-test suite `test/host/test_html_extract/test_html_extract.cpp` (Unity, native env) following `test/host/test_markdown/`; it `#include`s the module's pure-logic sources via project-relative paths (no per-test CMake needed in this project).

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: The dependency-free engine + in-RAM data structures + streaming fetcher that ALL
user stories build on.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T004 [P] Define in-RAM data structures and static config in `components/mod_browser/src/BrowserTypes.h` per data-model.md: `PageDocument`, `LinkRef`, `HeadSignals`, `RecentUrl`, and `constexpr` caps (`kMaxScanBytes=512*1024`, `kMaxLinks=128`, `kMaxRecents=16`, `kMaxHistory=8`); buffers via `cdc::core::PsramUniquePtr`.
- [X] T005 [P] Implement `HtmlTokenizer` in `components/mod_browser/src/HtmlTokenizer.{h,cpp}` per `contracts/html-tokenizer.md`: streaming FSM, bounded open-tag stack (depth <=32), RAWTEXT/RCDATA for `<script>/<style>`, lenient/non-crashing, chunk-boundary-agnostic, `TokenSink` callbacks + pull-style `attr()`, `inside()`/`depth()`.
- [X] T006 Implement `ContentExtractor` in `components/mod_browser/src/ContentExtractor.{h,cpp}` per `contracts/content-extractor.md` (depends on T005): head-signal capture (title/og/description/canonical/amphtml/feed/JSON-LD), jusText-lite block classification (text/link/stopword density, `<article>/<main>` preference, suppress nav/script/etc.), Markdown-ish emit with inline `[n]` markers + parallel link table, OG-card and empty fallbacks, `kStopwords` list.
- [X] T007 [P] Implement `SourceSelector` in `components/mod_browser/src/SourceSelector.{h,cpp}` per `contracts/feed-and-fetch.md` (depends on T004): pure `selectSource(requestUrl, HeadSignals)` ordering host-map text-mirror -> feed -> amp -> MainExtract (apply canonical), incl. built-in `kTextMirrorMap`.
- [X] T008 [P] Implement `FeedParser` in `components/mod_browser/src/FeedParser.{h,cpp}` per `contracts/feed-and-fetch.md`: streaming RSS/Atom item scanner (CDATA, predefined+numeric entities, RSS link-text vs Atom link-attr, full-text vs summary), item bodies re-run through `render::decodeWebText`.
- [X] T009 Implement `PageFetcher` in `components/mod_browser/src/PageFetcher.{h,cpp}` per `contracts/feed-and-fetch.md` (depends on T005 `TokenSink`): `esp_http_client` streaming read loop (`_open`/`_fetch_headers`/`_read` 4 KB -> sink, `_cleanup`), reuse `crt_bundle_attach`/redirect/`buffer_size=4096` from `host_api_http.cpp`, expose status/Content-Type/final URL, 512 KB cap, `*cancel` abort. Never buffers the whole response.
- [X] T010 [P] Host tests for `HtmlTokenizer` in `test/host/test_html_extract/test_tokenizer.cpp`: split-chunk invariance, `<script>/<style>` suppression, malformed/mis-nested no-crash, `inside()`/`attr()`.
- [X] T011 [P] Host tests for `ContentExtractor` in `test/host/test_html_extract/test_extractor.cpp`: article-fixture keeps prose + drops chrome, nav-heavy page dropped, `<article>` preference, head capture, OG-card/empty fallbacks, caps set `truncated`.
- [X] T012 [P] Host tests for `FeedParser` in `test/host/test_html_extract/test_feed.cpp`: RSS full-text, Atom, summary-only (`fullText=false`), CDATA/entity bodies, malformed feed no-crash.
- [X] T013 [P] Host tests for `SourceSelector` in `test/host/test_html_extract/test_selector.cpp`: each branch fires for the right inputs, canonical applied, no-variant -> MainExtract.

**Checkpoint**: `~/.platformio/penv/bin/pio test -e native -f test_html_extract` green; engine ready.

---

## Phase 3: User Story 1 - Read the main content of a page (Priority: P1) 🎯 MVP

**Goal**: From Tools -> Browser, enter a URL, fetch over HTTPS, pick the best source, extract the
main readable content, and render it as scrollable styled text with the title and the URL in the
footer; handle offline/error/empty gracefully.

**Independent Test**: Connect WiFi, open Tools -> Browser, enter a Wikipedia article URL; the
article body is shown wrapped and scrollable with chrome suppressed, URL in footer.

- [X] T014 [US1] Implement load orchestration in `components/mod_browser/src/BrowserController.{h,cpp}`: run `SourceSelector` -> `PageFetcher` (+ `FeedParser` for the feed path, AMP/mirror/canonical paths) -> `ContentExtractor` -> build a `PageDocument`; apply OG/empty fallbacks (depends on T006/T007/T008/T009).
- [X] T015 [US1] Implement `BrowserView` entry screen in `components/mod_browser/src/BrowserView.{h,cpp}`: URL entry via `T9InputView`, load action calling the controller, and the offline guard (`hal::getWifiControllerInstance()->isConnected()` -> actionable WiFi message, no `cdc_os_ui` internals).
- [X] T016 [US1] Implement `BrowserPageView : public MarkdownView` in `components/mod_browser/src/BrowserPageView.{h,cpp}`: feed `PageDocument.source` to `MarkdownView::init(title, src, len)`, `setFooterHint(url)` + `getFooterHint()` so the footer shows the URL (FR-021), `N` = back/exit.
- [X] T017 [US1] Wire the Tools entry in `components/mod_browser/src/BrowserModule.cpp`: `getMenuItems()` returns a `ModuleMenuItem{label=tr("mod_browser.title"), priority, getView=()->BrowserView, MenuLocation::TOOLS_MENU}` that pushes `BrowserView` via `ViewStack`.
- [X] T018 [US1] Implement result/error UX in `BrowserController`/`BrowserPageView`: non-HTML content-type notice, `text/plain` rendered as scrollable text (FR-020), empty extraction -> "requires JavaScript / no readable content" + raw-view option (FR-007), and readable messages for offline/DNS/timeout/TLS/HTTP-status (FR-013); back press cancels an in-flight fetch (FR-014).

**Checkpoint**: US1 fully functional and independently testable (MVP).

---

## Phase 4: User Story 2 - Follow links between pages (Priority: P2)

**Goal**: In-page links appear as a numbered, scrollable list; selecting one loads the target;
back returns to the previous page.

**Independent Test**: On a page with several links, open the links list, select one, confirm it
loads, press back, confirm the previous page returns.

- [X] T019 [US2] Implement the links list in `components/mod_browser/src/BrowserPageView.cpp`: key `3` opens a `ListView` populated from `PageDocument.links` (number + label); back returns to the page.
- [X] T020 [US2] Implement link navigation in `components/mod_browser/src/BrowserController.cpp`: on link select, resolve the `LinkRef.href` (already absolute) and load it via the existing load flow, pushing the current page onto history.
- [X] T021 [US2] Implement back-history in `components/mod_browser/src/BrowserSession.{h,cpp}`: push current `PageDocument` (+ scroll pos) on load, `N` pops to the previous page (best-effort scroll restore), exit Browser at the root; bounded depth `kMaxHistory` (FR-011).

**Checkpoint**: US1 + US2 both work independently.

---

## Phase 5: User Story 3 - Reuse recent / saved URLs (Priority: P3)

**Goal**: A short session-only recents list on the entry screen lets the user reopen URLs
without retyping.

**Independent Test**: Visit a URL, leave and re-enter Browser, confirm it is in recents and
reopens in one selection; after reboot, recents are gone.

- [X] T022 [US3] Implement the recents ring in `components/mod_browser/src/BrowserSession.{h,cpp}`: add on each visit, cap `kMaxRecents` (oldest dropped), RAM-only, cleared on exit/reboot/lock (FR-019).
- [X] T023 [US3] Show recents on the entry screen in `components/mod_browser/src/BrowserView.cpp`: a `ListView` of recent entries (label + URL) above/below the "enter URL" action; selecting one loads it.

**Checkpoint**: all three stories independently functional.

---

## Phase 6: Polish & Cross-Cutting Concerns

- [X] T024 [P] Documentation: add the **Browser** Tools entry to the matching page under `website/src/content/docs/` (URL entry + recents, source preference text-mirror/feed/AMP/main/OG, links list, footer-shows-URL deviation, JS-SPA limitation, session-only recents). Current-state only.
- [ ] T025 Memory-safety validation: run quickstart.md step 6 (huge >1 MB page) and confirm bounded internal-SRAM heap (no growth with page size), 512 KB scan cap + truncation indicator, no panic (`tools/coredump.py`).
- [ ] T026 Run full quickstart.md on-device validation (SC-001..SC-008): article read, source preference, link follow, recents, failure modes, JS-SPA notice.
- [X] T027 Security/robustness review of `HtmlTokenizer`/`FeedParser` for bounds safety (no OOB on truncated/adversarial input), per Constitution Principle III, against `website/src/content/docs/security/`.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: start immediately.
- **Foundational (Phase 2)**: after Setup. BLOCKS all user stories. Within it: T005 before T006;
  T009 needs T005's `TokenSink`; tests T010-T013 follow their units (may be written first against
  fixtures for the extractor).
- **US1 (Phase 3)**: after Foundational. The MVP.
- **US2 (Phase 4)**: after US1 (reuses the load flow and PageDocument link table).
- **US3 (Phase 5)**: after US1 (extends the entry screen and session).
- **Polish (Phase 6)**: after the desired stories.

### Within Each User Story

- US1: T014 (controller) and T015/T016 (views) can progress together; T017 wires the entry; T018
  layers UX on top. US2: T021 (history) underpins T020; T019 is the list UI. US3: T022 before T023.

### Parallel Opportunities

- Setup: T002, T003 in parallel.
- Foundational: T004, T005, T007, T008 in parallel (different files); T010-T013 in parallel once
  their units exist.
- Polish: T024 [P] independent of device tasks.
- Stories can be staffed in parallel after Foundational, given independence.

---

## Parallel Example: Foundational engine

```bash
# Build the dependency-free units in parallel (different files):
Task: "Implement HtmlTokenizer in components/mod_browser/src/HtmlTokenizer.{h,cpp}"
Task: "Implement SourceSelector in components/mod_browser/src/SourceSelector.{h,cpp}"
Task: "Implement FeedParser in components/mod_browser/src/FeedParser.{h,cpp}"
Task: "Define data structures in components/mod_browser/src/BrowserTypes.h"

# Then their host tests in parallel:
Task: "Host tests for HtmlTokenizer in test/host/test_html_extract/test_tokenizer.cpp"
Task: "Host tests for FeedParser in test/host/test_html_extract/test_feed.cpp"
Task: "Host tests for SourceSelector in test/host/test_html_extract/test_selector.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 only)

1. Phase 1 Setup -> 2. Phase 2 Foundational (engine green on host) -> 3. Phase 3 US1 ->
4. STOP and validate: open Tools -> Browser, read an article -> 5. flash/demo.

### Incremental Delivery

Setup + Foundational -> US1 (MVP, read content) -> US2 (follow links) -> US3 (recents) -> Polish.
Each story adds value without breaking the previous.

---

## Notes

- [P] = different files, no incomplete-task dependency. [Story] maps tasks to spec user stories.
- Host tests live in one `test_html_extract` suite (separate file per unit for parallelism) and
  use project-relative includes of `components/mod_browser/src/*` (CI `pio test -e native`).
- No version bumps, no `host_api.h` change, no persistence/migration (Constitution V).
- Commit after each task or logical group; validate at each checkpoint.
