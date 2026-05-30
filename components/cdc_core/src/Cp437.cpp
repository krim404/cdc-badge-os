/**
 * \file Cp437.cpp
 * \brief Canonical CP437 <-> Unicode/UTF-8 codec implementation.
 */

#include "cdc_core/Cp437.h"

namespace cdc::core::cp437 {

namespace {

/// Unicode codepoint for each CP437 byte 0x80..0xFF (the standard IBM-PC
/// code page 437 upper half). 0x00..0x7F is ASCII and handled separately.
constexpr uint16_t kHigh[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,  // 80
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,  // 88
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,  // 90
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,  // 98
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,  // A0
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,  // A8
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,  // B0
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,  // B8
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,  // C0
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,  // C8
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,  // D0
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,  // D8
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,  // E0
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,  // E8
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,  // F0
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,  // F8
};

}  // namespace

uint32_t toUnicode(uint8_t b)
{
    return (b < 0x80) ? b : kHigh[b - 0x80];
}

uint8_t fromUnicode(uint32_t cp)
{
    if (cp < 0x80) return static_cast<uint8_t>(cp);
    // Glyphs CP437 only carries outside the 0x80..0xFF block, mapped to their
    // closest renderable byte (the built-in font draws these positions).
    switch (cp) {
        case 0x00A6: return 0x7C;  // broken bar -> '|'
        case 0x00A7: return 0x15;  // section sign (control-area glyph)
        case 0x00B6: return 0x14;  // pilcrow (control-area glyph)
        case 0x00E3: return 0x83;  // a-tilde -> a-circ (closest glyph)
        default: break;
    }
    for (int i = 0; i < 128; ++i) {
        if (kHigh[i] == cp) return static_cast<uint8_t>(0x80 + i);
    }
    return 0;
}

std::string fromUtf8(const char* s)
{
    std::string out;
    if (!s) return out;
    const uint8_t* r = reinterpret_cast<const uint8_t*>(s);
    while (*r) {
        uint8_t c = *r;
        uint32_t cp = 0;
        uint8_t cont = 0;
        if ((c & 0x80) == 0) { out.push_back(static_cast<char>(c)); ++r; continue; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; cont = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; cont = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; cont = 3; }
        else { ++r; continue; }
        ++r;
        bool ok = true;
        for (uint8_t i = 0; i < cont; ++i) {
            if ((*r & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (*r & 0x3F);
            ++r;
        }
        if (!ok) continue;
        uint8_t mapped = fromUnicode(cp);
        if (mapped) out.push_back(static_cast<char>(mapped));
    }
    return out;
}

std::string toUtf8(const char* s)
{
    std::string out;
    if (!s) return out;
    for (const uint8_t* r = reinterpret_cast<const uint8_t*>(s); *r; ++r) {
        uint32_t cp = toUnicode(*r);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

}  // namespace cdc::core::cp437
