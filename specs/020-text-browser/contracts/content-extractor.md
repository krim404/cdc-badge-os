# Contract: ContentExtractor

Consumes `HtmlTokenizer` events (it IS a `TokenSink`) to (a) capture head signals and (b) build
the Markdown-ish body source with a jusText-lite density heuristic. Host-testable, no GFX/IDF.

## Interface (shape)

```cpp
namespace cdc::browser {

struct ExtractConfig {
    size_t maxSourceBytes;     // 64 KB (MarkdownView cap)
    size_t maxScanBytes;       // 512 KB (FR-005)
    uint16_t maxLinks;         // 128
    double maxLinkDensity;     // ~0.20 (jusText)
    uint16_t lengthLow;        // ~70
    uint16_t lengthHigh;       // ~200
};

struct ExtractResult {
    HeadSignals head;          // title, og:*, meta desc, canonical, amphtml, feedUrl, jsonLdArticleBody
    char*    source;           // Markdown-ish body, inline [n] markers (PSRAM, caller-owned)
    size_t   sourceLen;
    LinkRef* links;            // parallel link table
    uint16_t linkCount;
    bool     truncated;
    enum class Kind { MainExtract, OgCard, Empty } kind;
};

class ContentExtractor : public TokenSink {
public:
    ContentExtractor(const ExtractConfig&, const char* baseUrl /* for link resolution */);
    // TokenSink overrides drive accumulation.
    ExtractResult finalize();  // classify pending block, apply fallbacks, return result
    bool headComplete() const; // true once </head> (or first body content) seen
    const HeadSignals& head() const;  // usable after headComplete() for source selection
};

} // namespace cdc::browser
```

## Heuristic (jusText-lite, single pass)

- **Suppress** text while the tag stack contains any of: `script style noscript svg nav header
  footer aside form select`.
- **Segment** into blocks at block-level tags (`p div li h1..h6 blockquote article section td
  pre tr ul ol`) and runs of `>= 2` `<br>`.
- **Per block** accumulate: total text chars, link chars (inside `<a>`), stopword hits.
- **Classify**: link density `> maxLinkDensity` -> boilerplate (drop). Length `< lengthLow` and
  link-dense -> drop; `>= lengthHigh` with low link density -> keep; middle band decided by
  stopword density with a single-block look-behind.
- **Prefer `<article>/<main>`**: blocks inside them get a keep boost; if any content appears
  inside them, content outside is largely ignored.
- **Emit** kept blocks as Markdown-ish text: map `h1..h6 -> #..######`, `li -> -`/`1.`,
  `blockquote -> >`, `pre/code -> fenced`, `b/strong`,`i/em` inline; insert `[n]` at each `<a>`
  and append the resolved href to the link table (cap `maxLinks`).
- **Stop** at `maxSourceBytes` or `maxScanBytes` -> set `truncated`.

## Fallbacks (in `finalize`)

- Kept text below a minimum threshold -> `Kind::OgCard`: emit `# ogTitle` + ogDescription/
  metaDescription as the source.
- Still empty -> `Kind::Empty` (caller shows "requires JavaScript / no readable content").
- If `head.jsonLdArticleBody` is present and non-trivial, it MAY be used directly as the body,
  skipping block heuristics.

## Head capture

- `<title>` text; `<meta property="og:title|og:description" content=...>`;
  `<meta name="description" content=...>`; `<link rel="canonical|amphtml" href=...>`;
  `<link rel="alternate" type="application/rss+xml|atom+xml" href=...>`;
  `<script type="application/ld+json">` block captured (capped) for an `"articleBody"` scrape.

## Host tests

- Wikipedia-like article fixture: main prose kept, nav/infobox/footer dropped, links numbered.
- Nav-heavy page: high-link-density menus dropped.
- `<article>`-wrapped page: only article content emitted.
- SPA shell (near-empty body) -> `Kind::OgCard` then `Kind::Empty` when no OG.
- Head capture: og/description/canonical/amphtml/feed correctly parsed.
- Caps: link count, source bytes, scan bytes -> `truncated` set.
