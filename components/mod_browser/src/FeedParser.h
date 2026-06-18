#pragma once

#include "BrowserExtractTypes.h"

#include <cstddef>
#include <cstdint>

/**
 * \file FeedParser.h
 * \brief Streaming RSS/Atom scanner capturing the first item.
 *
 * Char-driven, no XML library. Handles CDATA sections, RSS `link` as element
 * text vs Atom `link` as an href attribute, and full-text
 * (`content:encoded` / Atom `<content>`) vs summary (`description` / `<summary>`).
 * Item bodies keep their inline HTML/entities; the caller runs them through the
 * HTML text pass afterwards. Host-safe.
 */

namespace cdc::browser {

struct FeedItem {
    char title[kTitleCap] = {0};
    char link[kUrlCap]    = {0};
    bool fullText = false;  ///< true if content:encoded / Atom <content> was present.
};

class FeedParser {
public:
    /// \brief Construct over a caller body buffer that receives the first item's body.
    FeedParser(char* bodyBuf, size_t bodyCap);

    void feed(const char* bytes, size_t len);
    void finish() {}

    /// \brief Copy the first item's fields. \return true if at least one item was seen.
    bool getFirstItem(FeedItem& out) const;
    uint16_t itemCount() const { return itemCount_; }
    size_t bodyLen() const { return bodyLen_; }

private:
    enum class State : uint8_t { Text, Tag, Cdata };
    enum class Field : uint8_t { None, Title, Link, Body };

    void byte(char c);
    void onTag();
    void appendField(char c);

    State  state_ = State::Text;
    Field  field_ = Field::None;

    char   tagBuf_[256];
    size_t tagLen_ = 0;
    char   tagQuote_ = 0;

    uint8_t cdataMatch_ = 0;  // progress matching "![CDATA[" after '<', and "]]>" inside
    uint8_t cdataClose_ = 0;

    bool     capturing_ = false;     // inside the first <item>/<entry>
    bool     firstItemDone_ = false;
    bool     bodyLocked_ = false;    // full-text body captured; ignore summary
    bool     capturingFull_ = false; // current Body field is full-text content
    uint16_t itemCount_ = 0;

    FeedItem item_;
    size_t   titleLen_ = 0;
    size_t   linkLen_ = 0;

    char*  body_;
    size_t bodyCap_;
    size_t bodyLen_ = 0;
};

}  // namespace cdc::browser
