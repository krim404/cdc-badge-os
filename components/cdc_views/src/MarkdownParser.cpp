#include "cdc_views/MarkdownParser.h"

namespace cdc::ui {
namespace {

struct Span {
    const char* s;
    uint16_t len;
};

bool isSpace(char c) { return c == ' ' || c == '\t'; }

/// Returns the raw line [start, end) within [0, n), and the index just past the
/// trailing '\n'. Strips a trailing '\r'.
size_t nextLine(const char* src, size_t n, size_t pos, Span& out) {
    size_t start = pos;
    size_t i = pos;
    while (i < n && src[i] != '\n') ++i;
    size_t end = i;
    if (end > start && src[end - 1] == '\r') --end;
    out.s = src + start;
    out.len = static_cast<uint16_t>(end - start);
    return (i < n) ? i + 1 : n;
}

/// Trims leading and trailing ASCII whitespace, reporting the leading count.
Span trimmed(Span raw, uint16_t& leading) {
    uint16_t a = 0;
    while (a < raw.len && isSpace(raw.s[a])) ++a;
    uint16_t b = raw.len;
    while (b > a && isSpace(raw.s[b - 1])) --b;
    leading = a;
    return Span{raw.s + a, static_cast<uint16_t>(b - a)};
}

/// Heading level 1..6 if the line is an ATX heading (`#`+ followed by space/end),
/// else 0.
uint8_t headingLevel(Span t) {
    uint8_t h = 0;
    while (h < t.len && t.s[h] == '#') ++h;
    if (h >= 1 && h <= 6 && (h == t.len || t.s[h] == ' ')) return h;
    return 0;
}

/// True if the line is a thematic break (>=3 of the same -, *, _ ignoring spaces).
bool isRule(Span t) {
    if (t.len < 3) return false;
    char marker = 0;
    int count = 0;
    for (uint16_t i = 0; i < t.len; ++i) {
        char c = t.s[i];
        if (c == ' ') continue;
        if (c != '-' && c != '*' && c != '_') return false;
        if (marker == 0) marker = c;
        else if (c != marker) return false;
        ++count;
    }
    return count >= 3;
}

bool isUnorderedMarker(Span t) {
    return t.len >= 2 && (t.s[0] == '-' || t.s[0] == '*' || t.s[0] == '+') && t.s[1] == ' ';
}

/// Length of an ordered-list marker ("12. " / "3) ") or 0.
uint16_t orderedMarkerLen(Span t) {
    uint16_t i = 0;
    while (i < t.len && t.s[i] >= '0' && t.s[i] <= '9') ++i;
    if (i == 0 || i + 1 >= t.len) return 0;
    if ((t.s[i] == '.' || t.s[i] == ')') && t.s[i + 1] == ' ') return static_cast<uint16_t>(i + 2);
    return 0;
}

bool isFenceDelim(Span t) {
    return t.len >= 3 &&
           ((t.s[0] == '`' && t.s[1] == '`' && t.s[2] == '`') ||
            (t.s[0] == '~' && t.s[1] == '~' && t.s[2] == '~'));
}

/// Strips the leading hashes (and one space) and any trailing hashes/spaces.
Span headingText(Span t, uint8_t level) {
    uint16_t a = level;
    if (a < t.len && t.s[a] == ' ') ++a;
    uint16_t b = t.len;
    while (b > a && (t.s[b - 1] == '#' || t.s[b - 1] == ' ')) --b;
    return Span{t.s + a, static_cast<uint16_t>(b - a)};
}

} // namespace

MarkdownParseResult parseMarkdown(const char* src, size_t len, StyledLineSink& sink, size_t maxBytes) {
    MarkdownParseResult result;
    size_t n = len;
    if (n > maxBytes) {
        n = maxBytes;
        result.truncated = true;
    }
    if (!src) return result;

    // Pass 1: collect distinct heading levels (ignoring fenced code) and map
    // each present level to a font, smallest font for the least-prominent level.
    bool present[7] = {false, false, false, false, false, false, false};
    {
        bool fence = false;
        size_t pos = 0;
        Span raw;
        while (pos < n) {
            pos = nextLine(src, n, pos, raw);
            uint16_t lead = 0;
            Span t = trimmed(raw, lead);
            if (isFenceDelim(t)) { fence = !fence; continue; }
            if (fence) continue;
            uint8_t lvl = headingLevel(t);
            if (lvl) present[lvl] = true;
        }
    }

    uint8_t levelFont[7] = {mdfont::Builtin, mdfont::Builtin, mdfont::Builtin, mdfont::Builtin,
                            mdfont::Builtin, mdfont::Builtin, mdfont::Builtin};
    {
        const uint8_t fonts[3] = {mdfont::Bold9pt, mdfont::Bold12pt, mdfont::Bold18pt};
        int idx = 0;
        for (int lvl = 6; lvl >= 1; --lvl) {
            if (!present[lvl]) continue;
            levelFont[lvl] = fonts[idx < 3 ? idx : 2];
            ++idx;
        }
    }

    // Pass 2: classify and emit each line.
    bool fence = false;
    size_t pos = 0;
    Span raw;
    while (pos < n) {
        pos = nextLine(src, n, pos, raw);
        uint16_t lead = 0;
        Span t = trimmed(raw, lead);

        StyledLine line;

        if (isFenceDelim(t)) {
            fence = !fence;
            continue;  // delimiters are not rendered
        }
        if (fence) {
            line.kind = MdLineKind::Code;
            line.inverted = true;
            line.text = raw.s;
            line.len = raw.len;
            sink.emit(line);
            ++result.lineCount;
            continue;
        }
        if (t.len == 0) {
            line.kind = MdLineKind::Blank;
            line.text = t.s;
            line.len = 0;
        } else if (uint8_t lvl = headingLevel(t)) {
            Span h = headingText(t, lvl);
            line.kind = MdLineKind::Heading;
            line.font = levelFont[lvl];
            line.text = h.s;
            line.len = h.len;
        } else if (isRule(t)) {
            line.kind = MdLineKind::Rule;
            line.text = t.s;
            line.len = 0;
        } else if (t.s[0] == '>') {
            uint16_t a = 1;
            if (a < t.len && t.s[a] == ' ') ++a;
            line.kind = MdLineKind::Quote;
            line.text = t.s + a;
            line.len = static_cast<uint16_t>(t.len - a);
        } else if (uint16_t om = orderedMarkerLen(t)) {
            line.kind = MdLineKind::Ordered;
            line.indent = static_cast<uint8_t>(lead / 2);
            line.text = t.s + om;
            line.len = static_cast<uint16_t>(t.len - om);
        } else if (isUnorderedMarker(t)) {
            line.kind = MdLineKind::Bullet;
            line.indent = static_cast<uint8_t>(lead / 2);
            line.text = t.s + 2;
            line.len = static_cast<uint16_t>(t.len - 2);
        } else if (lead >= 4) {
            line.kind = MdLineKind::Code;
            line.inverted = true;
            line.text = raw.s + 4;
            line.len = static_cast<uint16_t>(raw.len - 4);
        } else {
            line.kind = MdLineKind::Paragraph;
            line.text = t.s;
            line.len = t.len;
        }

        sink.emit(line);
        ++result.lineCount;
    }

    return result;
}

} // namespace cdc::ui
