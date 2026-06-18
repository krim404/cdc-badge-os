#include "FeedParser.h"

#include "BrowserTextUtil.h"

#include <cstring>

namespace cdc::browser {
using namespace util;
namespace {

/// Find attribute \p key in raw tag text; copy its value to \p out (NUL-terminated).
bool getAttr(const char* tag, size_t len, const char* key, char* out, size_t cap)
{
    size_t i = 0;
    while (i < len && !isWs(tag[i]) && tag[i] != '/') ++i;  // skip element name
    while (i < len) {
        while (i < len && isWs(tag[i])) ++i;
        if (i >= len || tag[i] == '/' || tag[i] == '>') break;
        size_t ns = i;
        while (i < len && isNameChar(tag[i])) ++i;
        size_t nlen = i - ns;
        const char* val = nullptr;
        size_t vlen = 0;
        while (i < len && isWs(tag[i])) ++i;
        if (i < len && tag[i] == '=') {
            ++i;
            while (i < len && isWs(tag[i])) ++i;
            if (i < len && (tag[i] == '"' || tag[i] == '\'')) {
                char q = tag[i++];
                size_t vs = i;
                while (i < len && tag[i] != q) ++i;
                val = &tag[vs];
                vlen = i - vs;
                if (i < len) ++i;
            } else {
                size_t vs = i;
                while (i < len && !isWs(tag[i]) && tag[i] != '>') ++i;
                val = &tag[vs];
                vlen = i - vs;
            }
        }
        if (nlen && ieq(tag + ns, nlen, key) && val) {
            size_t n = (vlen < cap - 1) ? vlen : cap - 1;
            std::memcpy(out, val, n);
            out[n] = '\0';
            return true;
        }
        if (i < len && (tag[i] == '/' || tag[i] == '>')) break;
    }
    return false;
}

}  // namespace

FeedParser::FeedParser(char* bodyBuf, size_t bodyCap) : body_(bodyBuf), bodyCap_(bodyCap)
{
    if (body_ && bodyCap_) body_[0] = '\0';
}

void FeedParser::appendField(char c)
{
    if (!capturing_) return;
    switch (field_) {
        case Field::Title:
            if (titleLen_ < sizeof(item_.title) - 1) {
                if (isWs(c)) {
                    if (titleLen_ > 0 && item_.title[titleLen_ - 1] != ' ') item_.title[titleLen_++] = ' ';
                } else {
                    item_.title[titleLen_++] = c;
                }
                item_.title[titleLen_] = '\0';
            }
            break;
        case Field::Link:
            if (!isWs(c) && linkLen_ < sizeof(item_.link) - 1) {
                item_.link[linkLen_++] = c;
                item_.link[linkLen_] = '\0';
            }
            break;
        case Field::Body:
            if (body_ && bodyLen_ < bodyCap_ - 1) {
                body_[bodyLen_++] = c;
                body_[bodyLen_] = '\0';
            }
            break;
        case Field::None:
            break;
    }
}

void FeedParser::feed(const char* bytes, size_t len)
{
    if (!bytes) return;
    for (size_t i = 0; i < len; ++i) byte(bytes[i]);
}

void FeedParser::byte(char c)
{
    switch (state_) {
        case State::Text:
            if (c == '<') {
                state_ = State::Tag;
                tagLen_ = 0;
                tagQuote_ = 0;
            } else {
                appendField(c);
            }
            return;

        case State::Tag:
            if (tagLen_ == 8 && std::memcmp(tagBuf_, "![CDATA[", 8) == 0) {
                state_ = State::Cdata;
                cdataClose_ = 0;
                // current byte c belongs to CDATA content; fall through by re-dispatch
                byte(c);
                return;
            }
            if (tagQuote_) {
                if (c == tagQuote_) tagQuote_ = 0;
                if (tagLen_ < sizeof(tagBuf_)) tagBuf_[tagLen_++] = c;
                return;
            }
            if (c == '"' || c == '\'') {
                tagQuote_ = c;
                if (tagLen_ < sizeof(tagBuf_)) tagBuf_[tagLen_++] = c;
                return;
            }
            if (c == '>') {
                onTag();
                state_ = State::Text;
                tagLen_ = 0;
                return;
            }
            if (tagLen_ < sizeof(tagBuf_)) tagBuf_[tagLen_++] = c;
            return;

        case State::Cdata:
            if (c == ']') {
                if (cdataClose_ < 255) cdataClose_++;
            } else if (c == '>' && cdataClose_ >= 2) {
                for (uint8_t k = 0; k + 2 < cdataClose_; ++k) appendField(']');
                cdataClose_ = 0;
                state_ = State::Text;
            } else {
                for (uint8_t k = 0; k < cdataClose_; ++k) appendField(']');
                cdataClose_ = 0;
                appendField(c);
            }
            return;
    }
}

void FeedParser::onTag()
{
    size_t i = 0;
    while (i < tagLen_ && isWs(tagBuf_[i])) ++i;
    if (i >= tagLen_) return;
    bool endTag = false;
    if (tagBuf_[i] == '/') { endTag = true; ++i; }
    if (i < tagLen_ && (tagBuf_[i] == '!' || tagBuf_[i] == '?')) return;

    char name[40];
    size_t nl = 0;
    while (i < tagLen_ && isNameChar(tagBuf_[i]) && nl < sizeof(name) - 1) {
        name[nl++] = lc(tagBuf_[i++]);
    }
    name[nl] = '\0';
    if (nl == 0) return;

    auto isItem = [&]() { return std::strcmp(name, "item") == 0 || std::strcmp(name, "entry") == 0; };
    auto isBodyElem = [&]() {
        return std::strcmp(name, "description") == 0 || std::strcmp(name, "summary") == 0 ||
               std::strcmp(name, "content") == 0 || std::strcmp(name, "content:encoded") == 0;
    };

    if (endTag) {
        if (isItem()) {
            if (capturing_) { capturing_ = false; firstItemDone_ = true; }
            field_ = Field::None;
            return;
        }
        if (std::strcmp(name, "title") == 0 || std::strcmp(name, "link") == 0 || isBodyElem()) {
            if (isBodyElem() && capturingFull_) { bodyLocked_ = true; capturingFull_ = false; }
            field_ = Field::None;
        }
        return;
    }

    if (isItem()) {
        ++itemCount_;
        if (!firstItemDone_) capturing_ = true;
        return;
    }
    if (!capturing_) return;

    if (std::strcmp(name, "title") == 0) {
        field_ = Field::Title;
    } else if (std::strcmp(name, "link") == 0) {
        char href[kUrlCap];
        if (getAttr(tagBuf_, tagLen_, "href", href, sizeof(href)) && href[0]) {
            if (!item_.link[0]) {
                std::strncpy(item_.link, href, sizeof(item_.link) - 1);
                item_.link[sizeof(item_.link) - 1] = '\0';
                linkLen_ = std::strlen(item_.link);
            }
            field_ = Field::None;
        } else {
            field_ = Field::Link;  // RSS: link is element text
        }
    } else if (std::strcmp(name, "content") == 0 || std::strcmp(name, "content:encoded") == 0) {
        if (!bodyLocked_) {
            field_ = Field::Body;
            item_.fullText = true;
            capturingFull_ = true;
            bodyLen_ = 0;
            if (body_) body_[0] = '\0';
        } else {
            field_ = Field::None;
        }
    } else if (std::strcmp(name, "description") == 0 || std::strcmp(name, "summary") == 0) {
        if (!bodyLocked_ && bodyLen_ == 0) {
            field_ = Field::Body;
            item_.fullText = false;
        } else {
            field_ = Field::None;
        }
    }
}

bool FeedParser::getFirstItem(FeedItem& out) const
{
    if (itemCount_ == 0) return false;
    out = item_;
    // Trim trailing space in title.
    size_t tl = std::strlen(out.title);
    while (tl > 0 && out.title[tl - 1] == ' ') out.title[--tl] = '\0';
    return true;
}

}  // namespace cdc::browser
