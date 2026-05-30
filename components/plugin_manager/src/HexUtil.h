/**
 * \file HexUtil.h
 * \brief Hex-digit decoding shared by the plugin_manager source files.
 */

#pragma once

namespace cdc::plugin_manager {

/**
 * \brief Convert a single hex digit to its numeric value.
 * \param c Hex character ('0'-'9', 'a'-'f', 'A'-'F').
 * \return Value 0-15, or -1 if `c` is not a hex digit.
 */
inline int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace cdc::plugin_manager
