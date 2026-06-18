#pragma once

#include <cstddef>
#include <cstdint>

/**
 * \file BrowserExtractTypes.h
 * \brief Host-safe POD types shared by the browser's pure-logic engine.
 *
 * These structures intentionally use only fixed-capacity char buffers and the
 * standard headers so the tokenizer, extractor, feed parser, and source
 * selector all compile and run under the native host-test environment (no
 * ESP-IDF / GFX dependencies). The hardware layer owns PSRAM allocation and
 * CP437 conversion separately.
 */

namespace cdc::browser {

constexpr size_t   kUrlCap   = 512;  ///< Max URL length kept anywhere.
constexpr size_t   kTitleCap = 128;  ///< Max title length.
constexpr size_t   kDescCap  = 256;  ///< Max meta/OG description length.
constexpr size_t   kLabelCap = 80;   ///< Max link label length.
constexpr uint16_t kMaxLinks = 128;  ///< Hard cap on collected links.

/// \brief One collected hyperlink (label + resolved-absolute target).
struct LinkRef {
    uint16_t index = 0;       ///< Number rendered inline as [index] and in the list.
    char     label[kLabelCap] = {0};
    char     href[kUrlCap]    = {0};
};

constexpr uint8_t kMaxForms       = 3;    ///< Max forms captured per page.
constexpr uint8_t kMaxFormFields  = 24;   ///< Max fields captured per form.
constexpr size_t  kFieldNameCap   = 64;   ///< Max form-field name length.
constexpr size_t  kFieldValueCap  = 256;  ///< Max form-field value length.

/// \brief One submittable form field (only the kinds the login UI handles).
struct FormField {
    enum class Kind : uint8_t { Hidden, Checkbox, Submit };
    Kind kind = Kind::Hidden;
    char name[kFieldNameCap]   = {0};
    char value[kFieldValueCap] = {0};
    bool checked = false;  ///< For Checkbox: initial checked state / user toggle.
};

/// \brief A captured HTML form reduced to its submittable fields.
struct FormSpec {
    char      action[kUrlCap] = {0};  ///< Resolved-absolute submit URL.
    bool      post = false;           ///< true = POST, false = GET.
    FormField fields[kMaxFormFields];
    uint8_t   fieldCount = 0;
};

/// \brief Signals captured while scanning the document <head>.
struct HeadSignals {
    char title[kTitleCap]            = {0};
    char ogTitle[kTitleCap]          = {0};
    char ogDescription[kDescCap]     = {0};
    char metaDescription[kDescCap]   = {0};
    char canonical[kUrlCap]          = {0};
    char amphtml[kUrlCap]            = {0};
    char feedUrl[kUrlCap]            = {0};

    void clear()
    {
        title[0] = ogTitle[0] = ogDescription[0] = metaDescription[0] = 0;
        canonical[0] = amphtml[0] = feedUrl[0] = 0;
    }
};

/// \brief Tier that produced the rendered content.
enum class SourceKind : uint8_t {
    MainExtract,  ///< Heuristic body extraction from the main page.
    Feed,         ///< RSS/Atom feed item.
    Amp,          ///< AMP variant page.
    Mirror,       ///< Static text-mirror host.
    OgCard,       ///< Open Graph title + description fallback card.
    RawText,      ///< text/plain response shown as-is.
    Empty,        ///< Nothing readable extracted.
};

/// \brief Tunable thresholds for the jusText-lite extractor.
struct ExtractConfig {
    size_t   maxSourceBytes = 64u * 1024u;    ///< Output (MarkdownView source) cap.
    size_t   maxScanBytes   = 512u * 1024u;   ///< Raw input scanned before truncation.
    uint16_t maxLinks       = kMaxLinks;      ///< Link-table cap.
    double   maxLinkDensity = 0.20;           ///< Block link-char / text-char gate.
    uint16_t lengthLow      = 70;             ///< Below: short block (needs context).
    uint16_t lengthHigh     = 200;            ///< At/above with low links: keep.
    bool     keepAll        = false;          ///< Keep every non-empty block (full-page mode).
};

}  // namespace cdc::browser
