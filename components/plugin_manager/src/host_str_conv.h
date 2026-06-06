/**
 * \file host_str_conv.h
 * \brief Internal UTF-8 <-> CP437 helpers for the plugin host API boundary.
 *
 * Plugins speak UTF-8 across the whole host API. The display pipeline
 * (cdc_views, render::printText/drawText) is canonical CP437. plugin_manager
 * is the single conversion boundary: inbound text is decoded to CP437 here,
 * outbound text is re-encoded to UTF-8 here. These helpers are not exported to
 * the WASM runtime.
 */

#pragma once

#include <cstddef>
#include <string>

namespace cdc::plugin_manager {

/**
 * \brief Decode a UTF-8 (with optional HTML entities) string into CP437 bytes.
 *
 * Wraps cdc::ui::render::decodeWebText with DisplayTarget::Cp437. The CP437
 * output is always <= the UTF-8 input length, so a strlen-sized scratch buffer
 * suffices.
 * \param utf8 Source UTF-8 string (may be nullptr).
 * \return CP437-encoded string ("" for nullptr/empty input).
 */
std::string toDisplay(const char* utf8);

/**
 * \brief Encode a CP437 string into a caller buffer as UTF-8.
 *
 * Truncates on a UTF-8 codepoint boundary so the result never ends mid-sequence,
 * and always null-terminates.
 * \param cp437 Source CP437 string (may be nullptr).
 * \param out Destination buffer.
 * \param out_size Capacity of \p out including the terminator.
 * \return Number of bytes written (excluding terminator), or HOST_ERR_INVALID_ARG.
 */
int copyUtf8(const char* cp437, char* out, size_t out_size);

}  // namespace cdc::plugin_manager
