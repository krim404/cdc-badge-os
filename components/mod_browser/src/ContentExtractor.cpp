#include "ContentExtractor.h"

#include "BrowserTextUtil.h"

#include <cstdio>
#include <cstring>

namespace cdc::browser {
using namespace util;

/// Block-tag classification result (kind mirrors ContentExtractor::Block as int).
struct ContentExtractorBlockTag {
    int     kind;   ///< -1 = not a block element.
    uint8_t level;  ///< Heading level when kind == Heading.
};

namespace {

bool suppressed(const HtmlTokenizer* t)
{
    if (!t) return false;
    static const char* s[] = {"script", "style", "noscript", "svg",  "nav",   "header",
                              "footer", "aside", "form",      "select", "button", "template"};
    for (const char* x : s) {
        if (t->inside(x)) return true;
    }
    return false;
}

bool insideArticle(const HtmlTokenizer* t)
{
    return t && (t->inside("article") || t->inside("main"));
}

void resolveUrl(const char* base, const char* rel, char* out, size_t cap)
{
    if (!out || cap == 0) return;
    while (*rel == ' ' || *rel == '\t') ++rel;
    if (!*rel) { std::snprintf(out, cap, "%s", base ? base : ""); return; }
    if (std::strstr(rel, "://")) { std::snprintf(out, cap, "%s", rel); return; }

    const char* schemeEnd = base ? std::strstr(base, "://") : nullptr;
    if (rel[0] == '/' && rel[1] == '/') {
        size_t sl = schemeEnd ? static_cast<size_t>(schemeEnd - base) : 4;
        std::snprintf(out, cap, "%.*s:%s", static_cast<int>(sl), base, rel);
        return;
    }
    const char* hostStart = schemeEnd ? schemeEnd + 3 : base;
    const char* pathStart = hostStart ? std::strchr(hostStart, '/') : nullptr;
    if (rel[0] == '/') {
        size_t hostLen = pathStart ? static_cast<size_t>(pathStart - base) : std::strlen(base);
        std::snprintf(out, cap, "%.*s%s", static_cast<int>(hostLen), base, rel);
        return;
    }
    if (rel[0] == '#') { std::snprintf(out, cap, "%s", base); return; }
    const char* lastSlash = hostStart ? std::strrchr(hostStart, '/') : nullptr;
    if (lastSlash) {
        size_t baseLen = static_cast<size_t>(lastSlash - base) + 1;
        std::snprintf(out, cap, "%.*s%s", static_cast<int>(baseLen), base, rel);
    } else {
        std::snprintf(out, cap, "%s/%s", base, rel);
    }
}

void hostOf(const char* url, char* out, size_t cap)
{
    const char* schemeEnd = std::strstr(url, "://");
    const char* h = schemeEnd ? schemeEnd + 3 : url;
    const char* e = h;
    while (*e && *e != '/' && *e != '?' && *e != '#') ++e;
    size_t n = static_cast<size_t>(e - h);
    if (n >= cap) n = cap - 1;
    std::memcpy(out, h, n);
    out[n] = '\0';
}

}  // namespace

namespace {
bool blockKindOf(const char* name, uint16_t len, ContentExtractorBlockTag& tag)
{
    tag.kind = -1;
    tag.level = 0;
    if (len == 2 && (name[0] == 'h' || name[0] == 'H') && name[1] >= '1' && name[1] <= '6') {
        tag.kind = 1;  // Heading
        tag.level = static_cast<uint8_t>(name[1] - '0');
        return true;
    }
    if (ieq(name, len, "li")) { tag.kind = 2; return true; }          // Bullet
    if (ieq(name, len, "blockquote")) { tag.kind = 3; return true; }  // Quote
    if (ieq(name, len, "pre")) { tag.kind = 4; return true; }         // Code
    static const char* para[] = {"p",   "div",     "section", "article", "main",  "ul",
                                 "ol",  "table",   "tr",      "td",      "th",    "header",
                                 "footer", "nav",  "aside",   "figure",  "figcaption",
                                 "dd",  "dt",      "dl"};
    for (const char* x : para) {
        if (ieq(name, len, x)) { tag.kind = 0; return true; }  // Paragraph
    }
    return false;
}
}  // namespace

ContentExtractor::ContentExtractor(const ExtractConfig& cfg, const char* baseUrl, char* outBuf,
                                   size_t outCap, LinkRef* links, uint16_t linksCap,
                                   FormSpec* forms, uint8_t formsCap)
    : cfg_(cfg), baseUrl_(baseUrl), out_(outBuf), outCap_(outCap), links_(links),
      linksCap_(linksCap), forms_(forms), formsCap_(formsCap)
{
    head_.clear();
    if (out_ && outCap_) out_[0] = '\0';
}

bool ContentExtractor::outPut(const char* s, size_t n)
{
    if (!out_ || outCap_ == 0) return false;
    if (outLen_ + n > outCap_ - 1) {
        size_t room = (outCap_ - 1 > outLen_) ? (outCap_ - 1 - outLen_) : 0;
        if (room) { std::memcpy(out_ + outLen_, s, room); outLen_ += room; }
        out_[outLen_] = '\0';
        truncated_ = true;
        return false;
    }
    std::memcpy(out_ + outLen_, s, n);
    outLen_ += n;
    out_[outLen_] = '\0';
    return true;
}

bool ContentExtractor::outPutStr(const char* s) { return outPut(s, std::strlen(s)); }

void ContentExtractor::blockPut(char c)
{
    if (isWs(c)) {
        if (!lastWasSpace_ && blockLen_ < kBlockCap) {
            block_[blockLen_++] = ' ';
            lastWasSpace_ = true;
        }
        return;
    }
    if (blockLen_ < kBlockCap) block_[blockLen_++] = c;
    lastWasSpace_ = false;
    blockTextChars_++;
    if (anchorDepth_ > 0) blockLinkChars_++;
}

void ContentExtractor::blockPutStr(const char* s)
{
    while (*s && blockLen_ < kBlockCap) block_[blockLen_++] = *s++;
}

void ContentExtractor::startBlock(Block kind, uint8_t headingLevel)
{
    blockKind_ = kind;
    headingLevel_ = headingLevel;
    blockLen_ = 0;
    blockTextChars_ = 0;
    blockLinkChars_ = 0;
    lastWasSpace_ = true;
    blockLinkStart_ = linkCount_;
}

void ContentExtractor::finishBlock()
{
    // Trim trailing space.
    while (blockLen_ > 0 && block_[blockLen_ - 1] == ' ') --blockLen_;

    bool keep = false;
    if (blockLen_ == 0 || blockTextChars_ == 0) {
        keep = false;
    } else if (cfg_.keepAll) {
        keep = true;  // full-page mode: keep every non-empty block
    } else {
        double linkDensity =
            static_cast<double>(blockLinkChars_) / static_cast<double>(blockTextChars_ ? blockTextChars_ : 1);
        if (blockKind_ == Block::Heading) {
            keep = linkDensity <= 0.90;
        } else if (linkDensity > cfg_.maxLinkDensity) {
            keep = insideArticle(tok_) && linkDensity <= cfg_.maxLinkDensity * 2.0;
        } else if (blockTextChars_ >= cfg_.lengthHigh) {
            keep = true;
        } else if (blockTextChars_ >= cfg_.lengthLow) {
            keep = true;
        } else {
            keep = insideArticle(tok_);  // short blocks only inside article/main
        }
    }

    if (!keep) {
        // The block (and its buffered [n] markers) is dropped, so discard the
        // links collected within it; otherwise the navigable link table would
        // list links that have no marker in the rendered text.
        linkCount_ = blockLinkStart_;
        if (pendingLinkIdx_ >= static_cast<int>(linkCount_)) pendingLinkIdx_ = -1;
        startBlock(Block::Paragraph, 0);
        return;
    }

    {
        switch (blockKind_) {
            case Block::Heading: {
                char pfx[8];
                uint8_t lvl = headingLevel_ ? headingLevel_ : 1;
                if (lvl > 6) lvl = 6;
                for (uint8_t i = 0; i < lvl; ++i) pfx[i] = '#';
                pfx[lvl] = ' ';
                pfx[lvl + 1] = '\0';
                outPutStr(pfx);
                break;
            }
            case Block::Bullet: outPutStr("- "); break;
            case Block::Quote: outPutStr("> "); break;
            case Block::Code: outPutStr("```\n"); break;
            case Block::Paragraph: break;
        }
        outPut(block_, blockLen_);
        if (blockKind_ == Block::Code) outPutStr("\n```");
        outPutStr("\n\n");
        keptTextChars_ += blockTextChars_;
    }
    startBlock(Block::Paragraph, 0);
}

void ContentExtractor::onStartTag(const char* name, uint16_t nameLen, const AttrList& attrs,
                                  bool selfClosing)
{
    (void)selfClosing;

    if (ieq(name, nameLen, "title")) { capTitle_ = true; return; }
    if (ieq(name, nameLen, "body")) { headDone_ = true; }

    if (ieq(name, nameLen, "meta")) {
        char key[48] = {0};
        if (!attrs.copy("property", key, sizeof(key))) attrs.copy("name", key, sizeof(key));
        if (key[0]) {
            char content[kDescCap];
            if (ieq(key, static_cast<uint16_t>(std::strlen(key)), "og:title") && !head_.ogTitle[0]) {
                attrs.copy("content", head_.ogTitle, sizeof(head_.ogTitle));
            } else if (ieq(key, static_cast<uint16_t>(std::strlen(key)), "og:description") &&
                       !head_.ogDescription[0]) {
                attrs.copy("content", head_.ogDescription, sizeof(head_.ogDescription));
            } else if (ieq(key, static_cast<uint16_t>(std::strlen(key)), "description") &&
                       !head_.metaDescription[0]) {
                attrs.copy("content", head_.metaDescription, sizeof(head_.metaDescription));
            }
            (void)content;
        }
        return;
    }

    if (ieq(name, nameLen, "link")) {
        char rel[48] = {0};
        attrs.copy("rel", rel, sizeof(rel));
        if (!rel[0]) return;
        char href[kUrlCap] = {0};
        if (!attrs.copy("href", href, sizeof(href)) || !href[0]) return;
        if (std::strstr(rel, "canonical") && !head_.canonical[0]) {
            resolveUrl(baseUrl_, href, head_.canonical, sizeof(head_.canonical));
        } else if (std::strstr(rel, "amphtml") && !head_.amphtml[0]) {
            resolveUrl(baseUrl_, href, head_.amphtml, sizeof(head_.amphtml));
        } else if (std::strstr(rel, "alternate") && !head_.feedUrl[0]) {
            char type[64] = {0};
            attrs.copy("type", type, sizeof(type));
            if (std::strstr(type, "rss+xml") || std::strstr(type, "atom+xml")) {
                resolveUrl(baseUrl_, href, head_.feedUrl, sizeof(head_.feedUrl));
            }
        }
        return;
    }

    if (ieq(name, nameLen, "a")) {
        if (suppressed(tok_)) return;
        char href[kUrlCap] = {0};
        if (!attrs.copy("href", href, sizeof(href)) || !href[0]) return;
        if (href[0] == '#') return;  // in-page anchor, not navigable content
        if (linkCount_ < linksCap_ && links_) {
            LinkRef& lr = links_[linkCount_];
            lr.index = static_cast<uint16_t>(linkCount_ + 1);
            resolveUrl(baseUrl_, href, lr.href, sizeof(lr.href));
            lr.label[0] = '\0';
            pendingLinkIdx_ = static_cast<int>(linkCount_);
            ++linkCount_;
            blockPutStr("\x10");  // link-text start delimiter (hidden, rendered underlined)
            lastWasSpace_ = true;
            linkLabelStart_ = blockLen_;
        }
        ++anchorDepth_;
        return;
    }

    // Form capture (independent of body-text suppression).
    if (ieq(name, nameLen, "form")) {
        curFormIdx_ = -1;
        if (forms_ && formCount_ < formsCap_) {
            curFormIdx_ = static_cast<int>(formCount_++);
            FormSpec& f = forms_[curFormIdx_];
            f.fieldCount = 0;
            char method[8] = {0};
            attrs.copy("method", method, sizeof(method));
            f.post = (method[0] == 'p' || method[0] == 'P');
            char action[kUrlCap] = {0};
            if (attrs.copy("action", action, sizeof(action)) && action[0]) {
                resolveUrl(baseUrl_, action, f.action, sizeof(f.action));
            } else {
                std::strncpy(f.action, baseUrl_ ? baseUrl_ : "", sizeof(f.action) - 1);
                f.action[sizeof(f.action) - 1] = '\0';
            }
        }
        return;
    }
    if (curFormIdx_ >= 0 && (ieq(name, nameLen, "input") || ieq(name, nameLen, "button"))) {
        FormSpec& f = forms_[curFormIdx_];
        if (f.fieldCount >= kMaxFormFields) return;
        char type[16] = {0};
        attrs.copy("type", type, sizeof(type));
        uint16_t tlen = static_cast<uint16_t>(std::strlen(type));
        FormField::Kind kind;
        if (ieq(name, nameLen, "input")) {
            if (ieq(type, tlen, "hidden")) kind = FormField::Kind::Hidden;
            else if (ieq(type, tlen, "checkbox")) kind = FormField::Kind::Checkbox;
            else if (ieq(type, tlen, "submit") || ieq(type, tlen, "image")) kind = FormField::Kind::Submit;
            else return;  // text/password/email/radio/select not handled in v1
        } else {
            if (type[0] && !ieq(type, tlen, "submit")) return;  // <button> defaults to submit
            kind = FormField::Kind::Submit;
        }
        FormField& fld = f.fields[f.fieldCount];
        fld = FormField{};
        fld.kind = kind;
        attrs.copy("name", fld.name, sizeof(fld.name));
        attrs.copy("value", fld.value, sizeof(fld.value));
        const char* cv = nullptr;
        uint16_t cl = 0;
        fld.checked = (kind == FormField::Kind::Checkbox) && attrs.get("checked", &cv, &cl);
        if (kind != FormField::Kind::Submit && fld.name[0] == '\0') return;  // need a name
        ++f.fieldCount;
        return;
    }

    ContentExtractorBlockTag tag;
    if (blockKindOf(name, nameLen, tag)) {
        finishBlock();
        Block k = static_cast<Block>(tag.kind);
        startBlock(k, tag.level);
    } else if (ieq(name, nameLen, "br")) {
        if (blockTextChars_ > 0) {
            Block k = blockKind_;
            uint8_t lvl = headingLevel_;
            finishBlock();
            startBlock(k, lvl);
        }
    }
}

void ContentExtractor::onEndTag(const char* name, uint16_t nameLen)
{
    if (ieq(name, nameLen, "title")) { capTitle_ = false; return; }
    if (ieq(name, nameLen, "head")) { headDone_ = true; return; }
    if (ieq(name, nameLen, "form")) { curFormIdx_ = -1; return; }

    if (ieq(name, nameLen, "a")) {
        if (anchorDepth_ > 0) --anchorDepth_;
        if (pendingLinkIdx_ >= 0 && pendingLinkIdx_ < static_cast<int>(linkCount_)) {
            LinkRef& lr = links_[pendingLinkIdx_];
            size_t n = (blockLen_ > linkLabelStart_) ? (blockLen_ - linkLabelStart_) : 0;
            if (n > 0) {
                if (n >= sizeof(lr.label)) n = sizeof(lr.label) - 1;
                std::memcpy(lr.label, block_ + linkLabelStart_, n);
                lr.label[n] = '\0';
            }
            blockPutStr("\x11");  // link-text end delimiter
            pendingLinkIdx_ = -1;
        }
        return;
    }

    ContentExtractorBlockTag tag;
    if (blockKindOf(name, nameLen, tag)) finishBlock();
}

void ContentExtractor::onText(const char* text, size_t len)
{
    if (capTitle_) {
        size_t cur = std::strlen(head_.title);
        for (size_t i = 0; i < len && cur < sizeof(head_.title) - 1; ++i) {
            char c = text[i];
            if (isWs(c)) {
                if (cur > 0 && head_.title[cur - 1] != ' ') head_.title[cur++] = ' ';
            } else {
                head_.title[cur++] = c;
            }
        }
        head_.title[cur] = '\0';
        return;
    }
    if (suppressed(tok_)) return;
    for (size_t i = 0; i < len; ++i) blockPut(text[i]);
}

void ContentExtractor::finalize()
{
    finishBlock();

    for (uint16_t i = 0; i < linkCount_; ++i) {
        if (!links_[i].label[0]) hostOf(links_[i].href, links_[i].label, sizeof(links_[i].label));
    }

    if (keptTextChars_ < 40) {
        // Fall back to an Open Graph / metadata card.
        outLen_ = 0;
        if (out_ && outCap_) out_[0] = '\0';
        const char* title = head_.ogTitle[0] ? head_.ogTitle : head_.title;
        const char* desc = head_.ogDescription[0] ? head_.ogDescription : head_.metaDescription;
        if ((title && title[0]) || (desc && desc[0])) {
            if (title && title[0]) {
                outPutStr("# ");
                outPutStr(title);
                outPutStr("\n\n");
            }
            if (desc && desc[0]) outPutStr(desc);
            kind_ = SourceKind::OgCard;
        } else {
            kind_ = SourceKind::Empty;
        }
    } else {
        kind_ = SourceKind::MainExtract;
    }
    if (out_ && outCap_) out_[outLen_] = '\0';
}

}  // namespace cdc::browser
