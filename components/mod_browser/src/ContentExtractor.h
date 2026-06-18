#pragma once

#include "BrowserExtractTypes.h"
#include "HtmlTokenizer.h"

/**
 * \file ContentExtractor.h
 * \brief jusText-lite main-content extractor and head-signal capture.
 *
 * A TokenSink driven by HtmlTokenizer. In one streaming pass it (a) captures
 * head signals (title, OG, description, canonical, amphtml, feed) and (b) builds
 * a Markdown-ish body source with inline [n] link markers plus a parallel link
 * table, keeping prose blocks and dropping navigation/boilerplate by text/link
 * density. Output text is UTF-8 with entities intact; the hardware layer decodes
 * entities and converts to CP437 before display. Host-safe.
 */

namespace cdc::browser {

class ContentExtractor : public TokenSink {
public:
    /**
     * \brief Construct over caller-owned output and link buffers.
     * \param cfg Extraction thresholds and caps.
     * \param baseUrl Page URL used to resolve relative links (NUL-terminated).
     * \param outBuf Destination for the Markdown-ish source (NUL-terminated on finalize).
     * \param outCap Capacity of \p outBuf in bytes.
     * \param links Caller link table.
     * \param linksCap Capacity of \p links.
     */
    ContentExtractor(const ExtractConfig& cfg, const char* baseUrl,
                     char* outBuf, size_t outCap, LinkRef* links, uint16_t linksCap,
                     FormSpec* forms = nullptr, uint8_t formsCap = 0);

    /// \brief Wire the owning tokenizer (for ancestor queries). Call before feeding.
    void setTokenizer(const HtmlTokenizer* t) { tok_ = t; }

    void onStartTag(const char* name, uint16_t nameLen, const AttrList& attrs,
                    bool selfClosing) override;
    void onEndTag(const char* name, uint16_t nameLen) override;
    void onText(const char* text, size_t len) override;

    /// \brief Flush the pending block and apply OG-card / empty fallbacks.
    void finalize();

    const HeadSignals& head() const { return head_; }
    bool       headComplete() const { return headDone_; }
    size_t     sourceLen() const { return outLen_; }
    uint16_t   linkCount() const { return linkCount_; }
    bool       truncated() const { return truncated_; }
    SourceKind kind() const { return kind_; }
    uint8_t          formCount() const { return formCount_; }
    const FormSpec*  forms() const { return forms_; }

private:
    enum class Block : uint8_t { Paragraph, Heading, Bullet, Quote, Code };

    void startBlock(Block kind, uint8_t headingLevel);
    void finishBlock();
    void blockPut(char c);
    void blockPutStr(const char* s);
    bool outPut(const char* s, size_t n);
    bool outPutStr(const char* s);

    const ExtractConfig cfg_;
    const char*         baseUrl_;
    char*               out_;
    size_t              outCap_;
    size_t              outLen_ = 0;
    LinkRef*            links_;
    uint16_t            linksCap_;
    uint16_t            linkCount_ = 0;
    FormSpec*           forms_ = nullptr;
    uint8_t             formsCap_ = 0;
    uint8_t             formCount_ = 0;
    int                 curFormIdx_ = -1;  // index into forms_ while inside a <form>

    const HtmlTokenizer* tok_ = nullptr;
    HeadSignals          head_;
    bool                 headDone_ = false;
    bool                 truncated_ = false;
    SourceKind           kind_ = SourceKind::MainExtract;
    size_t               keptTextChars_ = 0;

    // Title capture.
    bool capTitle_ = false;

    // Current block accumulation.
    static constexpr size_t kBlockCap = 8192;
    char     block_[kBlockCap];
    size_t   blockLen_ = 0;
    Block    blockKind_ = Block::Paragraph;
    uint8_t  headingLevel_ = 0;
    uint32_t blockTextChars_ = 0;
    uint32_t blockLinkChars_ = 0;
    bool     lastWasSpace_ = true;  // collapse leading/inner whitespace

    // Anchor tracking.
    uint8_t  anchorDepth_ = 0;
    size_t   linkLabelStart_ = 0;   // block_ offset where current anchor text began
    int      pendingLinkIdx_ = -1;  // index of LinkRef awaiting its label
    uint16_t blockLinkStart_ = 0;   // linkCount_ at the current block's start
};

}  // namespace cdc::browser
