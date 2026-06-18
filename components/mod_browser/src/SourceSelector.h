#pragma once

#include "BrowserExtractTypes.h"

/**
 * \file SourceSelector.h
 * \brief Chooses the lightest content source for a fetched page.
 */

namespace cdc::browser {

/// \brief Chosen content source.
enum class Source : uint8_t {
    TextMirror,   ///< Known text-only mirror host.
    Feed,         ///< Advertised RSS/Atom feed.
    Amp,          ///< Advertised AMP variant.
    MainExtract,  ///< Extract from the main page (after canonical).
};

struct SourceDecision {
    Source source = Source::MainExtract;
    char   fetchUrl[kUrlCap] = {0};  ///< URL to fetch for the chosen source.
};

/**
 * \brief If \p requestUrl's host is a known text-only mirror, write the rewritten
 *        URL into \p out and return true.
 */
bool textMirrorFor(const char* requestUrl, char* out, size_t cap);

/**
 * \brief Decide the content source given the request URL and captured head signals.
 *
 * Order: text mirror -> feed -> AMP -> main-page extraction (preferring the
 * canonical URL when advertised).
 */
SourceDecision selectSource(const char* requestUrl, const HeadSignals& head);

}  // namespace cdc::browser
