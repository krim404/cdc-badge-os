#pragma once

#include <cstddef>
#include <cstdint>

#include <gfxfont.h>

#include "cdc_views/LayoutConstants.h"

class Adafruit_GFX;
class Gdey029T94;

namespace cdc::ui::render {

// Backwards-compatible aliases for the shared layout constants.
// Prefer the canonical names in cdc::ui::layout for new code.
constexpr int FOOTER_HEIGHT = cdc::ui::layout::FOOTER_HEIGHT;
constexpr int SCROLL_INDICATOR_WIDTH = cdc::ui::layout::SCROLL_INDICATOR_WIDTH;

void drawHeaderLeft(Gdey029T94* gfx, const char* title, int x, int y,
                    uint16_t width, int underlineOffset = 18);
void drawHeaderCentered(Gdey029T94* gfx, const char* title, int y, uint16_t width);

void drawFooterBar(Gdey029T94* gfx, uint16_t width, uint16_t height,
                   const char* prefix, const char* hint, bool force = false);

void drawScrollIndicator(Gdey029T94* gfx, int x, int y, int listHeight,
                         uint16_t totalItems, uint16_t visibleItems,
                         uint16_t scrollPos);

void drawDialogFrame(Gdey029T94* gfx, int x, int y, int w, int h);

/**
 * \brief Print `text` at the current cursor, truncated with an ellipsis to fit
 *        `maxWidthPx`. Caller must have already called `setCursor` and
 *        `setTextColor`/`setTextSize`. Adafruit-GFX text wrap should be off.
 * \param gfx Display drawing context.
 * \param text Null-terminated text.
 * \param maxWidthPx Maximum width in pixels.
 */
void printTruncated(Adafruit_GFX* gfx, const char* text, int maxWidthPx);

/**
 * \brief Maps a CP437 byte to the equivalent Latin-1 byte for use with
 *        Unicode/Latin-1 indexed GFX fonts (e.g. FreeMonoBold*pt8b).
 * \param c CP437 byte value.
 * \return Latin-1 byte representing the same character, or `c` when no mapping is needed.
 */
uint8_t cp437ToLatin1(uint8_t c);

/**
 * \brief Decodes a UTF-8 string in place to CP437 single bytes. Truncates if
 *        the buffer is too small. Unmapped codepoints are dropped silently.
 * \param buf Mutable null-terminated UTF-8 buffer; written back as CP437.
 */
void utf8ToCp437Inplace(char* buf);

/// Display encoding targets for decodeWebText().
enum class DisplayTarget : uint8_t {
    Cp437  = 0,  ///< GFX builtin glcdfont (default after `setFont(nullptr)`).
    Latin1 = 1,  ///< FreeMonoBold*pt8b fonts (Latin-1 indexed, 0x20..0xFF).
};

/**
 * \brief Normalises a web payload for display: HTML named/numeric entities
 *        decode first, then UTF-8 multibyte sequences collapse into the
 *        single-byte layout expected by the target font.
 *
 * \p target picks the output codepage:
 *   - \ref DisplayTarget::Cp437 - matches the GFX builtin glcdfont.
 *   - \ref DisplayTarget::Latin1 - matches the FreeMonoBold*pt8b fonts.
 *
 * Output is always NUL-terminated. Unmapped codepoints are dropped.
 * `in` and `out` must not alias.
 *
 * \param in       Source string (UTF-8, optionally containing HTML entities).
 * \param out      Destination buffer.
 * \param out_size Capacity of `out` in bytes (including the terminator).
 * \param target   Encoding target. Defaults to CP437.
 */
void decodeWebText(const char* in, char* out, size_t out_size,
                   DisplayTarget target = DisplayTarget::Cp437);

/**
 * \brief Prints a CP437 string by mapping each byte to Latin-1 before drawing.
 *        Use with TTF-derived GFX fonts (range 0x20..0xFF) that expect Latin-1 indices.
 * \param gfx Target display.
 * \param text CP437-encoded null-terminated string.
 */
void drawCp437Text(Adafruit_GFX* gfx, const char* text);

/**
 * \brief Draws CP437-encoded text correctly for the given font: the built-in
 *        glcdfont (`font == nullptr`) is CP437-indexed and receives the bytes
 *        raw; TTF-derived GFX fonts are Latin-1-indexed and receive a per-byte
 *        CP437->Latin1 mapping. This is the single point where the in-memory
 *        CP437 canonical form is adapted to the active font's index space.
 *        The caller must have set \p font on \p gfx and positioned the cursor.
 * \param gfx Target display.
 * \param text CP437-encoded null-terminated string.
 * \param font Active font (`nullptr` for the built-in glcdfont).
 */
void drawText(Adafruit_GFX* gfx, const char* text, const GFXfont* font);

/**
 * \brief Draws CP437 text with the built-in 6x8 glyph font, byte-for-byte.
 *
 * Use this instead of `gfx->print(const char*)` for any user/i18n string: the
 * CalEPD `Epd::print(const std::string&)` overload assumes UTF-8 and adds 64 to
 * bytes 0x84..0xBE, corrupting CP437 umlauts (ae 0x84 -> 0xC4 etc.). This forces
 * the built-in font and writes each byte straight through, bypassing that.
 * \param gfx Display drawing context.
 * \param text CP437-encoded null-terminated string.
 */
void printText(Adafruit_GFX* gfx, const char* text);

/**
 * \brief Measures CP437 text exactly as \ref drawText would render it with
 *        \p font, so width-based layout (centering, fitting, truncation)
 *        matches the drawn glyphs.
 * \param gfx Target display.
 * \param text CP437-encoded null-terminated string.
 * \param font Active font (`nullptr` for the built-in glcdfont).
 * \param x0 Starting x for measurement.
 * \param y0 Starting y for measurement.
 * \param x1 Output: top-left x of the rendered bounds.
 * \param y1 Output: top-left y of the rendered bounds.
 * \param w  Output: rendered width.
 * \param h  Output: rendered height.
 */
void measureText(Adafruit_GFX* gfx, const char* text, const GFXfont* font,
                 int16_t x0, int16_t y0, int16_t* x1, int16_t* y1,
                 uint16_t* w, uint16_t* h);

/**
 * \brief Picks the largest font from \p candidates whose rendered width of
 *        \p text fits within \p maxWidthPx. Candidates are evaluated in array
 *        order; pass them sorted from largest to smallest so the first match
 *        is the biggest font that still fits.
 * \param gfx Target display. Its current font is overwritten while measuring
 *        and restored to the selected font on return.
 * \param text Null-terminated text to measure.
 * \param maxWidthPx Pixel budget that the rendered text must stay below.
 * \param candidates Array of GFX font pointers. `nullptr` entries select the
 *        built-in 6x8 font.
 * \param count Number of entries in \p candidates.
 * \param cp437 If true, measure via measureCp437Text() (CP437->Latin1 mapping);
 *        otherwise use Adafruit-GFX getTextBounds() directly.
 * \return Selected font pointer. Falls back to the last (smallest) candidate
 *         when nothing fits; returns nullptr if the array is empty.
 */
const GFXfont* pickFontThatFits(Adafruit_GFX* gfx,
                                const char* text,
                                int maxWidthPx,
                                const GFXfont* const* candidates,
                                size_t count,
                                bool cp437 = false);

/**
 * \brief Measures a CP437 string using the current font (via Latin-1 mapping).
 * \param gfx Target display.
 * \param text CP437-encoded null-terminated string.
 * \param x0 Starting x position for measurement.
 * \param y0 Starting y position for measurement.
 * \param x1 Output: top-left x of the rendered bounds.
 * \param y1 Output: top-left y of the rendered bounds.
 * \param w  Output: width of the rendered text.
 * \param h  Output: height of the rendered text.
 */
void measureCp437Text(Adafruit_GFX* gfx, const char* text, int16_t x0, int16_t y0,
                      int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h);

/**
 * \brief True when the ordered-dither matrix inks pixel (x, y) for `shade`.
 * \param x Pixel column.
 * \param y Pixel row.
 * \param shade Grey level 0 (white) .. 255 (black), quantised to 64 levels.
 */
bool ditherOn(int16_t x, int16_t y, uint8_t shade);

/**
 * \brief Fills a rectangle with an ordered-dither fake-grey pattern.
 * \param gfx Target drawing context.
 * \param color Ink color for dithered-on pixels.
 */
void fillRectDither(Adafruit_GFX* gfx, int16_t x, int16_t y, int16_t w, int16_t h,
                    uint8_t shade, uint16_t color);

/// \brief Dithered filled circle; see \ref fillRectDither.
void fillCircleDither(Adafruit_GFX* gfx, int16_t cx, int16_t cy, int16_t r,
                      uint8_t shade, uint16_t color);

/// \brief Dithered filled triangle; see \ref fillRectDither.
void fillTriangleDither(Adafruit_GFX* gfx, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                        int16_t x2, int16_t y2, uint8_t shade, uint16_t color);

/// Options for \ref drawBitmapMasked. Zero-init = plain transparent blit.
struct BlitOpts {
    const uint8_t* mask = nullptr;  ///< Mask plane (same layout) or null.
    bool     opaque  = false;  ///< Without a mask: unset bits paint bg.
    bool     flipH   = false;
    bool     flipV   = false;
    bool     rot90   = false;  ///< 90 deg clockwise (before flips); output
                               ///< box becomes h x w.
    uint8_t  scale   = 1;      ///< Integer upscale 1..4.
    uint16_t srcX    = 0;      ///< Horizontal source window start.
    uint16_t srcW    = 0;      ///< Window width; 0 = full width. Ignores
                               ///< rot90/flips (marquee path).
    uint16_t srcSpan = 0;      ///< Wrap period >= bitmap width (content +
                               ///< blank gap); 0 = no wrap-around.
};

/**
 * \brief Blit a packed 1-bpp bitmap with optional mask plane, flipping,
 *        90-degree rotation, integer scaling and a horizontal source window.
 *
 * Rows are byte-padded, MSB first (the surface/QR/image convention). Without
 * a mask, set bits paint \p fg and unset bits are transparent unless
 * `opts.opaque`, in which case they paint \p bg. With a mask, only pixels
 * whose mask bit is set are painted (data bit picks \p fg / \p bg), which
 * allows drawing white pixels without making the whole rectangle opaque.
 */
void drawBitmapMasked(Adafruit_GFX* gfx, int16_t x, int16_t y,
                      const uint8_t* data, int16_t w, int16_t h,
                      const BlitOpts& opts, uint16_t fg, uint16_t bg);

} // namespace cdc::ui::render
