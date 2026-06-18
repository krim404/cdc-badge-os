# Contract: HtmlTokenizer

Streaming, host-testable HTML tokenizer. No DOM, no GFX/ESP-IDF deps. Fed bytes incrementally;
emits events via a sink. Memory is constant (state + bounded open-tag stack + caller buffers).

## Interface (shape)

```cpp
namespace cdc::browser {

enum class TokKind : uint8_t { StartTag, EndTag, Text, Comment };

struct TagEvent {
    const char* name;     // lowercased tag name (e.g. "a", "div"); span into scratch
    uint8_t     nameLen;
    bool        selfClosing;
    // attribute lookup is pull-style during a StartTag callback:
    // bool attr(const char* key, const char** valOut, size_t* lenOut);
};

struct TokenSink {
    virtual void onStartTag(const TagEvent& t) = 0;
    virtual void onEndTag(const char* name, uint8_t len) = 0;
    virtual void onText(const char* utf8, size_t len) = 0;  // raw text run (entities NOT yet decoded)
    virtual ~TokenSink() = default;
};

class HtmlTokenizer {
public:
    explicit HtmlTokenizer(TokenSink& sink);
    void feed(const char* bytes, size_t len);  // push a chunk; may emit 0..n events
    void finish();                              // flush trailing text
    bool inside(const char* tagName) const;     // open-tag stack query (O(stack))
    uint8_t depth() const;
};

} // namespace cdc::browser
```

## Behavioral guarantees

- **RAWTEXT/RCDATA**: inside `<script>`/`<style>`, content up to the matching close tag is NOT
  interpreted as markup (no `onStartTag` for `<` inside); the body may be dropped by the sink.
- **Lenient**: unclosed/garbage/exotic tags never crash or hang; unbalanced end tags pop to the
  nearest match or are ignored. Malformed `<` with no valid tag is treated as text (FR-017).
- **Bounded**: open-tag stack capped (depth <= 32); deeper nesting stops pushing (still safe).
  Tag-name and attribute scratch are fixed caller buffers; longer names/values are truncated.
- **Chunk-agnostic**: a tag or attribute split across `feed()` calls is handled (partial token
  carried in state); output is identical regardless of chunk boundaries.
- **Text runs** are emitted raw (UTF-8, entities intact); entity decode + CP437 conversion is a
  later stage (`render::decodeWebText`).

## Host tests (test_html_extract)

- Split-chunk invariance: feeding the same HTML in 1-byte vs 4 KB chunks yields identical events.
- `<script>`/`<style>` bodies never surface as `onStartTag`.
- Unclosed `<div>`, stray `</p>`, `<b><i></b>` mis-nesting: no crash, stack stays bounded.
- `inside("article")` / `inside("a")` correct across nesting.
- Attribute parse: quoted/unquoted/missing values; `attr()` lookups by key.
