# Feature Specification: Text Browser

**Feature Branch**: `020-text-browser`

**Created**: 2026-06-18

**Status**: Draft

**Input**: User description: "ein ganz einfacher Text-Stripper-Browser: URLs öffnen (auch HTTPS, bereits implementiert) und sinnvolle Daten anzeigen. Kritisch: die meisten Websites sind voller Schrott, daher Content-Extraction-Algorithmen prüfen ('echte Daten einer Website identifizieren') und ggf. alternative Render-/Datenquellen (AMP?) nutzen. Das Ganze landet unter Tools als 'Browser'."

## Overview

A minimal, read-only text browser reachable from the **Tools** menu as **Browser**. It
fetches a web page over HTTP/HTTPS, discards the page's chrome (navigation, ads,
sidebars, scripts, styling) and presents the **main readable content** as scrollable
styled text on the E-Paper display, with in-page links collected into a numbered,
selectable list so the user can navigate from page to page.

The defining challenge is not fetching (already solved by the existing WiFi + HTTPS
client) but **separating real content from boilerplate** on a device with no DOM, no
JavaScript engine, no CSS layout, and a tight RAM budget. The feature addresses this
with a tiered extraction strategy that prefers cheap, reliable structured signals and
falls back to streaming density heuristics.

## Clarifications

### Session 2026-06-18

- Q: How should recents/history be handled on this security device? → A: Session-only in RAM, cleared on reboot/lock, never persisted to flash.
- Q: Maximum streamed response size scanned for content before truncation? → A: 512 KB (extracted text remains separately capped at 64 KB).
- Q: How does the user select an in-page link on the 12-key keypad? → A: A scrollable links-list view (scroll + select), consistent with existing OS lists.
- User directive: When a page advertises a feed (RSS/Atom) or AMP variant, that lighter/cleaner source is **preferred** over the full main page; and working memory MUST stay bounded regardless of how large the raw HTML is.
- User directive: In the Browser, the footer shows the current URL instead of the usual key hints (a deliberate deviation from the standard UI flow).
- User directive (supersedes the session-only recents above): saved pages are explicit **bookmarks persisted in NVS** (not auto-collected session recents); the home screen lists bookmarks plus an "Open webpage" action, and the page context menu (key 3) offers Open webpage / Links / Bookmark / Pages / Close. Navigation: Y = select, 2/8 = scroll, N = back, long-N = exit, 3 = context menu; deleting a bookmark is the home list's key-3 menu.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Read the main content of a page (Priority: P1)

A user opens **Tools → Browser**, enters a URL (or picks a recent one), and the badge
fetches the page, strips the clutter, and shows the article's title and main text as
clean, vertically scrollable text. Encyclopedia articles, news articles, blog posts,
documentation and plain-HTML pages are the target.

**Why this priority**: This is the entire point of the feature and a complete,
demonstrable MVP on its own. Without readable extraction the browser is useless; with
it, a user can already consume most text-centric pages.

**Independent Test**: Enter a Wikipedia article URL on a connected badge and confirm
the article body is shown without the navigation column, edit links, footer, or
"references" chrome dominating the view.

**Acceptance Scenarios**:

1. **Given** the badge is connected to WiFi, **When** the user enters a valid HTTPS
   article URL, **Then** the page title and its main body text are displayed as
   wrapped, scrollable text within the configured load timeout.
2. **Given** a page wrapped in heavy navigation, headers, footers and ad markup,
   **When** it is rendered, **Then** the main content is shown and the surrounding
   chrome is suppressed.
3. **Given** a page that exposes structured article metadata (title / description /
   article body), **When** it is loaded, **Then** that structured content is preferred
   over heuristic guessing.
4. **Given** a page whose main body cannot be confidently isolated, **When** it is
   loaded, **Then** the browser still shows at least the page title and summary
   (from page metadata) rather than a blank screen.

---

### User Story 2 - Follow links between pages (Priority: P2)

While reading, the user sees in-text links rendered as numbered references (lynx-style
`[1]`, `[2]`, …). A links list lets the user select a reference; the browser resolves
it to an absolute URL, loads it, and a back action returns to the previous page.

**Why this priority**: Turns a single-page reader into an actual browser. Valuable, but
US1 already delivers standalone value, so this is second.

**Independent Test**: Load a page with several outbound links, open the links list,
select one, confirm the target page loads, then press back and confirm the original
page is restored at its prior scroll position.

**Acceptance Scenarios**:

1. **Given** a rendered page containing hyperlinks, **When** the user opens the links
   view, **Then** each link is listed with a stable number and a readable label.
2. **Given** the links list, **When** the user selects a relative or absolute link,
   **Then** it is resolved against the current page URL and the target is fetched.
3. **Given** the user has navigated forward one or more pages, **When** the user
   presses back, **Then** the previous page is shown again; pressing back at the first
   page exits the Browser.

---

### User Story 3 - Reuse recent / saved URLs (Priority: P3)

The user does not want to retype long URLs on a keypad. The Browser keeps a short list
of recently visited (and optionally saved) URLs, selectable directly from the Browser
entry screen.

**Why this priority**: Pure convenience; the browser is fully functional without it.

**Independent Test**: Visit a URL, leave and re-enter the Browser, confirm the URL
appears in the recents list and can be reopened in one selection.

**Acceptance Scenarios**:

1. **Given** the user has visited at least one URL, **When** the Browser entry screen
   is shown, **Then** recent URLs are listed for one-tap reopening.
2. **Given** the recents list is at capacity, **When** a new URL is visited, **Then**
   the oldest entry is dropped (bounded list).

---

### Edge Cases

- **Offline**: WiFi is not connected → a clear message is shown with a pointer to the
  connectivity settings, not a silent failure or hang.
- **Network failure**: DNS resolution failure, connection refused, or response timeout
  → a readable error naming the failure category; the user can retry or go back.
- **TLS failure**: an HTTPS handshake/certificate failure is reported as such.
- **HTTP error status** (4xx/5xx): the status is surfaced; the body, if any, is still
  offered for rendering.
- **Redirects**: 3xx redirects (including http→https) are followed up to a bounded
  limit; an exceeded limit is reported.
- **Non-HTML content**: a content type the browser cannot render as an article (binary,
  PDF, archive, JSON, image) shows a content-type notice; `text/plain` is shown as-is.
- **JavaScript-rendered pages**: a page whose content is injected client-side yields
  little extractable body; the browser falls back to the metadata card and, if even
  that is empty, shows a "no readable content" notice with the option to view the raw
  stripped text.
- **Oversized / absurdly large page**: working memory stays bounded because the response
  is never fully buffered; the scanned content is capped at 512 KB with a truncation
  indicator. When the page advertises a feed/AMP variant, that smaller source is
  preferred, avoiding the oversized body entirely.
- **Malformed HTML**: unbalanced or unclosed tags must not crash or hang the parser;
  extraction degrades gracefully.
- **Character set**: page text is converted to the device codepage for display; glyphs
  outside it (non-Latin scripts) are substituted rather than corrupting the layout.
- **Excessive links**: the number of collected links is capped; the cap being reached is
  indicated.
- **Cancellation**: pressing back during a load aborts the in-flight fetch and returns
  to the previous screen.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST add a **Browser** entry to the Tools menu that opens the
  text browser.
- **FR-002**: Users MUST be able to enter an arbitrary URL via on-screen input and load
  it.
- **FR-003**: The system MUST support both `http` and `https` URLs.
- **FR-004**: The system MUST require an active network connection and, when absent,
  show an actionable message directing the user to connectivity settings.
- **FR-005**: The system MUST fetch and process the response as a bounded streaming
  pipeline that NEVER buffers the whole response: working memory MUST stay bounded and
  independent of page size (so even an absurdly large HTML document cannot exhaust RAM),
  the raw response scanned for content MUST be capped at 512 KB (bytes beyond are dropped
  with a truncation indicator), and a bounded time limit applies.
- **FR-006**: The system MUST extract the page's **main readable content** using a
  tiered strategy, applied in order until one yields usable content:
  0. **Advertised lightweight variant** — if the document head advertises a content feed
     (RSS/Atom) or an AMP variant, prefer fetching and rendering that smaller, cleaner
     source instead of the full main page (see FR-018).
  1. **Structured metadata** — page title, Open Graph (`og:*`) properties, the meta
     description, and structured article-body data when present.
  2. **Semantic containers** — content within main-article semantic regions when the
     markup provides them.
  3. **Boilerplate removal + density heuristic** — remove script, style, navigation,
     header, footer, aside and form regions, then keep blocks with a high text-to-link
     (and text-to-markup) ratio and discard high-link-density blocks (menus, nav).
- **FR-007**: The system MUST always render at least the page title and an available
  summary (from metadata) when full-body extraction does not produce usable content,
  rather than a blank result.
- **FR-008**: The system MUST render extracted content as wrapped, vertically
  scrollable styled text showing the page title, reusing the existing scrollable
  document viewer so reading never requires horizontal scrolling.
- **FR-009**: The system MUST collect in-page hyperlinks into a numbered, selectable
  list and render inline link markers (e.g. `[1]`) at the link positions in the text.
- **FR-010**: Users MUST be able to select a link from a scrollable links-list view
  (scroll + select, consistent with other OS lists) to navigate to it; the system MUST
  resolve relative links against the current page URL before fetching.
- **FR-011**: The system MUST maintain a session back-history; a back action returns to
  the previous page (restoring its scroll position where feasible), and back at the
  first page exits the Browser.
- **FR-012**: The system MUST follow HTTP redirects up to a bounded limit and report
  when the limit is exceeded.
- **FR-013**: The system MUST report every failure mode (offline, DNS/connection,
  timeout, TLS, HTTP error status, unsupported content type, empty extraction) as a
  readable on-screen message without hanging or crashing.
- **FR-014**: The system MUST allow an in-flight fetch to be cancelled via the back key.
- **FR-015**: The system MUST cap the number of collected links and indicate when the
  cap is reached.
- **FR-016**: The system MUST decode HTML entities and convert page text to the device
  display codepage, substituting unsupported glyphs without corrupting layout.
- **FR-017**: The system MUST tolerate malformed HTML (unbalanced/unclosed tags, missing
  end tags) without crashing, hanging, or unbounded memory growth.
- **FR-018**: When a page advertises a cleaner, lighter rendering source — an associated
  content feed (RSS/Atom via the head's alternate links) or an AMP variant (the head's
  `amphtml` link) — the system MUST detect it while streaming the document head and
  PREFER fetching and rendering that variant over downloading and parsing the full main
  page. The system MUST still function on pages that advertise no such variant.
- **FR-019**: The system SHOULD keep a bounded, session-only list of recent URLs (held in
  RAM, cleared on reboot or device lock, never written to persistent storage) for
  one-step reopening from the Browser entry screen.
- **FR-020**: For a content type the browser cannot render as an article, the system
  MUST show a content-type notice; `text/plain` MUST be rendered as scrollable text.
- **FR-021**: While a page is displayed, the system MUST show the current page URL in the
  footer area (replacing the usual key-hint footer), truncated as needed, as a deliberate
  deviation from the standard footer-hint convention.

### Key Entities *(include if feature involves data)*

- **Browser Session**: the live browsing state — the current page, the back-history
  stack of previously visited pages, and the per-page scroll position.
- **Page Document**: the result of loading one URL — source URL (post-redirect), title,
  the extracted readable content (styled lines), the link list, the extraction tier that
  produced the content, and a truncation flag.
- **Link Reference**: one collected hyperlink — its display number, a readable label,
  and its resolved absolute target URL.
- **Recent URL**: a bounded, session-only entry of a previously visited URL (label +
  URL) held in RAM for quick reopening; never persisted to flash, cleared on reboot or
  device lock.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On a fixed evaluation set of 20 text-centric pages (encyclopedia, news,
  blog, documentation), at least 80% show the main article text with site chrome
  (navigation, header, footer, ads) suppressed.
- **SC-002**: For a typical article page on a typical home connection, first readable
  content is visible within 8 seconds of confirming the URL.
- **SC-003**: 100% of rendered pages wrap text to the display so reading requires only
  vertical scrolling.
- **SC-004**: A user can follow a link from one page to another and return to the prior
  page with a single back action, without leaving the Browser.
- **SC-005**: The Browser renders pages of any size without running out of memory,
  including absurdly large pages, because the response is never fully buffered; verified
  with an input far exceeding the 512 KB scan cap.
- **SC-008**: When a page advertises a feed or AMP variant, the Browser loads that
  lighter variant rather than the full main page.
- **SC-006**: Every defined failure mode (offline, DNS/connection, timeout, TLS, HTTP
  error, unsupported type, empty content) produces a readable on-screen message rather
  than a hang or crash, in 100% of tested cases.
- **SC-007**: A user can reopen a previously visited URL from the recents list in a
  single selection without retyping it.

## Assumptions

- The Browser is a native firmware feature surfaced under the existing **Tools** menu
  (not a sandboxed WASM plugin under the Plugins menu), reusing the existing WiFi
  controller and HTTP/HTTPS client.
- WiFi credentials and the network-join flow are owned by the existing connectivity
  settings; the Browser only requires an active connection and points the user there
  when offline.
- URL entry reuses the existing on-screen text-input component; the recents list reduces
  retyping.
- Rendering reuses the existing scrollable styled-text document viewer; text is shown in
  the device codepage (CP437), and non-Latin scripts may not render fully. The footer is
  repurposed to show the current URL instead of key hints (a deliberate deviation from
  the standard UI flow).
- There is **no** client-side JavaScript execution, **no** CSS layout, and **no**
  cookies / login / form submission (POST). Pages that require JavaScript to render
  their content will show only what is present in the initially delivered HTML (commonly
  just the metadata card).
- Images and other non-text media are **out of scope for v1** (text only); unsupported
  content types show a notice. (Reusing the existing image viewer is a possible future
  extension, not part of this feature.)
- An advertised feed/AMP variant is **preferred** when present, because it is smaller and
  cleaner than the full page (better for both extraction quality and the device's memory
  budget). AMP availability is declining across the web, so the feature must not depend on
  any variant and works equally on pages that advertise none.
- Server-certificate validation for HTTPS is already provided by the existing HTTP client
  (full ESP-IDF CA bundle); the Browser inherits verified HTTPS and introduces no trust
  decisions of its own. HTTP redirects are auto-followed by that client up to its limit.
- All buffers are bounded: page fetch size, extracted-content size (consistent with the
  document viewer's existing source cap), and link count are capped; large inputs are
  truncated with an indicator.
- Per project policy, user/developer documentation under `website/` is updated as part of
  delivering this feature (new Tools entry and behaviour).
