/**
 * \file CanvasView.cpp
 * \brief Plugin-side custom canvas: plugin draws into the body, host owns the
 *        input state for inline widgets (slider, text-input, button).
 */

#include "cdc_views/CanvasView.h"
#include "cdc_views/Fonts.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/LayoutConstants.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_hal/IKeypad.h"
#include "cdc_log.h"
#include "esp_timer.h"
#include <goodisplay/gdey029T94.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace cdc::ui {

namespace {

constexpr int TITLE_Y = 5;
constexpr int HEADER_HEIGHT_DEFAULT = 30;
constexpr uint16_t BLOB_NONE = 0xFFFF;  // sentinel: bitmap not stored

const char* t9_chars(char key) {
    switch (key) {
        case '0': return " 0";
        case '1': return ".?!,;:'\"()-_@#$%&*+=/\\<>[]{}|^~`1";
        case '2': return "abc2";
        case '3': return "def3";
        case '4': return "ghi4";
        case '5': return "jkl5";
        case '6': return "mno6";
        case '7': return "pqrs7";
        case '8': return "tuv8";
        case '9': return "wxyz9";
        default:  return nullptr;
    }
}

uint32_t nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// Ordered-dither threshold matrix (8x8 Bayer, 64 levels) used to fake grey
// fills on the 1-bpp panel.
constexpr uint8_t kBayer8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 },
};

inline bool ditherOn(int16_t x, int16_t y, uint8_t shade) {
    uint8_t level = static_cast<uint8_t>(shade >> 2);  // 0..63 ink threshold
    return level > kBayer8[y & 7][x & 7];
}

void fillRectDither(Gdey029T94* g, int16_t x, int16_t y, int16_t w, int16_t h,
                    uint8_t shade) {
    for (int16_t yy = 0; yy < h; ++yy) {
        for (int16_t xx = 0; xx < w; ++xx) {
            if (ditherOn(x + xx, y + yy, shade)) g->drawPixel(x + xx, y + yy, EPD_BLACK);
        }
    }
}

void fillCircleDither(Gdey029T94* g, int16_t cx, int16_t cy, int16_t r, uint8_t shade) {
    int32_t r2 = static_cast<int32_t>(r) * r;
    for (int16_t dy = -r; dy <= r; ++dy) {
        for (int16_t dx = -r; dx <= r; ++dx) {
            if (static_cast<int32_t>(dx) * dx + static_cast<int32_t>(dy) * dy <= r2 &&
                ditherOn(cx + dx, cy + dy, shade)) {
                g->drawPixel(cx + dx, cy + dy, EPD_BLACK);
            }
        }
    }
}

void fillTriangleDither(Gdey029T94* g, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                        int16_t x2, int16_t y2, uint8_t shade) {
    if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }
    if (y1 > y2) { std::swap(y1, y2); std::swap(x1, x2); }
    if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }
    if (y2 == y0) {
        int16_t a = std::min(x0, std::min(x1, x2));
        int16_t b = std::max(x0, std::max(x1, x2));
        for (int16_t xx = a; xx <= b; ++xx)
            if (ditherOn(xx, y0, shade)) g->drawPixel(xx, y0, EPD_BLACK);
        return;
    }
    for (int16_t y = y0; y <= y2; ++y) {
        bool upper = (y < y1);
        int32_t xa = x0 + static_cast<int32_t>(x2 - x0) * (y - y0) / (y2 - y0);
        int32_t xb = upper
            ? ((y1 == y0) ? x1 : x0 + static_cast<int32_t>(x1 - x0) * (y - y0) / (y1 - y0))
            : ((y2 == y1) ? x2 : x1 + static_cast<int32_t>(x2 - x1) * (y - y1) / (y2 - y1));
        int16_t left = static_cast<int16_t>(xa < xb ? xa : xb);
        int16_t right = static_cast<int16_t>(xa < xb ? xb : xa);
        for (int16_t xx = left; xx <= right; ++xx)
            if (ditherOn(xx, y, shade)) g->drawPixel(xx, y, EPD_BLACK);
    }
}

}  // namespace

Gdey029T94* CanvasView::gfx() const {
    auto* display = hal::getDisplayInstance();
    if (!display) return nullptr;
    return static_cast<Gdey029T94*>(display->getNativeHandle());
}

int CanvasView::bodyBottom() const {
    auto* display = hal::getDisplayInstance();
    return display ? (display->getHeight() - cdc::ui::layout::FOOTER_HEIGHT) : 128;
}

void CanvasView::init(const char* title) {
    title_ = title;
    allocArenas();
    widgetCount_ = 0;
    focused_ = 0;
    textSize_ = 1;
    textInverted_ = false;
    keyCb_ = nullptr;
    widgetCb_ = nullptr;
    longPressCb_ = nullptr;
    footer_ = nullptr;
    headerHeight_ = (title && title[0] != '\0') ? HEADER_HEIGHT_DEFAULT : 0;
    needsFullRefresh_ = true;
    keyRepeatInitialMs_ = 0;
    keyRepeatPeriodMs_ = 0;
    headerDrawnOnce_ = false;
    customFooter_ = nullptr;
    cmdCount_ = 0;
    textArenaUsed_ = 0;
    overflowLogged_ = false;
    fontId_ = 0;
    shade_ = 255;
    dirty_ = true;
}

void CanvasView::setFooter(const char* hint) {
    footer_ = hint;
    customFooter_ = hint;
}

void CanvasView::setKeyRepeat(uint16_t initial_ms, uint16_t repeat_ms) {
    keyRepeatInitialMs_ = initial_ms;
    keyRepeatPeriodMs_ = repeat_ms;
    applyKeypadConfig();
}

void CanvasView::getBodySize(uint16_t* w, uint16_t* h) const {
    auto* display = hal::getDisplayInstance();
    if (!display) {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    if (w) *w = display->getWidth();
    if (h) *h = static_cast<uint16_t>(bodyBottom() - bodyTop());
}

void CanvasView::clearBody() {
    cmdCount_ = 0;
    textArenaUsed_ = 0;
    blobUsed_ = 0;
}

void CanvasView::allocArenas() {
    if (cmds_ && textArena_ && widgets_ && blobArena_) {
        memset(cmds_.get(), 0, sizeof(DrawCmd) * MAX_CMDS);
        memset(widgets_.get(), 0, sizeof(Widget) * MAX_WIDGETS);
        return;
    }
    cmds_      = cdc::core::psramAlloc<DrawCmd>(MAX_CMDS);
    textArena_ = cdc::core::psramAlloc<char>(TEXT_ARENA);
    widgets_   = cdc::core::psramAlloc<Widget>(MAX_WIDGETS);
    blobArena_ = cdc::core::psramAlloc<uint8_t>(BLOB_ARENA);
    if (!cmds_ || !textArena_ || !widgets_ || !blobArena_) {
        LOG_E("CanvasView", "PSRAM arena allocation failed");
        cmds_.reset();
        textArena_.reset();
        widgets_.reset();
        blobArena_.reset();
        return;
    }
    memset(cmds_.get(), 0, sizeof(DrawCmd) * MAX_CMDS);
    memset(widgets_.get(), 0, sizeof(Widget) * MAX_WIDGETS);
}

uint16_t CanvasView::internBlob(const uint8_t* data, uint16_t len) {
    if (!blobArena_ || !data || len == 0) return BLOB_NONE;
    uint16_t off = blobUsed_;
    if (off + len > BLOB_ARENA) {
        if (!overflowLogged_) {
            LOG_W("CanvasView", "draw blob arena full, dropping bitmap");
            overflowLogged_ = true;
        }
        return BLOB_NONE;
    }
    memcpy(&blobArena_[off], data, len);
    blobUsed_ = static_cast<uint16_t>(off + len);
    return off;
}

uint16_t CanvasView::internText(const char* text, uint16_t* outLen) {
    if (!textArena_) {
        if (outLen) *outLen = 0;
        return 0;
    }
    size_t len = text ? strlen(text) : 0;
    uint16_t off = textArenaUsed_;
    if (off + len + 1 > TEXT_ARENA) {
        // Truncate to the remaining arena (leave one byte for the NUL).
        len = (off + 1 < TEXT_ARENA) ? (TEXT_ARENA - off - 1) : 0;
        if (!overflowLogged_) {
            LOG_W("CanvasView", "draw text arena full, truncating");
            overflowLogged_ = true;
        }
    }
    if (len) memcpy(&textArena_[off], text, len);
    textArena_[off + len] = '\0';
    textArenaUsed_ = static_cast<uint16_t>(off + len + 1);
    *outLen = static_cast<uint16_t>(len);
    return off;
}

void CanvasView::drawText(int16_t x, int16_t y, const char* text) {
    if (!text || !cmds_ || cmdCount_ >= MAX_CMDS) {
        if (text && !overflowLogged_) {
            LOG_W("CanvasView", "display list full, dropping draw");
            overflowLogged_ = true;
        }
        return;
    }
    DrawCmd c{};
    c.type = CmdType::Text;
    c.x = x;
    c.y = y;
    c.fontId = fontId_;
    c.textSize = textSize_;
    c.inverted = textInverted_;
    c.strOff = internText(text, &c.strLen);
    cmds_[cmdCount_++] = c;
}

void CanvasView::drawTextAligned(int16_t x, int16_t y, int16_t w,
                                  const char* text, uint8_t align) {
    if (!text || !cmds_ || cmdCount_ >= MAX_CMDS) {
        if (text && !overflowLogged_) {
            LOG_W("CanvasView", "display list full, dropping draw");
            overflowLogged_ = true;
        }
        return;
    }
    DrawCmd c{};
    c.type = CmdType::TextAligned;
    c.x = x;
    c.y = y;
    c.w = w;
    c.align = align;
    c.fontId = fontId_;
    c.textSize = textSize_;
    c.inverted = textInverted_;
    c.strOff = internText(text, &c.strLen);
    cmds_[cmdCount_++] = c;
}

void CanvasView::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, bool filled) {
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::Rect;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.filled = filled;
    c.shade = shade_;
    cmds_[cmdCount_++] = c;
}

void CanvasView::drawPixel(int16_t x, int16_t y) {
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::Pixel;
    c.x = x;
    c.y = y;
    cmds_[cmdCount_++] = c;
}

void CanvasView::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::Line;
    c.x = x0;
    c.y = y0;
    c.w = x1;
    c.h = y1;
    cmds_[cmdCount_++] = c;
}

void CanvasView::drawCircle(int16_t x, int16_t y, int16_t r, bool filled) {
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::Circle;
    c.x = x;
    c.y = y;
    c.w = r;
    c.filled = filled;
    c.shade = shade_;
    cmds_[cmdCount_++] = c;
}

void CanvasView::drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                              int16_t x2, int16_t y2, bool filled) {
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::Triangle;
    c.x = x0;
    c.y = y0;
    c.w = x1;
    c.h = y1;
    c.x2 = x2;
    c.y2 = y2;
    c.filled = filled;
    c.shade = shade_;
    cmds_[cmdCount_++] = c;
}

void CanvasView::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                               int16_t r, bool filled) {
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::RoundRect;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.x2 = r;
    c.filled = filled;
    cmds_[cmdCount_++] = c;
}

void CanvasView::drawBitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                            const uint8_t* data, uint32_t len) {
    if (!cmds_ || cmdCount_ >= MAX_CMDS || !data || w <= 0 || h <= 0) return;
    uint32_t need = static_cast<uint32_t>((w + 7) / 8) * static_cast<uint32_t>(h);
    if (len < need || need == 0 || need > BLOB_ARENA) {
        if (!overflowLogged_) {
            LOG_W("CanvasView", "bitmap too large or short, dropping");
            overflowLogged_ = true;
        }
        return;
    }
    uint16_t off = internBlob(data, static_cast<uint16_t>(need));
    if (off == BLOB_NONE) return;
    DrawCmd c{};
    c.type = CmdType::Bitmap;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.strOff = off;
    c.strLen = static_cast<uint16_t>(need);
    cmds_[cmdCount_++] = c;
}

void CanvasView::drawHLine(int16_t x, int16_t y, int16_t w) {
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::HLine;
    c.x = x;
    c.y = y;
    c.w = w;
    cmds_[cmdCount_++] = c;
}

void CanvasView::drawVLine(int16_t x, int16_t y, int16_t h) {
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::VLine;
    c.x = x;
    c.y = y;
    c.h = h;
    cmds_[cmdCount_++] = c;
}

void CanvasView::paintText(int16_t x, int16_t y, int16_t w, const char* text,
                           uint8_t align, uint8_t fontId, uint8_t textSize,
                           bool inverted) {
    auto* g = gfx();
    if (!g || !text) return;
    const GFXfont* font = getGfxFont(fontId);
    g->setFont(font);
    g->setTextSize(textSize);
    g->setTextColor(inverted ? EPD_WHITE : EPD_BLACK);
    g->setTextWrap(false);

    int16_t draw_x = x;
    if (align == 1 || align == 2) {
        int16_t bx, by;
        uint16_t bw, bh;
        render::measureText(g, text, font, 0, 0, &bx, &by, &bw, &bh);
        if (align == 1) {
            draw_x = x + (w - static_cast<int16_t>(bw)) / 2;
        } else {
            draw_x = x + w - static_cast<int16_t>(bw);
        }
    }
    g->setCursor(draw_x, y + bodyTop());
    render::drawText(g, text, font);
}

void CanvasView::replayDisplayList() {
    auto* g = gfx();
    if (!g || !cmds_) return;
    for (uint16_t i = 0; i < cmdCount_; ++i) {
        const DrawCmd& c = cmds_[i];
        switch (c.type) {
            case CmdType::Text:
                paintText(c.x, c.y, 0, &textArena_[c.strOff], 0,
                          c.fontId, c.textSize, c.inverted);
                break;
            case CmdType::TextAligned:
                paintText(c.x, c.y, c.w, &textArena_[c.strOff], c.align,
                          c.fontId, c.textSize, c.inverted);
                break;
            case CmdType::Rect: {
                int16_t yy = c.y + bodyTop();
                if (c.filled) {
                    if (c.shade >= 255) g->fillRect(c.x, yy, c.w, c.h, EPD_BLACK);
                    else if (c.shade > 0) fillRectDither(g, c.x, yy, c.w, c.h, c.shade);
                } else {
                    g->drawRect(c.x, yy, c.w, c.h, EPD_BLACK);
                }
                break;
            }
            case CmdType::HLine:
                g->drawFastHLine(c.x, c.y + bodyTop(), c.w, EPD_BLACK);
                break;
            case CmdType::VLine:
                g->drawFastVLine(c.x, c.y + bodyTop(), c.h, EPD_BLACK);
                break;
            case CmdType::Pixel:
                g->drawPixel(c.x, c.y + bodyTop(), EPD_BLACK);
                break;
            case CmdType::Line:
                g->drawLine(c.x, c.y + bodyTop(), c.w, c.h + bodyTop(), EPD_BLACK);
                break;
            case CmdType::Circle:
                if (c.filled) {
                    if (c.shade >= 255) g->fillCircle(c.x, c.y + bodyTop(), c.w, EPD_BLACK);
                    else if (c.shade > 0) fillCircleDither(g, c.x, c.y + bodyTop(), c.w, c.shade);
                } else {
                    g->drawCircle(c.x, c.y + bodyTop(), c.w, EPD_BLACK);
                }
                break;
            case CmdType::Triangle:
                if (c.filled) {
                    if (c.shade >= 255)
                        g->fillTriangle(c.x, c.y + bodyTop(), c.w, c.h + bodyTop(),
                                        c.x2, c.y2 + bodyTop(), EPD_BLACK);
                    else if (c.shade > 0)
                        fillTriangleDither(g, c.x, c.y + bodyTop(), c.w, c.h + bodyTop(),
                                           c.x2, c.y2 + bodyTop(), c.shade);
                } else {
                    g->drawTriangle(c.x, c.y + bodyTop(), c.w, c.h + bodyTop(),
                                    c.x2, c.y2 + bodyTop(), EPD_BLACK);
                }
                break;
            case CmdType::RoundRect:
                if (c.filled) {
                    g->fillRoundRect(c.x, c.y + bodyTop(), c.w, c.h, c.x2, EPD_BLACK);
                } else {
                    g->drawRoundRect(c.x, c.y + bodyTop(), c.w, c.h, c.x2, EPD_BLACK);
                }
                break;
            case CmdType::Bitmap:
                g->drawBitmap(c.x, c.y + bodyTop(), &blobArena_[c.strOff],
                              c.w, c.h, EPD_BLACK);
                break;
        }
    }
}

void CanvasView::commit(bool full_refresh) {
    if (full_refresh) {
        needsFullRefresh_ = true;
    }
    markDirty();
}

CanvasView::Widget* CanvasView::findWidget(uint32_t id) {
    if (id == 0) return nullptr;
    for (uint8_t i = 0; i < widgetCount_; ++i) {
        if (widgets_[i].id == id) return &widgets_[i];
    }
    return nullptr;
}

const CanvasView::Widget* CanvasView::findWidget(uint32_t id) const {
    if (id == 0) return nullptr;
    for (uint8_t i = 0; i < widgetCount_; ++i) {
        if (widgets_[i].id == id) return &widgets_[i];
    }
    return nullptr;
}

CanvasView::Widget* CanvasView::focusedWidget() {
    return findWidget(focused_);
}

bool CanvasView::addSlider(uint32_t id, int32_t min, int32_t max,
                            int32_t initial, int32_t step) {
    if (id == 0 || !widgets_ || widgetCount_ >= MAX_WIDGETS || findWidget(id)) {
        return false;
    }
    Widget& w = widgets_[widgetCount_++];
    w = Widget{};
    w.id = id;
    w.type = WidgetType::Slider;
    w.min = min;
    w.max = max;
    w.step = step > 0 ? step : 1;
    w.value = std::clamp(initial, min, max);
    return true;
}

bool CanvasView::addText(uint32_t id, uint16_t max_len, const char* initial) {
    if (id == 0 || !widgets_ || widgetCount_ >= MAX_WIDGETS || findWidget(id)) {
        return false;
    }
    Widget& w = widgets_[widgetCount_++];
    w = Widget{};
    w.id = id;
    w.type = WidgetType::Text;
    w.max_len = max_len < MAX_TEXT_LEN ? max_len : (MAX_TEXT_LEN - 1);
    if (initial) {
        size_t n = std::min(strlen(initial), static_cast<size_t>(w.max_len));
        memcpy(w.text, initial, n);
        w.text[n] = '\0';
        w.text_len = static_cast<uint16_t>(n);
    }
    return true;
}

bool CanvasView::addButton(uint32_t id) {
    if (id == 0 || !widgets_ || widgetCount_ >= MAX_WIDGETS || findWidget(id)) {
        return false;
    }
    Widget& w = widgets_[widgetCount_++];
    w = Widget{};
    w.id = id;
    w.type = WidgetType::Button;
    return true;
}

bool CanvasView::removeWidget(uint32_t id) {
    for (uint8_t i = 0; i < widgetCount_; ++i) {
        if (widgets_[i].id == id) {
            for (uint8_t j = i + 1; j < widgetCount_; ++j) {
                widgets_[j - 1] = widgets_[j];
            }
            --widgetCount_;
            widgets_[widgetCount_] = Widget{};
            if (focused_ == id) focused_ = 0;
            return true;
        }
    }
    return false;
}

bool CanvasView::setValue(uint32_t id, int32_t value) {
    Widget* w = findWidget(id);
    if (!w || w->type != WidgetType::Slider) return false;
    w->value = std::clamp(value, w->min, w->max);
    return true;
}

bool CanvasView::getValue(uint32_t id, int32_t* out) const {
    const Widget* w = findWidget(id);
    if (!w || !out) return false;
    *out = w->value;
    return true;
}

bool CanvasView::setText(uint32_t id, const char* text) {
    Widget* w = findWidget(id);
    if (!w || w->type != WidgetType::Text) return false;
    size_t n = std::min(text ? strlen(text) : size_t{0},
                        static_cast<size_t>(w->max_len));
    if (text) memcpy(w->text, text, n);
    w->text[n] = '\0';
    w->text_len = static_cast<uint16_t>(n);
    w->t9_last_key = 0;
    w->t9_press_count = 0;
    return true;
}

int CanvasView::getText(uint32_t id, char* out, size_t cap) const {
    const Widget* w = findWidget(id);
    if (!w || !out || cap == 0) return -1;
    size_t n = std::min(static_cast<size_t>(w->text_len), cap - 1);
    memcpy(out, w->text, n);
    out[n] = '\0';
    return static_cast<int>(n);
}

bool CanvasView::setFocus(uint32_t id) {
    if (id == 0) {
        focused_ = 0;
        return true;
    }
    Widget* w = findWidget(id);
    if (!w) return false;
    focused_ = id;
    return true;
}

void CanvasView::t9_commit_pending(Widget& w) {
    w.t9_last_key = 0;
    w.t9_press_count = 0;
}

void CanvasView::t9_apply_key(Widget& w, char key, uint32_t now) {
    const char* table = t9_chars(key);
    if (!table) return;
    size_t table_len = strlen(table);
    if (table_len == 0) return;

    bool same_key = (w.t9_last_key == key)
                    && ((now - w.t9_last_time) < T9_SETTLE_MS)
                    && w.text_len > 0;

    if (same_key) {
        w.t9_press_count = (w.t9_press_count + 1) % static_cast<uint8_t>(table_len);
        w.text[w.text_len - 1] = table[w.t9_press_count];
    } else {
        if (w.text_len >= w.max_len) {
            return;
        }
        w.text[w.text_len++] = table[0];
        w.text[w.text_len] = '\0';
        w.t9_press_count = 0;
    }
    w.t9_last_key = key;
    w.t9_last_time = now;
}

InputResult CanvasView::dispatchKeyToWidget(Widget& w, char key) {
    uint32_t now = nowMs();
    switch (w.type) {
        case WidgetType::Slider: {
            if (key == '4' || key == KEY_UP) {
                int32_t nv = std::clamp<int32_t>(w.value - w.step, w.min, w.max);
                if (nv != w.value) {
                    w.value = nv;
                    if (widgetCb_) widgetCb_(w.id, WidgetEvent::Changed);
                }
                return InputResult::CONSUMED;
            }
            if (key == '6' || key == KEY_DOWN) {
                int32_t nv = std::clamp<int32_t>(w.value + w.step, w.min, w.max);
                if (nv != w.value) {
                    w.value = nv;
                    if (widgetCb_) widgetCb_(w.id, WidgetEvent::Changed);
                }
                return InputResult::CONSUMED;
            }
            if (key == KEY_YES) {
                if (widgetCb_) widgetCb_(w.id, WidgetEvent::Committed);
                return InputResult::CONSUMED;
            }
            if (key == KEY_NO) {
                if (widgetCb_) widgetCb_(w.id, WidgetEvent::Cancelled);
                return InputResult::CONSUMED;
            }
            return InputResult::IGNORED;
        }
        case WidgetType::Text: {
            if (key >= '0' && key <= '9') {
                t9_apply_key(w, key, now);
                if (widgetCb_) widgetCb_(w.id, WidgetEvent::Changed);
                return InputResult::CONSUMED;
            }
            if (key == '*') {
                t9_commit_pending(w);
                if (w.text_len > 0) {
                    --w.text_len;
                    w.text[w.text_len] = '\0';
                    if (widgetCb_) widgetCb_(w.id, WidgetEvent::Changed);
                }
                return InputResult::CONSUMED;
            }
            if (key == '#') {
                t9_commit_pending(w);
                if (w.text_len < w.max_len) {
                    w.text[w.text_len++] = ' ';
                    w.text[w.text_len] = '\0';
                    if (widgetCb_) widgetCb_(w.id, WidgetEvent::Changed);
                }
                return InputResult::CONSUMED;
            }
            if (key == KEY_YES) {
                t9_commit_pending(w);
                if (widgetCb_) widgetCb_(w.id, WidgetEvent::Committed);
                return InputResult::CONSUMED;
            }
            if (key == KEY_NO) {
                if (w.text_len > 0 && w.t9_last_key != 0
                    && (now - w.t9_last_time) < T9_SETTLE_MS) {
                    t9_commit_pending(w);
                    return InputResult::CONSUMED;
                }
                if (w.text_len > 0) {
                    --w.text_len;
                    w.text[w.text_len] = '\0';
                    if (widgetCb_) widgetCb_(w.id, WidgetEvent::Changed);
                    return InputResult::CONSUMED;
                }
                if (widgetCb_) widgetCb_(w.id, WidgetEvent::Cancelled);
                return InputResult::CONSUMED;
            }
            return InputResult::IGNORED;
        }
        case WidgetType::Button: {
            if (key == KEY_YES) {
                if (widgetCb_) widgetCb_(w.id, WidgetEvent::Committed);
                return InputResult::CONSUMED;
            }
            if (key == KEY_NO) {
                if (widgetCb_) widgetCb_(w.id, WidgetEvent::Cancelled);
                return InputResult::CONSUMED;
            }
            return InputResult::IGNORED;
        }
        default:
            return InputResult::IGNORED;
    }
}

InputResult CanvasView::onKey(char key) {
    Widget* focused = focusedWidget();
    if (focused) {
        InputResult r = dispatchKeyToWidget(*focused, key);
        if (r == InputResult::CONSUMED) {
            return InputResult::CONSUMED;
        }
    }

    if (keyCb_) {
        keyCb_(key, focused_);
        return InputResult::CONSUMED;
    }

    if (key == KEY_NO) {
        return InputResult::REQUEST_POP;
    }
    return InputResult::IGNORED;
}

// Push this canvas's keypad modes (deferred short-press while a long-press
// handler is registered, and key-repeat) to the global keypad. Applied while
// the canvas is the active view; onExit restores the defaults for other views.
void CanvasView::applyKeypadConfig() {
    if (auto* kp = hal::getKeypadInstance()) {
        kp->setDeferShortPress(hal::IKeypad::DEFER_SRC_VIEW, longPressCb_ != nullptr);
        kp->setKeyRepeat(keyRepeatInitialMs_, keyRepeatPeriodMs_);
    }
}

void CanvasView::setLongPressCallback(LongPressCallback cb) {
    longPressCb_ = cb;
    applyKeypadConfig();
}

InputResult CanvasView::onLongPress(char key) {
    if (longPressCb_) {
        longPressCb_(key);
        return InputResult::CONSUMED;
    }
    return InputResult::IGNORED;
}

void CanvasView::onEnter(void* context) {
    ViewBase::onEnter(context);
    applyKeypadConfig();
}

void CanvasView::onResume() {
    ViewBase::onResume();
    applyKeypadConfig();
}

void CanvasView::onExit() {
    if (auto* kp = hal::getKeypadInstance()) {
        kp->setDeferShortPress(hal::IKeypad::DEFER_SRC_VIEW, false);
        kp->setKeyRepeat(0, 0);
    }
    ViewBase::onExit();
}

void CanvasView::render(bool partial) {
    auto* display = hal::getDisplayInstance();
    if (!display) return;
    auto* g = gfx();
    if (!g) return;

    const uint16_t width = display->getWidth();
    const uint16_t height = display->getHeight();

    if (!partial || needsFullRefresh_) {
        g->fillScreen(EPD_WHITE);
        needsFullRefresh_ = false;
        headerDrawnOnce_ = false;
    }

    if (title_ && title_[0] != '\0' && !headerDrawnOnce_) {
        g->setTextColor(EPD_BLACK);
        g->setTextSize(1);
        g->setTextWrap(false);
        render::drawHeaderLeft(g, title_, 4, TITLE_Y, width);
        headerDrawnOnce_ = true;
    }

    replayDisplayList();

    const char* hint = customFooter_ ? customFooter_ : footer_;
    render::drawFooterBar(g, width, height, nullptr, hint, hint != nullptr);

    dirty_ = false;
}

}  // namespace cdc::ui
