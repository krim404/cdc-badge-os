/**
 * \file ColorPickerView.cpp
 * \brief RGB-triangle color picker with Bayer-4 dithered preview for
 *        1-bit e-paper displays.
 */

#include "cdc_views/ColorPickerView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/LayoutConstants.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_ui/I18n.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_hal/IKeypad.h"
#include "cdc_log.h"
#include <goodisplay/gdey029T94.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

static constexpr uint32_t REPEAT_INITIAL_MS = 300;
static constexpr uint32_t REPEAT_PERIOD_MS = 70;

namespace cdc::ui {

namespace {

constexpr int16_t HEADER_LINE_OFF  = 6;       // line at y = 16

constexpr int16_t TRI_OX = 10;
constexpr int16_t TRI_OY = 20;
constexpr int16_t R_X = 50,  R_Y = 0;
constexpr int16_t G_X = 0,   G_Y = 60;
constexpr int16_t B_X = 100, B_Y = 60;

constexpr int16_t TEXT_X = 160;
constexpr int16_t TEXT_Y = 22;

constexpr int16_t SLIDER_X = 160;
constexpr int16_t SLIDER_Y = 80;
constexpr int16_t SLIDER_W = 125;
constexpr int16_t SLIDER_H = 8;

constexpr int16_t NAME_Y = 98;

constexpr uint8_t kBayer4[4][4] = {
    { 15, 135,  47, 175 },
    { 207,  79, 239, 111 },
    {  63, 191,  31, 159 },
    { 255, 127, 223,  95 }
};

inline int32_t sign(int16_t x1, int16_t y1,
                    int16_t x2, int16_t y2,
                    int16_t x3, int16_t y3) {
    return (x1 - x3) * (y2 - y3) - (x2 - x3) * (y1 - y3);
}

inline uint8_t lum(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
}

const char* colorName(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t mx = std::max({r, g, b});
    uint8_t mn = std::min({r, g, b});
    if (mx < 24) return "Black";
    if (mx - mn < 24) {
        if (mx < 90)  return "Dark gray";
        if (mx < 200) return "Gray";
        return "White";
    }
    int16_t delta = mx - mn;
    int16_t hue;
    if (mx == r)      hue = ((int16_t)(g - b) * 60) / delta;
    else if (mx == g) hue = 120 + ((int16_t)(b - r) * 60) / delta;
    else              hue = 240 + ((int16_t)(r - g) * 60) / delta;
    if (hue < 0) hue += 360;
    bool dark = mx < 110;
    if (hue < 15)  return dark ? "Brown" : "Red";
    if (hue < 35)  return dark ? "Brown" : "Orange";
    if (hue < 55)  return dark ? "Olive" : "Yellow";
    if (hue < 80)  return dark ? "Olive" : "Lime";
    if (hue < 150) return dark ? "Dark green" : "Green";
    if (hue < 180) return dark ? "Teal" : "Cyan";
    if (hue < 220) return dark ? "Teal" : "Sky blue";
    if (hue < 260) return dark ? "Navy" : "Blue";
    if (hue < 290) return dark ? "Purple" : "Violet";
    if (hue < 330) return dark ? "Magenta" : "Pink";
    return dark ? "Brown" : "Red";
}

}  // namespace

void ColorPickerView::init(uint8_t r, uint8_t g, uint8_t b) {
    value_ = std::max({r, g, b, static_cast<uint8_t>(1)});
    cursorX_ = (R_X + G_X + B_X) / 3;
    cursorY_ = (R_Y + G_Y + B_Y) / 3;
    setFromRGB(r, g, b);
    repeatStartMs_ = 0;
    lastRepeatMs_ = 0;
    onSave_ = nullptr;
    dirty_ = true;
}

void ColorPickerView::barycentric(int16_t px, int16_t py,
                                  int32_t& aR, int32_t& aG, int32_t& aB) const {
    int32_t d = sign(R_X, R_Y, G_X, G_Y, B_X, B_Y);
    if (d == 0) { aR = aG = aB = 0; return; }
    aR = sign(px, py, G_X, G_Y, B_X, B_Y) * 255 / d;
    aG = sign(R_X, R_Y, px, py, B_X, B_Y) * 255 / d;
    aB = sign(R_X, R_Y, G_X, G_Y, px, py) * 255 / d;
}

void ColorPickerView::clampInsideTriangle() {
    int32_t aR, aG, aB;
    int16_t bestX = cursorX_;
    int16_t bestY = cursorY_;
    barycentric(cursorX_, cursorY_, aR, aG, aB);
    if (aR < 0 || aG < 0 || aB < 0) {
        int32_t total = aR + aG + aB;
        if (total != 0) {
            int32_t cx = (R_X * std::max<int32_t>(aR, 0) +
                          G_X * std::max<int32_t>(aG, 0) +
                          B_X * std::max<int32_t>(aB, 0)) /
                         std::max<int32_t>(1, std::max<int32_t>(aR, 0) +
                                              std::max<int32_t>(aG, 0) +
                                              std::max<int32_t>(aB, 0));
            int32_t cy = (R_Y * std::max<int32_t>(aR, 0) +
                          G_Y * std::max<int32_t>(aG, 0) +
                          B_Y * std::max<int32_t>(aB, 0)) /
                         std::max<int32_t>(1, std::max<int32_t>(aR, 0) +
                                              std::max<int32_t>(aG, 0) +
                                              std::max<int32_t>(aB, 0));
            bestX = static_cast<int16_t>(cx);
            bestY = static_cast<int16_t>(cy);
        }
        cursorX_ = bestX;
        cursorY_ = bestY;
    }
}

void ColorPickerView::setFromRGB(uint8_t r, uint8_t g, uint8_t b) {
    int32_t sum = static_cast<int32_t>(r) + g + b;
    if (sum == 0) {
        cursorX_ = (R_X + G_X + B_X) / 3;
        cursorY_ = (R_Y + G_Y + B_Y) / 3;
        return;
    }
    int32_t cx = (r * R_X + g * G_X + b * B_X) / sum;
    int32_t cy = (r * R_Y + g * G_Y + b * B_Y) / sum;
    cursorX_ = static_cast<int16_t>(cx);
    cursorY_ = static_cast<int16_t>(cy);
}

void ColorPickerView::moveCursor(int16_t dx, int16_t dy) {
    int16_t nx = cursorX_ + dx;
    int16_t ny = cursorY_ + dy;
    int32_t aR, aG, aB;
    int16_t saved_x = cursorX_;
    int16_t saved_y = cursorY_;
    cursorX_ = nx;
    cursorY_ = ny;
    barycentric(cursorX_, cursorY_, aR, aG, aB);
    if (aR < 0 || aG < 0 || aB < 0) {
        cursorX_ = saved_x;
        cursorY_ = saved_y;
        return;
    }
    dirty_ = true;
}

void ColorPickerView::adjustValue(int8_t delta) {
    int16_t nv = value_ + delta;
    nv = std::clamp<int16_t>(nv, 1, 255);
    if (nv != value_) {
        value_ = static_cast<uint8_t>(nv);
        dirty_ = true;
    }
}

void ColorPickerView::rawColor(uint8_t& r, uint8_t& g, uint8_t& b) const {
    int32_t aR, aG, aB;
    barycentric(cursorX_, cursorY_, aR, aG, aB);
    aR = std::max<int32_t>(0, aR);
    aG = std::max<int32_t>(0, aG);
    aB = std::max<int32_t>(0, aB);
    int32_t sum = aR + aG + aB;
    if (sum == 0) { r = g = b = 0; return; }
    r = static_cast<uint8_t>(aR * 255 / sum);
    g = static_cast<uint8_t>(aG * 255 / sum);
    b = static_cast<uint8_t>(aB * 255 / sum);
}

void ColorPickerView::currentColor(uint8_t& r, uint8_t& g, uint8_t& b) const {
    rawColor(r, g, b);
    r = static_cast<uint8_t>((static_cast<uint16_t>(r) * value_) / 255);
    g = static_cast<uint8_t>((static_cast<uint16_t>(g) * value_) / 255);
    b = static_cast<uint8_t>((static_cast<uint16_t>(b) * value_) / 255);
}

void ColorPickerView::drawDitheredBox(Gdey029T94* gfx, int16_t x, int16_t y,
                                       int16_t w, int16_t h, uint8_t luminance) const {
    if (!gfx) return;
    for (int16_t row = 0; row < h; ++row) {
        for (int16_t col = 0; col < w; ++col) {
            uint8_t threshold = kBayer4[row & 3][col & 3];
            if (luminance < threshold) {
                gfx->drawPixel(x + col, y + row, EPD_BLACK);
            }
        }
    }
}

void ColorPickerView::drawTriangle(Gdey029T94* gfx, int16_t ox, int16_t oy) const {
    gfx->drawLine(ox + R_X, oy + R_Y, ox + G_X, oy + G_Y, EPD_BLACK);
    gfx->drawLine(ox + G_X, oy + G_Y, ox + B_X, oy + B_Y, EPD_BLACK);
    gfx->drawLine(ox + B_X, oy + B_Y, ox + R_X, oy + R_Y, EPD_BLACK);
}

InputResult ColorPickerView::onKey(char key) {
    switch (key) {
        case '2': moveCursor(0, -3); repeatStartMs_ = 0; return InputResult::CONSUMED;
        case '8': moveCursor(0,  3); repeatStartMs_ = 0; return InputResult::CONSUMED;
        case '4': moveCursor(-3, 0); repeatStartMs_ = 0; return InputResult::CONSUMED;
        case '6': moveCursor( 3, 0); repeatStartMs_ = 0; return InputResult::CONSUMED;
        case '7': adjustValue(-10); repeatStartMs_ = 0; return InputResult::CONSUMED;
        case '9': adjustValue( 10); repeatStartMs_ = 0; return InputResult::CONSUMED;
        case KEY_YES: {
            cdc::ui::ViewStack::instance().pop();
            if (onSave_) {
                uint8_t r, g, b;
                currentColor(r, g, b);
                onSave_(r, g, b);
            }
            return InputResult::CONSUMED;
        }
        case KEY_NO:
            return InputResult::REQUEST_POP;
        default:
            return InputResult::IGNORED;
    }
}

void ColorPickerView::onTick(uint32_t nowMs) {
    auto* kp = cdc::hal::getKeypadInstance();
    if (!kp) return;
    bool any = false;
    int16_t dx = 0, dy = 0;
    int8_t  dv = 0;
    using cdc::hal::Key;
    if (kp->isKeyPressed(Key::KEY_2)) { dy -= 3; any = true; }
    if (kp->isKeyPressed(Key::KEY_8)) { dy += 3; any = true; }
    if (kp->isKeyPressed(Key::KEY_4)) { dx -= 3; any = true; }
    if (kp->isKeyPressed(Key::KEY_6)) { dx += 3; any = true; }
    if (kp->isKeyPressed(Key::KEY_7)) { dv -= 10; any = true; }
    if (kp->isKeyPressed(Key::KEY_9)) { dv += 10; any = true; }
    if (!any) { repeatStartMs_ = 0; return; }
    if (repeatStartMs_ == 0) {
        repeatStartMs_ = nowMs;
        lastRepeatMs_ = nowMs;
        return;
    }
    if (nowMs - repeatStartMs_ < REPEAT_INITIAL_MS) return;
    if (nowMs - lastRepeatMs_ < REPEAT_PERIOD_MS) return;
    lastRepeatMs_ = nowMs;
    if (dx || dy) moveCursor(dx, dy);
    if (dv) adjustValue(dv);
}

const char* ColorPickerView::getFooterHint() const {
    if (customFooter_) return customFooter_;
    return "[2/4/6/8] mix  [7/9] intensity  [Y] OK";
}

void ColorPickerView::render(bool partial) {
    auto* display = hal::getDisplayInstance();
    if (!display) return;
    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;
    const uint16_t width = display->getWidth();
    const uint16_t height = display->getHeight();

    if (!partial) {
        gfx->fillScreen(EPD_WHITE);
    } else {
        gfx->fillRect(0, 0, width, height - layout::FOOTER_HEIGHT, EPD_WHITE);
    }

    gfx->setTextColor(EPD_BLACK);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);

    int16_t ox = TRI_OX;
    int16_t oy = TRI_OY;

    // Subtle ~6% textured background inside the triangle.
    int32_t det = sign(R_X, R_Y, G_X, G_Y, B_X, B_Y);
    int16_t bx0 = std::min({R_X, G_X, B_X});
    int16_t bx1 = std::max({R_X, G_X, B_X});
    int16_t by0 = std::min({R_Y, G_Y, B_Y});
    int16_t by1 = std::max({R_Y, G_Y, B_Y});
    for (int16_t py = by0; py <= by1; ++py) {
        for (int16_t px = bx0; px <= bx1; ++px) {
            int32_t s1 = sign(px, py, G_X, G_Y, B_X, B_Y);
            int32_t s2 = sign(R_X, R_Y, px, py, B_X, B_Y);
            int32_t s3 = sign(R_X, R_Y, G_X, G_Y, px, py);
            bool inside = (s1 >= 0 && s2 >= 0 && s3 >= 0) ||
                          (s1 <= 0 && s2 <= 0 && s3 <= 0);
            if (!inside) continue;
            if (kBayer4[py & 3][px & 3] < 16) {
                gfx->drawPixel(ox + px, oy + py, EPD_BLACK);
            }
        }
    }
    (void)det;

    drawTriangle(gfx, ox, oy);

    gfx->setCursor(ox + R_X - 3, oy + R_Y - 9);
    gfx->print("R");
    gfx->setCursor(ox + G_X - 6, oy + G_Y + 2);
    gfx->print("G");
    gfx->setCursor(ox + B_X + 2, oy + B_Y + 2);
    gfx->print("B");

    int16_t cx = ox + cursorX_;
    int16_t cy = oy + cursorY_;
    gfx->fillRect(cx - 2, cy - 2, 5, 5, EPD_BLACK);
    gfx->drawPixel(cx, cy, EPD_WHITE);

    uint8_t r, g, b;
    currentColor(r, g, b);

    char buf[24];
    gfx->setTextSize(1);
    std::snprintf(buf, sizeof(buf), "R: %3u", r);
    gfx->setCursor(TEXT_X, TEXT_Y);
    gfx->print(buf);
    std::snprintf(buf, sizeof(buf), "G: %3u", g);
    gfx->setCursor(TEXT_X, TEXT_Y + 14);
    gfx->print(buf);
    std::snprintf(buf, sizeof(buf), "B: %3u", b);
    gfx->setCursor(TEXT_X, TEXT_Y + 28);
    gfx->print(buf);

    int16_t sy = SLIDER_Y;
    gfx->setCursor(SLIDER_X, sy - 10);
    std::snprintf(buf, sizeof(buf), "Intensity: %3u", value_);
    gfx->print(buf);
    gfx->drawRect(SLIDER_X, sy, SLIDER_W, SLIDER_H, EPD_BLACK);
    int16_t fill = static_cast<int16_t>((static_cast<uint32_t>(value_) * (SLIDER_W - 4)) / 255);
    gfx->fillRect(SLIDER_X + 2, sy + 2, fill, SLIDER_H - 4, EPD_BLACK);

    const char* name = colorName(r, g, b);
    int16_t bx, by;
    uint16_t bw, bh;
    gfx->setTextSize(1);
    gfx->getTextBounds(name, 0, 0, &bx, &by, &bw, &bh);
    gfx->setCursor((width - bw) / 2, NAME_Y);
    render::printText(gfx, name);

    render::drawFooterBar(gfx, width, height, nullptr, getFooterHint(), true);

    dirty_ = false;
}

}  // namespace cdc::ui
