#pragma once

#include <cstddef>
#include <cstdint>

/**
 * \file BrowserTextUtil.h
 * \brief Tiny ASCII text helpers shared by the browser's pure-logic units.
 *
 * Inline + header-guarded so the tokenizer, extractor, and feed parser can all
 * use them without duplicate-symbol clashes when host tests include several
 * translation units together. Host-safe (no locale, no ESP/GFX).
 */

namespace cdc::browser::util {

inline char lc(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

inline bool isWs(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

inline bool isNameChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == ':' || c == '_';
}

/// \brief Case-insensitive compare of \p aLen chars of \p a against NUL-terminated \p b.
inline bool ieq(const char* a, size_t aLen, const char* b)
{
    for (size_t i = 0; i < aLen; ++i) {
        if (!b[i] || lc(a[i]) != lc(b[i])) return false;
    }
    return b[aLen] == '\0';
}

}  // namespace cdc::browser::util
