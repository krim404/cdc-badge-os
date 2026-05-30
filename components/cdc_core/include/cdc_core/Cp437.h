/**
 * \file Cp437.h
 * \brief Canonical CP437 <-> Unicode/UTF-8 codec.
 *
 * The badge display font is CP437 (IBM-PC code page 437); all other text on
 * the system (language files, plugin strings, file contents) is UTF-8. The C
 * library and ESP-IDF have no CP437 codec, so the conversion is provided here.
 */

#pragma once

#include <cstdint>
#include <string>

namespace cdc::core::cp437 {

/// \brief Map a CP437 byte to its Unicode codepoint. 0x00-0x7F is ASCII.
uint32_t toUnicode(uint8_t b);

/// \brief Map a Unicode codepoint to its CP437 byte, or 0 if it has none.
uint8_t fromUnicode(uint32_t cp);

/// \brief Convert a UTF-8 string to CP437 bytes (unmapped chars dropped).
std::string fromUtf8(const char* s);

/// \brief Convert CP437 bytes to a UTF-8 string.
std::string toUtf8(const char* s);

}  // namespace cdc::core::cp437
