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
#include "cdc_log.h"
#include "esp_timer.h"
#include <goodisplay/gdey029T94.h>
#include <algorithm>
#include <cstring>

namespace cdc::ui {

namespace {

constexpr int TITLE_Y = 12;
constexpr int HEADER_HEIGHT_DEFAULT = 18;

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
    widgetCount_ = 0;
    focused_ = 0;
    textSize_ = 1;
    textInverted_ = false;
    keyCb_ = nullptr;
    widgetCb_ = nullptr;
    footer_ = nullptr;
    headerHeight_ = (title && title[0] != '\0') ? HEADER_HEIGHT_DEFAULT : 0;
    needsFullRefresh_ = true;
    keyRepeatInitialMs_ = 0;
    keyRepeatPeriodMs_ = 0;
    lastKeyTimeMs_ = 0;
    lastKeyRepeatMs_ = 0;
    heldKey_ = 0;
    headerDrawnOnce_ = false;
    customFooter_ = nullptr;
    dirty_ = true;
}

void CanvasView::setFooter(const char* hint) {
    footer_ = hint;
    customFooter_ = hint;
}

void CanvasView::setKeyRepeat(uint16_t initial_ms, uint16_t repeat_ms) {
    keyRepeatInitialMs_ = initial_ms;
    keyRepeatPeriodMs_ = repeat_ms;
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
    auto* g = gfx();
    auto* display = hal::getDisplayInstance();
    if (!g || !display) return;
    g->fillRect(0, bodyTop(), display->getWidth(),
                bodyBottom() - bodyTop(), EPD_WHITE);
}

void CanvasView::gfxApplyTextState() {
    auto* g = gfx();
    if (!g) return;
    g->setFont(cdc::ui::getGfxFont(fontId_));
    g->setTextSize(textSize_);
    g->setTextColor(textInverted_ ? EPD_WHITE : EPD_BLACK);
    g->setTextWrap(false);
}

void CanvasView::drawText(int16_t x, int16_t y, const char* text) {
    auto* g = gfx();
    if (!g || !text) return;
    gfxApplyTextState();
    g->setCursor(x, y + bodyTop());
    render::drawText(g, text, getGfxFont(fontId_));
}

void CanvasView::drawTextAligned(int16_t x, int16_t y, int16_t w,
                                  const char* text, uint8_t align) {
    auto* g = gfx();
    if (!g || !text) return;
    gfxApplyTextState();

    const GFXfont* font = getGfxFont(fontId_);
    int16_t bx, by;
    uint16_t bw, bh;
    render::measureText(g, text, font, 0, 0, &bx, &by, &bw, &bh);

    int16_t draw_x = x;
    if (align == 1) {
        draw_x = x + (w - static_cast<int16_t>(bw)) / 2;
    } else if (align == 2) {
        draw_x = x + w - static_cast<int16_t>(bw);
    }
    g->setCursor(draw_x, y + bodyTop());
    render::drawText(g, text, font);
}

void CanvasView::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, bool filled) {
    auto* g = gfx();
    if (!g) return;
    int16_t yy = y + bodyTop();
    if (filled) {
        g->fillRect(x, yy, w, h, EPD_BLACK);
    } else {
        g->drawRect(x, yy, w, h, EPD_BLACK);
    }
}

void CanvasView::invertRect(int16_t x, int16_t y, int16_t w, int16_t h) {
    (void)x; (void)y; (void)w; (void)h;
}

void CanvasView::drawHLine(int16_t x, int16_t y, int16_t w) {
    auto* g = gfx();
    if (!g) return;
    g->drawFastHLine(x, y + bodyTop(), w, EPD_BLACK);
}

void CanvasView::drawVLine(int16_t x, int16_t y, int16_t h) {
    auto* g = gfx();
    if (!g) return;
    g->drawFastVLine(x, y + bodyTop(), h, EPD_BLACK);
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
    if (id == 0 || widgetCount_ >= MAX_WIDGETS || findWidget(id)) {
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
    if (id == 0 || widgetCount_ >= MAX_WIDGETS || findWidget(id)) {
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
    if (id == 0 || widgetCount_ >= MAX_WIDGETS || findWidget(id)) {
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
    lastKeyTimeMs_ = nowMs();
    heldKey_ = key;
    lastKeyRepeatMs_ = lastKeyTimeMs_;

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

void CanvasView::onTick(uint32_t now) {
    if (heldKey_ == 0 || keyRepeatInitialMs_ == 0 || keyRepeatPeriodMs_ == 0) {
        return;
    }
    uint32_t elapsed = now - lastKeyTimeMs_;
    if (elapsed < keyRepeatInitialMs_) return;

    uint32_t sinceLastRepeat = now - lastKeyRepeatMs_;
    if (sinceLastRepeat < keyRepeatPeriodMs_) return;

    lastKeyRepeatMs_ = now;
    onKey(heldKey_);
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

    const char* hint = customFooter_ ? customFooter_ : footer_;
    render::drawFooterBar(g, width, height, nullptr, hint, hint != nullptr);

    dirty_ = false;
}

}  // namespace cdc::ui
