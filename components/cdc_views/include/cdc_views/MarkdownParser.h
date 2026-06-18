#pragma once

#include <cstddef>
#include <cstdint>

#include "cdc_views/MarkdownModel.h"

namespace cdc::ui {

/// \brief Outcome of a parse run.
struct MarkdownParseResult {
    uint16_t lineCount = 0;  ///< Number of styled lines emitted.
    bool truncated = false;  ///< True when the source exceeded `maxBytes`.
};

/**
 * \brief Parses Markdown source into styled logical lines via a sink.
 *
 * Emits one \ref StyledLine per logical line (headings, list items, code, quote,
 * rule, paragraph); pixel wrapping is left to the view. Heading font sizes are
 * assigned dynamically: the least-prominent heading level present gets the
 * smallest heading font, stepping up one size per higher-prominence level, so
 * the number of distinct fonts used equals the number of distinct heading levels
 * (a document using only `#` does not get the largest font). Unsupported
 * constructs degrade to paragraph text.
 *
 * \param src Markdown source (UTF-8/CP437). Must outlive the emitted spans.
 * \param len Source length in bytes.
 * \param sink Receives the styled lines in document order.
 * \param maxBytes Truncation cap; bytes beyond are ignored and `truncated` set.
 * \return Emitted line count and truncation flag.
 */
MarkdownParseResult parseMarkdown(const char* src, size_t len, StyledLineSink& sink, size_t maxBytes);

} // namespace cdc::ui
