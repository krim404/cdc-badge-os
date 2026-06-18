#pragma once

#include <cstddef>
#include <cstdint>

/**
 * \file HtmlTokenizer.h
 * \brief Streaming, allocation-free HTML tokenizer (no DOM).
 *
 * Fed bytes incrementally via feed(); emits start/end-tag and text events to a
 * TokenSink. Maintains a bounded open-tag stack (used by inside()) and treats
 * the contents of <script>/<style> as raw text that is never interpreted as
 * markup. Memory is constant: state + a fixed tag/attribute scratch + the tag
 * stack. Host-safe (standard headers only).
 */

namespace cdc::browser {

/// \brief Attributes of the current start tag, valid only during onStartTag().
struct AttrList {
    static constexpr int kMax = 24;
    struct KV {
        const char* name = nullptr;   ///< Not NUL-terminated; use nameLen.
        uint16_t    nameLen = 0;
        const char* value = nullptr;  ///< Not NUL-terminated; use valueLen.
        uint16_t    valueLen = 0;
    };
    KV  items[kMax];
    int count = 0;

    /// \brief Case-insensitive lookup. \return true and sets out when found.
    bool get(const char* key, const char** value, uint16_t* len) const;
    /// \brief Copy a found attribute value into \p out (NUL-terminated, truncated).
    bool copy(const char* key, char* out, size_t outCap) const;
};

/// \brief Receives tokenizer events. Tag names are lowercased.
struct TokenSink {
    virtual void onStartTag(const char* name, uint16_t nameLen,
                            const AttrList& attrs, bool selfClosing) = 0;
    virtual void onEndTag(const char* name, uint16_t nameLen) = 0;
    /// \brief A raw text run (UTF-8, HTML entities NOT yet decoded).
    virtual void onText(const char* text, size_t len) = 0;
    virtual ~TokenSink() = default;
};

class HtmlTokenizer {
public:
    explicit HtmlTokenizer(TokenSink& sink);

    /// \brief Push a chunk of bytes; may emit zero or more events.
    void feed(const char* bytes, size_t len);
    /// \brief Flush any trailing buffered text. Call once at end of input.
    void finish();

    /// \brief True if \p name (lowercase) is currently an open ancestor.
    bool inside(const char* name) const;
    /// \brief Current open-tag stack depth.
    uint8_t depth() const { return stackDepth_; }

private:
    enum class State : uint8_t { Text, InTag, Comment, RawText };

    void byte(char c);
    void flushText();
    void emitBufferedTag();
    void pushTag(const char* name, uint16_t len);
    void popTag(const char* name, uint16_t len);

    static constexpr size_t   kTextCap  = 512;
    static constexpr size_t   kTagCap   = 1024;
    static constexpr uint8_t  kStackMax = 32;
    static constexpr uint8_t  kRawNameMax = 16;

    TokenSink& sink_;
    State      state_ = State::Text;

    char     textBuf_[kTextCap];
    size_t   textLen_ = 0;

    char     tagBuf_[kTagCap];
    size_t   tagLen_ = 0;
    char     tagQuote_ = 0;  ///< Active quote char inside a tag, 0 if none.

    // Comment scanning ("-->") progress and "<!--" detection at tag start.
    uint8_t  commentDashes_ = 0;

    // RawText: close-tag matching against rawName_ (e.g. "script").
    char     rawName_[kRawNameMax];
    uint8_t  rawNameLen_ = 0;
    uint8_t  rawClosePos_ = 0;  ///< Match progress against "</rawName_".

    // Open-tag stack: short tag names for inside()/nesting.
    char     stack_[kStackMax][kRawNameMax];
    uint8_t  stackLen_[kStackMax];
    uint8_t  stackDepth_ = 0;
};

}  // namespace cdc::browser
