#include "HtmlTokenizer.h"

#include "BrowserTextUtil.h"

#include <cstring>

namespace cdc::browser {
using namespace util;
namespace {

bool isVoid(const char* name, uint16_t len)
{
    static const char* kVoid[] = {"area", "base",  "br",   "col",  "embed", "hr",
                                  "img",  "input", "link", "meta", "param", "source",
                                  "track", "wbr"};
    for (const char* v : kVoid) {
        if (ieq(name, len, v)) return true;
    }
    return false;
}

}  // namespace

bool AttrList::get(const char* key, const char** value, uint16_t* len) const
{
    for (int i = 0; i < count; ++i) {
        if (ieq(items[i].name, items[i].nameLen, key)) {
            if (value) *value = items[i].value;
            if (len) *len = items[i].valueLen;
            return true;
        }
    }
    return false;
}

bool AttrList::copy(const char* key, char* out, size_t outCap) const
{
    if (!out || outCap == 0) return false;
    const char* v = nullptr;
    uint16_t vl = 0;
    if (!get(key, &v, &vl)) { out[0] = '\0'; return false; }
    size_t n = (vl < outCap - 1) ? vl : outCap - 1;
    std::memcpy(out, v, n);
    out[n] = '\0';
    return true;
}

HtmlTokenizer::HtmlTokenizer(TokenSink& sink) : sink_(sink) { rawName_[0] = '\0'; }

void HtmlTokenizer::feed(const char* bytes, size_t len)
{
    if (!bytes) return;
    for (size_t i = 0; i < len; ++i) byte(bytes[i]);
}

void HtmlTokenizer::finish() { flushText(); }

void HtmlTokenizer::flushText()
{
    if (textLen_) {
        sink_.onText(textBuf_, textLen_);
        textLen_ = 0;
    }
}

void HtmlTokenizer::byte(char c)
{
    switch (state_) {
        case State::Text:
            if (c == '<') {
                flushText();
                state_ = State::InTag;
                tagLen_ = 0;
                tagQuote_ = 0;
            } else {
                if (textLen_ >= kTextCap) flushText();
                textBuf_[textLen_++] = c;
            }
            return;

        case State::InTag:
            // Detect a comment as soon as we have "!--".
            if (tagLen_ == 3 && tagBuf_[0] == '!' && tagBuf_[1] == '-' && tagBuf_[2] == '-') {
                state_ = State::Comment;
                commentDashes_ = 0;
                // Re-process current byte under Comment rules.
                byte(c);
                return;
            }
            if (tagQuote_) {
                if (c == tagQuote_) tagQuote_ = 0;
                if (tagLen_ < kTagCap) tagBuf_[tagLen_++] = c;
                return;
            }
            if (c == '"' || c == '\'') {
                tagQuote_ = c;
                if (tagLen_ < kTagCap) tagBuf_[tagLen_++] = c;
                return;
            }
            if (c == '>') {
                emitBufferedTag();
                if (state_ == State::InTag) state_ = State::Text;  // emit may switch to RawText
                tagLen_ = 0;
                return;
            }
            if (tagLen_ < kTagCap) tagBuf_[tagLen_++] = c;
            return;

        case State::Comment:
            if (c == '-') {
                if (commentDashes_ < 2) commentDashes_++;
            } else if (c == '>' && commentDashes_ >= 2) {
                state_ = State::Text;
                tagLen_ = 0;
                commentDashes_ = 0;
            } else {
                commentDashes_ = 0;
            }
            return;

        case State::RawText: {
            const uint16_t targetLen = static_cast<uint16_t>(2 + rawNameLen_);
            char want;
            if (rawClosePos_ == 0) want = '<';
            else if (rawClosePos_ == 1) want = '/';
            else want = rawName_[rawClosePos_ - 2];

            if (rawClosePos_ < targetLen) {
                if (lc(c) == lc(want)) {
                    rawClosePos_++;
                } else {
                    rawClosePos_ = (c == '<') ? 1 : 0;
                }
                return;
            }
            // Matched "</name": require a tag terminator.
            if (c == '>') {
                popTag(rawName_, rawNameLen_);
                sink_.onEndTag(rawName_, rawNameLen_);
                state_ = State::Text;
                tagLen_ = 0;
                rawClosePos_ = 0;
            } else if (isWs(c) || c == '/') {
                // stay matched, swallow until '>'
            } else {
                rawClosePos_ = (c == '<') ? 1 : 0;
            }
            return;
        }
    }
}

void HtmlTokenizer::emitBufferedTag()
{
    size_t i = 0;
    while (i < tagLen_ && isWs(tagBuf_[i])) ++i;
    if (i >= tagLen_) return;

    bool endTag = false;
    if (tagBuf_[i] == '/') { endTag = true; ++i; }
    if (i < tagLen_ && (tagBuf_[i] == '!' || tagBuf_[i] == '?')) return;  // doctype/PI/CDATA

    size_t nameStart = i;
    while (i < tagLen_ && isNameChar(tagBuf_[i])) {
        tagBuf_[i] = lc(tagBuf_[i]);
        ++i;
    }
    uint16_t nameLen = static_cast<uint16_t>(i - nameStart);
    if (nameLen == 0) return;
    const char* name = &tagBuf_[nameStart];

    if (endTag) {
        popTag(name, nameLen);
        sink_.onEndTag(name, nameLen);
        return;
    }

    AttrList attrs;
    bool selfClosing = false;
    while (i < tagLen_) {
        while (i < tagLen_ && isWs(tagBuf_[i])) ++i;
        if (i >= tagLen_) break;
        if (tagBuf_[i] == '/') { selfClosing = true; ++i; continue; }
        if (!isNameChar(tagBuf_[i])) { ++i; continue; }

        size_t aNameStart = i;
        while (i < tagLen_ && isNameChar(tagBuf_[i])) ++i;
        uint16_t aNameLen = static_cast<uint16_t>(i - aNameStart);

        const char* aVal = nullptr;
        uint16_t aValLen = 0;
        while (i < tagLen_ && isWs(tagBuf_[i])) ++i;
        if (i < tagLen_ && tagBuf_[i] == '=') {
            ++i;
            while (i < tagLen_ && isWs(tagBuf_[i])) ++i;
            if (i < tagLen_ && (tagBuf_[i] == '"' || tagBuf_[i] == '\'')) {
                char q = tagBuf_[i++];
                size_t vs = i;
                while (i < tagLen_ && tagBuf_[i] != q) ++i;
                aVal = &tagBuf_[vs];
                aValLen = static_cast<uint16_t>(i - vs);
                if (i < tagLen_) ++i;  // closing quote
            } else {
                size_t vs = i;
                while (i < tagLen_ && !isWs(tagBuf_[i]) && tagBuf_[i] != '>') ++i;
                aVal = &tagBuf_[vs];
                aValLen = static_cast<uint16_t>(i - vs);
            }
        }
        if (attrs.count < AttrList::kMax) {
            auto& kv = attrs.items[attrs.count++];
            kv.name = &tagBuf_[aNameStart];
            kv.nameLen = aNameLen;
            kv.value = aVal;
            kv.valueLen = aValLen;
        }
    }

    bool voidEl = isVoid(name, nameLen);
    bool raw = ieq(name, nameLen, "script") || ieq(name, nameLen, "style");
    if (!voidEl && !selfClosing) pushTag(name, nameLen);

    sink_.onStartTag(name, nameLen, attrs, selfClosing);

    if (raw && !selfClosing) {
        state_ = State::RawText;
        rawClosePos_ = 0;
        rawNameLen_ = (nameLen < kRawNameMax - 1) ? static_cast<uint8_t>(nameLen) : (kRawNameMax - 1);
        std::memcpy(rawName_, name, rawNameLen_);
        rawName_[rawNameLen_] = '\0';
    }
}

void HtmlTokenizer::pushTag(const char* name, uint16_t len)
{
    if (stackDepth_ >= kStackMax) return;
    uint8_t n = (len < kRawNameMax - 1) ? static_cast<uint8_t>(len) : (kRawNameMax - 1);
    std::memcpy(stack_[stackDepth_], name, n);
    stack_[stackDepth_][n] = '\0';
    stackLen_[stackDepth_] = n;
    stackDepth_++;
}

void HtmlTokenizer::popTag(const char* name, uint16_t len)
{
    // Lenient: pop down to and including the nearest matching open tag.
    for (int d = stackDepth_ - 1; d >= 0; --d) {
        if (stackLen_[d] == len && ieq(name, len, stack_[d])) {
            stackDepth_ = static_cast<uint8_t>(d);
            return;
        }
    }
}

bool HtmlTokenizer::inside(const char* name) const
{
    for (int d = 0; d < stackDepth_; ++d) {
        if (ieq(stack_[d], stackLen_[d], name)) return true;
    }
    return false;
}

}  // namespace cdc::browser
