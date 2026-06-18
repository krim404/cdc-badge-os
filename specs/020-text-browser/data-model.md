# Phase 1 Data Model: Text Browser

All structures are **in-RAM only** (PSRAM), session-scoped, never persisted. No NVS/flash/FAT.
Sizes below are working defaults; finalize as `constexpr` in the module headers.

## PageDocument

The result of loading one URL. Lives for as long as it is the current page or on the history
stack. Backing buffers are `PsramUniquePtr<char>`.

| Field | Type | Notes |
|-------|------|-------|
| `url` | bounded char buffer (<=512) | Final URL after redirects/canonical/mirror rewrite. Shown in footer (FR-021). |
| `title` | bounded char buffer (<=128) | From `<title>` or `og:title` (CP437). |
| `source` | `PsramUniquePtr<char>` (<=64 KB) | Markdown-ish body with inline `[n]` link markers; fed to `MarkdownView::init`. |
| `sourceLen` | `size_t` | Bytes in `source`. |
| `links` | `LinkRef[]` (cap 128) | Parallel link table; index = the `[n]` shown inline. |
| `linkCount` | `uint16_t` | Number of links collected; truncation flagged when cap hit. |
| `sourceKind` | enum | `Feed`, `Amp`, `Mirror`, `MainExtract`, `OgCard`, `RawText`, `Empty` - which tier produced the content (diagnostics + footer/UX hints). |
| `truncated` | `bool` | Set when 512 KB scan cap or 64 KB source cap or link cap was reached. |

State: a PageDocument is *built* by the fetch/extract pipeline, then *displayed* by
`BrowserPageView`. It is immutable once built (a new load creates a new PageDocument).

## LinkRef

One collected hyperlink.

| Field | Type | Notes |
|-------|------|-------|
| `index` | `uint16_t` | The number rendered inline as `[index]` and listed in the links view. |
| `label` | bounded char buffer (<=80) | Readable anchor text (CP437), truncated; falls back to the host if empty. |
| `href` | bounded char buffer (<=512) | Resolved **absolute** URL (relative resolved against the page URL before storage). |

## HeadSignals

Captured during the single head scan; drives `SourceSelector`. Transient (not retained after
the source decision, except `title`/`ogDescription` which may seed PageDocument).

| Field | Type | Notes |
|-------|------|-------|
| `title` | bounded char buffer | `<title>` text. |
| `ogTitle` / `ogDescription` | bounded char buffers | `og:title` / `og:description`. |
| `metaDescription` | bounded char buffer | `<meta name="description">`. |
| `canonical` | bounded char buffer | `<link rel="canonical">` href. |
| `amphtml` | bounded char buffer | `<link rel="amphtml">` href (empty if none). |
| `feedUrl` | bounded char buffer | First `<link rel="alternate" type=rss/atom>` href (empty if none). |
| `jsonLdArticleBody` | optional `PsramUniquePtr<char>` (capped 16-32 KB) | Opportunistic; empty when absent/over cap. |

## BrowserSession

Owns the live browsing state for the module. One instance (Meyer's singleton or owned by the
module), valid only while the Browser is in use; cleared on exit, reboot, and device lock.

| Field | Type | Notes |
|-------|------|-------|
| `current` | `PageDocument*` | The page currently displayed. |
| `history` | `PageDocument` stack (bounded depth, e.g. 8) | Back-history; `N` pops. Empty -> exit Browser (FR-011). |
| `recents` | `RecentUrl[]` (cap 16, ring) | Session-only recents (FR-019); oldest dropped at capacity. |
| `scrollOf` | per-history-entry `uint16_t` | Best-effort scroll restore on back (FR-011). |

State transitions:
- **load(url)**: build PageDocument -> push current onto `history` (with its scroll pos) ->
  set `current` -> add `url` to `recents`.
- **back()**: pop `history` -> restore as `current` (and scroll pos); if empty, exit.
- **lock/reboot/exit**: free all PageDocuments, clear `history` and `recents`.

## RecentUrl

| Field | Type | Notes |
|-------|------|-------|
| `url` | bounded char buffer (<=512) | Visited URL. |
| `label` | bounded char buffer (<=80) | Page title if known, else the URL host. |

## Static configuration (compile-time)

| Name | Type | Purpose |
|------|------|---------|
| `kTextMirrorMap` | `{host, mirrorHost}[]` | Small built-in table (e.g. `cnn.com -> lite.cnn.com`). Source tier 1. |
| `kStopwords` | `const char*[]` | Small ASCII/CP437 stoplist for jusText-lite stopword density. |
| `kMaxScanBytes` | `constexpr size_t = 512*1024` | Raw response scan cap (FR-005). |
| `kMaxLinks` | `constexpr uint16_t = 128` | Link-table cap (FR-015). |
| `kMaxRecents` | `constexpr uint8_t = 16` | Recents ring size. |
| `kMaxHistory` | `constexpr uint8_t = 8` | Back-history depth. |

## Validation rules (from requirements)

- All char buffers are fixed-capacity and always null-terminated; over-length input is
  truncated, never overflowed (FR-017 memory safety).
- `href` is always stored resolved-absolute (FR-010).
- A PageDocument with `sourceKind == Empty` triggers the "no readable content" UX (FR-007/FR-013).
- `truncated` true -> footer/area shows a truncation indicator (FR-005/FR-015).
