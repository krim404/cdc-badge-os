/**
 * RenderHelpers
 *
 * Shared rendering utilities for common UI chrome and dialogs.
 */

#include "cdc_views/RenderHelpers.h"
#include "cdc_core/Cp437.h"
#include <goodisplay/gdey029T94.h>
#include <algorithm>
#include <cstring>

namespace cdc::ui::render {

namespace {
/**
 * \brief Draws CP437 bytes one at a time through write(), bypassing
 *        Epd::print(const std::string&). That overload assumes UTF-8 input and
 *        adds 64 to bytes 0x84..0xBE (e.g. ae 0x84 -> 0xC4 box line, oe 0x94 ->
 *        0xD4), which corrupts our CP437 text. write() routes straight to
 *        Epd::write(uint8_t) -> drawChar with no transform.
 */
void writeRaw(Gdey029T94* gfx, const char* text) {
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p; ++p) {
        gfx->write(*p);
    }
}
}  // namespace

/**
 * \brief Draws a left-aligned header with optional underline.
 * \param gfx Display drawing context.
 * \param title Optional title text.
 * \param x Header text X position.
 * \param y Header text Y position.
 * \param width Width used for underline.
 * \param underlineOffset Vertical offset for underline.
 * \return void
 */
void printTruncated(Gdey029T94* gfx, const char* text, int maxWidthPx) {
    if (!gfx || !text || maxWidthPx <= 0) return;

    int16_t x1, y1;
    uint16_t w, h;
    gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    if (static_cast<int>(w) <= maxWidthPx) {
        writeRaw(gfx, text);
        return;
    }

    constexpr char ELLIPSIS[] = "...";
    uint16_t ew, eh;
    int16_t ex1, ey1;
    gfx->getTextBounds(ELLIPSIS, 0, 0, &ex1, &ey1, &ew, &eh);

    const int budget = maxWidthPx - static_cast<int>(ew);
    if (budget <= 0) {
        writeRaw(gfx, ELLIPSIS);
        return;
    }

    char buf[128];
    size_t len = std::strlen(text);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    std::memcpy(buf, text, len);
    buf[len] = '\0';

    while (len > 0) {
        buf[len] = '\0';
        gfx->getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
        if (static_cast<int>(w) <= budget) break;
        --len;
    }

    writeRaw(gfx, buf);
    writeRaw(gfx, ELLIPSIS);
}

void drawHeaderLeft(Gdey029T94* gfx, const char* title, int x, int y,
                    uint16_t width, int underlineOffset) {
    if (!gfx) return;

    if (title && title[0] != '\0') {
        gfx->setCursor(x, y);
        writeRaw(gfx, title);
    }
    gfx->drawFastHLine(0, y + underlineOffset, width, EPD_BLACK);
}

/**
 * \brief Draws a centered header title.
 * \param gfx Display drawing context.
 * \param title Title text.
 * \param y Header text Y position.
 * \param width Total layout width.
 * \return void
 */
void drawHeaderCentered(Gdey029T94* gfx, const char* title, int y, uint16_t width) {
    if (!gfx || !title || title[0] == '\0') return;

    int16_t x1, y1;
    uint16_t w, h;
    gfx->getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
    gfx->setCursor((width - w) / 2, y);
    writeRaw(gfx, title);
}

/**
 * \brief Draws footer bar with optional prefix and hint text.
 * \param gfx Display drawing context.
 * \param width Display width.
 * \param height Display height.
 * \param prefix Optional prefix text.
 * \param hint Optional hint text.
 * \param force Draw even if prefix/hint are empty.
 * \return void
 */
void drawFooterBar(Gdey029T94* gfx, uint16_t width, uint16_t height,
                   const char* prefix, const char* hint, bool force) {
    if (!gfx) return;
    if (!force && !prefix && !hint) return;

    gfx->fillRect(0, height - FOOTER_HEIGHT, width, FOOTER_HEIGHT, EPD_BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(EPD_WHITE);
    gfx->setCursor(4, height - 12);

    if (prefix) {
        writeRaw(gfx, prefix);
    }
    if (hint) {
        writeRaw(gfx, hint);
    }
}

/**
 * \brief Draws scroll arrows and scrollbar thumb.
 * \param gfx Display drawing context.
 * \param x Indicator X position.
 * \param y Indicator Y position.
 * \param listHeight Height of the scrollable list area.
 * \param totalItems Total item count.
 * \param visibleItems Number of visible items.
 * \param scrollPos Current scroll offset.
 * \return void
 */
void drawScrollIndicator(Gdey029T94* gfx, int x, int y, int listHeight,
                         uint16_t totalItems, uint16_t visibleItems,
                         uint16_t scrollPos) {
    if (!gfx) return;
    if (totalItems <= visibleItems) return;

    gfx->fillRect(x, y, SCROLL_INDICATOR_WIDTH, listHeight, EPD_WHITE);

    const int midX = x + (SCROLL_INDICATOR_WIDTH / 2);
    const int leftX = x + 1;
    const int rightX = x + SCROLL_INDICATOR_WIDTH - 1;

    if (scrollPos > 0) {
        const int topY = y + 4;
        gfx->fillTriangle(
            midX, topY,
            leftX, topY + 6,
            rightX, topY + 6,
            EPD_BLACK
        );
    }

    if (scrollPos + visibleItems < totalItems) {
        const int arrowY = y + listHeight - 12;
        gfx->fillTriangle(
            midX, arrowY + 8,
            leftX, arrowY + 2,
            rightX, arrowY + 2,
            EPD_BLACK
        );
    }

    const int barHeight = listHeight - 24;
    if (barHeight <= 0) return;

    const int barX = x + (SCROLL_INDICATOR_WIDTH / 2) - 2;
    const int barY = y + 12;
    int thumbHeight = std::max(10, barHeight * static_cast<int>(visibleItems) /
                                     static_cast<int>(totalItems));
    int scrollRange = static_cast<int>(totalItems - visibleItems);
    int thumbPos = scrollRange > 0 ? (barHeight - thumbHeight) *
                                     static_cast<int>(scrollPos) / scrollRange
                                   : 0;

    gfx->drawRect(barX, barY, 4, barHeight, EPD_BLACK);
    gfx->fillRect(barX, barY + thumbPos, 4, thumbHeight, EPD_BLACK);
}

/**
 * \brief Draws a framed dialog box with double border.
 * \param gfx Display drawing context.
 * \param x Left position.
 * \param y Top position.
 * \param w Frame width.
 * \param h Frame height.
 * \return void
 */
void drawDialogFrame(Gdey029T94* gfx, int x, int y, int w, int h) {
    if (!gfx) return;

    gfx->fillRect(x, y, w, h, EPD_WHITE);
    gfx->drawRect(x, y, w, h, EPD_BLACK);
    gfx->drawRect(x + 1, y + 1, w - 2, h - 2, EPD_BLACK);
}

/**
 * \brief Maps a CP437 byte to the equivalent Latin-1 byte for use with
 *        Latin-1 indexed GFX fonts (TTF-derived range 0x20..0xFF).
 *        ASCII (<0x80) and untracked codes pass through unchanged.
 */
uint8_t cp437ToLatin1(uint8_t c) {
    switch (c) {
        case 0x80: return 0xC7; case 0x81: return 0xFC;
        case 0x82: return 0xE9; case 0x83: return 0xE2;
        case 0x84: return 0xE4; case 0x85: return 0xE0;
        case 0x86: return 0xE5; case 0x87: return 0xE7;
        case 0x88: return 0xEA; case 0x89: return 0xEB;
        case 0x8A: return 0xE8; case 0x8B: return 0xEF;
        case 0x8C: return 0xEE; case 0x8D: return 0xEC;
        case 0x8E: return 0xC4; case 0x8F: return 0xC5;
        case 0x90: return 0xC9; case 0x91: return 0xE6;
        case 0x92: return 0xC6; case 0x93: return 0xF4;
        case 0x94: return 0xF6; case 0x95: return 0xF2;
        case 0x96: return 0xFB; case 0x97: return 0xF9;
        case 0x98: return 0xFF; case 0x99: return 0xD6;
        case 0x9A: return 0xDC; case 0x9B: return 0xA2;
        case 0x9C: return 0xA3; case 0x9D: return 0xA5;
        case 0xA0: return 0xE1; case 0xA1: return 0xED;
        case 0xA2: return 0xF3; case 0xA3: return 0xFA;
        case 0xA4: return 0xF1; case 0xA5: return 0xD1;
        case 0xA6: return 0xAA; case 0xA7: return 0xBA;
        case 0xA8: return 0xBF; case 0xAA: return 0xAC;
        case 0xAB: return 0xBD; case 0xAC: return 0xBC;
        case 0xAD: return 0xA1; case 0xAE: return 0xAB;
        case 0xAF: return 0xBB; case 0xE1: return 0xDF;
        case 0xE6: return 0xB5; case 0xF1: return 0xB1;
        case 0xF6: return 0xF7; case 0xF8: return 0xB0;
        case 0xFD: return 0xB2;
        default: return c;
    }
}


namespace {

struct NamedEntity { const char* name; uint32_t cp; };

constexpr NamedEntity kNamedEntities[] = {
    {"amp",    '&'},    {"lt",     '<'},    {"gt",     '>'},
    {"quot",   '"'},    {"apos",   '\''},   {"nbsp",   0x00A0},
    {"auml",   0x00E4}, {"Auml",   0x00C4},
    {"ouml",   0x00F6}, {"Ouml",   0x00D6},
    {"uuml",   0x00FC}, {"Uuml",   0x00DC},
    {"szlig",  0x00DF}, {"ssharp", 0x00DF},
    {"aacute", 0x00E1}, {"Aacute", 0x00C1},
    {"eacute", 0x00E9}, {"Eacute", 0x00C9},
    {"iacute", 0x00ED}, {"Iacute", 0x00CD},
    {"oacute", 0x00F3}, {"Oacute", 0x00D3},
    {"uacute", 0x00FA}, {"Uacute", 0x00DA},
    {"agrave", 0x00E0}, {"Agrave", 0x00C0},
    {"egrave", 0x00E8}, {"Egrave", 0x00C8},
    {"igrave", 0x00EC}, {"Igrave", 0x00CC},
    {"ograve", 0x00F2}, {"Ograve", 0x00D2},
    {"ugrave", 0x00F9}, {"Ugrave", 0x00D9},
    {"acirc",  0x00E2}, {"Acirc",  0x00C2},
    {"ecirc",  0x00EA}, {"Ecirc",  0x00CA},
    {"icirc",  0x00EE}, {"Icirc",  0x00CE},
    {"ocirc",  0x00F4}, {"Ocirc",  0x00D4},
    {"ucirc",  0x00FB}, {"Ucirc",  0x00DB},
    {"atilde", 0x00E3}, {"Atilde", 0x00C3},
    {"ntilde", 0x00F1}, {"Ntilde", 0x00D1},
    {"otilde", 0x00F5}, {"Otilde", 0x00D5},
    {"ccedil", 0x00E7}, {"Ccedil", 0x00C7},
    {"aring",  0x00E5}, {"Aring",  0x00C5},
    {"aelig",  0x00E6}, {"AElig",  0x00C6},
    {"oslash", 0x00F8}, {"Oslash", 0x00D8},
    {"yuml",   0x00FF}, {"Yuml",   0x0178},
    {"copy",   0x00A9}, {"reg",    0x00AE}, {"trade",  0x2122},
    {"deg",    0x00B0}, {"plusmn", 0x00B1}, {"para",   0x00B6},
    {"sect",   0x00A7}, {"micro",  0x00B5}, {"middot", 0x00B7},
    {"laquo",  0x00AB}, {"raquo",  0x00BB},
    {"iexcl",  0x00A1}, {"iquest", 0x00BF},
    {"cent",   0x00A2}, {"pound",  0x00A3}, {"yen",    0x00A5},
    {"euro",   0x20AC},
    {"hellip", 0x2026}, {"mdash",  0x2014}, {"ndash",  0x2013},
    {"lsquo",  0x2018}, {"rsquo",  0x2019},
    {"ldquo",  0x201C}, {"rdquo",  0x201D},
    {"bull",   0x2022},
};

void appendUtf8(char*& w, char* end, uint32_t cp) {
    if (cp < 0x80) {
        if (w < end) *w++ = static_cast<char>(cp);
    } else if (cp < 0x800) {
        if (w + 2 <= end) {
            *w++ = static_cast<char>(0xC0 | (cp >> 6));
            *w++ = static_cast<char>(0x80 | (cp & 0x3F));
        }
    } else if (cp < 0x10000) {
        if (w + 3 <= end) {
            *w++ = static_cast<char>(0xE0 | (cp >> 12));
            *w++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            *w++ = static_cast<char>(0x80 | (cp & 0x3F));
        }
    } else if (w + 4 <= end) {
        *w++ = static_cast<char>(0xF0 | (cp >> 18));
        *w++ = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *w++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *w++ = static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool parseEntity(const char* p, uint32_t* cp_out, const char** after) {
    if (*p != '&') return false;
    const char* q = p + 1;
    uint32_t cp = 0;
    if (*q == '#') {
        ++q;
        bool hex = (*q == 'x' || *q == 'X');
        if (hex) ++q;
        const char* digits = q;
        while (*q && *q != ';') {
            int d;
            char c = *q;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            cp = cp * (hex ? 16u : 10u) + static_cast<uint32_t>(d);
            ++q;
        }
        if (*q != ';' || q == digits) return false;
        *cp_out = cp;
        *after  = q + 1;
        return true;
    }
    for (const auto& e : kNamedEntities) {
        size_t nl = std::strlen(e.name);
        if (std::strncmp(q, e.name, nl) == 0 && q[nl] == ';') {
            *cp_out = e.cp;
            *after  = q + nl + 1;
            return true;
        }
    }
    return false;
}

}  // namespace

void utf8ToCp437Inplace(char* buf) {
    if (!buf) return;
    std::string cp437 = cdc::core::cp437::fromUtf8(buf);
    std::memcpy(buf, cp437.c_str(), cp437.size() + 1);
}

namespace {

void utf8ToLatin1Inplace(char* buf) {
    if (!buf) return;
    uint8_t* r = reinterpret_cast<uint8_t*>(buf);
    uint8_t* w = r;
    while (*r) {
        uint8_t c = *r;
        uint32_t cp = 0;
        uint8_t cont = 0;
        if ((c & 0x80) == 0) { *w++ = c; ++r; continue; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; cont = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; cont = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; cont = 3; }
        else { ++r; continue; }
        ++r;
        bool ok = true;
        for (uint8_t i = 0; i < cont; i++) {
            if ((*r & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (*r & 0x3F);
            ++r;
        }
        if (!ok) continue;
        if (cp < 0x100) {
            *w++ = static_cast<uint8_t>(cp);
        }
    }
    *w = '\0';
}

}  // namespace

void decodeWebText(const char* in, char* out, size_t out_size,
                   DisplayTarget target) {
    if (!in || !out || out_size == 0) return;
    char* w = out;
    char* end = out + out_size - 1;
    const char* r = in;
    while (*r && w < end) {
        if (*r == '&') {
            uint32_t cp;
            const char* after;
            if (parseEntity(r, &cp, &after)) {
                appendUtf8(w, end, cp);
                r = after;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
    if (target == DisplayTarget::Latin1) {
        utf8ToLatin1Inplace(out);
    } else {
        utf8ToCp437Inplace(out);
    }
}

void drawCp437Text(Gdey029T94* gfx, const char* text) {
    if (!gfx || !text) return;
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p; ++p) {
        gfx->write(cp437ToLatin1(*p));
    }
}

void drawText(Gdey029T94* gfx, const char* text, const GFXfont* font) {
    if (!gfx || !text) return;
    // Make the active font match the encoding chosen below: the built-in
    // glcdfont (font == nullptr) is CP437-indexed and gets raw bytes; Latin-1
    // GFX fonts get CP437->Latin1 mapping. Setting it here prevents a stale
    // font from mismatching the bytes we emit.
    gfx->setFont(font);
    if (!font) {
        writeRaw(gfx, text);       // built-in glcdfont: raw CP437 bytes
    } else {
        drawCp437Text(gfx, text);  // Latin-1 GFX font: CP437->Latin1 per byte
    }
}

void printText(Gdey029T94* gfx, const char* text) {
    if (!gfx || !text) return;
    gfx->setFont(nullptr);  // built-in glcdfont (CP437-indexed)
    writeRaw(gfx, text);
}

void measureText(Gdey029T94* gfx, const char* text, const GFXfont* font,
                 int16_t x0, int16_t y0, int16_t* x1, int16_t* y1,
                 uint16_t* w, uint16_t* h) {
    if (!gfx || !text) {
        if (x1) *x1 = x0;
        if (y1) *y1 = y0;
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    gfx->setFont(font);
    if (!font) {
        gfx->getTextBounds(text, x0, y0, x1, y1, w, h);
    } else {
        measureCp437Text(gfx, text, x0, y0, x1, y1, w, h);
    }
}

const GFXfont* pickFontThatFits(Gdey029T94* gfx,
                                const char* text,
                                int maxWidthPx,
                                const GFXfont* const* candidates,
                                size_t count,
                                bool cp437) {
    if (count == 0) return nullptr;
    if (!gfx || !text || !candidates || maxWidthPx <= 0) {
        return candidates[count - 1];
    }

    const GFXfont* selected = candidates[count - 1];
    for (size_t i = 0; i < count; ++i) {
        const GFXfont* f = candidates[i];
        gfx->setFont(f);
        gfx->setTextSize(1);
        int16_t x1, y1;
        uint16_t w = 0, h = 0;
        if (cp437) {
            measureCp437Text(gfx, text, 0, 0, &x1, &y1, &w, &h);
        } else {
            gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
        }
        if (static_cast<int>(w) <= maxWidthPx) {
            selected = f;
            break;
        }
    }
    gfx->setFont(selected);
    gfx->setTextSize(1);
    return selected;
}

void measureCp437Text(Gdey029T94* gfx, const char* text, int16_t x0, int16_t y0,
                      int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h) {
    if (!gfx || !text) {
        if (x1) *x1 = x0;
        if (y1) *y1 = y0;
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    char buf[128];
    size_t i = 0;
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p && i + 1 < sizeof(buf); ++p) {
        buf[i++] = static_cast<char>(cp437ToLatin1(*p));
    }
    buf[i] = '\0';
    gfx->getTextBounds(buf, x0, y0, x1, y1, w, h);
}

} // namespace cdc::ui::render
