/**
 * \file CanvasView.cpp
 * \brief Plugin-side custom canvas: plugin draws into the body, host owns the
 *        input state for inline widgets (slider, text-input, button).
 */

#include "cdc_views/CanvasView.h"
#include "cdc_views/Fonts.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/LayoutConstants.h"
#include "cdc_views/MonoSurface.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_hal/IKeypad.h"
#include "cdc_ui/ViewStack.h"
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

/// Upper bound of completion events one animation tick can produce.
constexpr uint8_t ANIM_DONE_CAP =
    cdc::ui::anim::CanvasAnimator::MAX_TWEENS + cdc::ui::anim::SpriteStore::MAX_SPRITES;

}  // namespace

Gdey029T94* CanvasView::gfx() const {
    auto* display = hal::getDisplayInstance();
    if (!display) return nullptr;
    return static_cast<Gdey029T94*>(display->getNativeHandle());
}

int CanvasView::bodyBottom() const {
    auto* display = hal::getDisplayInstance();
    if (!display) return 128;
    // Reserve the footer strip only when a footer hint is actually shown;
    // a footerless canvas (a full-screen badge) gets the whole panel height.
    const bool hasFooter = footer_ || customFooter_;
    return display->getHeight() - (hasFooter ? cdc::ui::layout::FOOTER_HEIGHT : 0);
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
    blobUsed_ = 0;
    elemCount_ = 0;
    curElem_ = 0;
    memset(elems_, 0, sizeof(elems_));
    overflowLogged_ = false;
    fontId_ = 0;
    shade_ = 255;
    inkWhite_ = false;
    animator_.reset();
    if (sprites_.hasArena()) sprites_.setArena(spriteArena_.get(), SPRITE_ARENA);
    animCb_ = nullptr;
    animPolicy_ = ANIM_REFRESH_AUTO;
    animFrameIntervalMs_ = 1000 / ANIM_FPS_DEFAULT;
    lastAnimStepMs_ = 0;
    idleCleanupAtMs_ = 0;
    pausedAtMs_ = 0;
    animCommitCount_ = 0;
    animActive_ = false;
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

void CanvasView::clearBody(bool keep_sprites) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    cmdCount_ = 0;
    textArenaUsed_ = 0;
    blobUsed_ = 0;
    elemCount_ = 0;
    curElem_ = 0;
    memset(elems_, 0, sizeof(elems_));
    // Elements are gone, so their animations go with them (silently: no
    // completion events during teardown).
    animator_.reset();
    if (keep_sprites) {
        // Assets survive for the next screen build; stop playback so an
        // orphaned loop (e.g. a marquee's backing sprite) cannot keep the
        // animation clock and the panel busy with nothing to show.
        sprites_.stopAll();
    } else if (sprites_.hasArena()) {
        sprites_.setArena(spriteArena_.get(), SPRITE_ARENA);
    }
    animActive_ = false;
    idleCleanupAtMs_ = 0;
}

void CanvasView::pushCmd(DrawCmd& c) {
    c.elemId = curElem_;
    cmds_[cmdCount_++] = c;
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
    cdc::core::RecursiveMutexGuard guard(editMutex_);
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
    pushCmd(c);
}

void CanvasView::drawTextAligned(int16_t x, int16_t y, int16_t w,
                                  const char* text, uint8_t align) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
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
    pushCmd(c);
}

void CanvasView::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, bool filled) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::Rect;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.filled = filled;
    c.shade = shade_;
    c.inverted = inkWhite_;
    pushCmd(c);
}

void CanvasView::drawPixel(int16_t x, int16_t y) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::Pixel;
    c.x = x;
    c.y = y;
    c.inverted = inkWhite_;
    pushCmd(c);
}

void CanvasView::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::Line;
    c.x = x0;
    c.y = y0;
    c.w = x1;
    c.h = y1;
    c.inverted = inkWhite_;
    pushCmd(c);
}

void CanvasView::drawCircle(int16_t x, int16_t y, int16_t r, bool filled) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::Circle;
    c.x = x;
    c.y = y;
    c.w = r;
    c.filled = filled;
    c.shade = shade_;
    c.inverted = inkWhite_;
    pushCmd(c);
}

void CanvasView::drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                              int16_t x2, int16_t y2, bool filled) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
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
    c.inverted = inkWhite_;
    pushCmd(c);
}

void CanvasView::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                               int16_t r, bool filled) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::RoundRect;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.x2 = r;
    c.filled = filled;
    c.inverted = inkWhite_;
    pushCmd(c);
}

void CanvasView::drawBitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                            const uint8_t* data, uint32_t len) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
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
    pushCmd(c);
}

void CanvasView::drawHLine(int16_t x, int16_t y, int16_t w) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::HLine;
    c.x = x;
    c.y = y;
    c.w = w;
    c.inverted = inkWhite_;
    pushCmd(c);
}

void CanvasView::drawVLine(int16_t x, int16_t y, int16_t h) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    DrawCmd c{};
    c.type = CmdType::VLine;
    c.x = x;
    c.y = y;
    c.h = h;
    c.inverted = inkWhite_;
    pushCmd(c);
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

CanvasView::Elem* CanvasView::findElem(uint32_t id) {
    if (id == 0) return nullptr;
    for (uint8_t i = 0; i < elemCount_; ++i) {
        if (elems_[i].id == id) return &elems_[i];
    }
    return nullptr;
}

const CanvasView::Elem* CanvasView::findElem(uint32_t id) const {
    return const_cast<CanvasView*>(this)->findElem(id);
}

bool CanvasView::beginElem(uint32_t id) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (id == 0) return false;
    if (!findElem(id)) {
        if (elemCount_ >= MAX_ELEMS) {
            if (!overflowLogged_) {
                LOG_W("CanvasView", "element table full, dropping element");
                overflowLogged_ = true;
            }
            return false;
        }
        elems_[elemCount_] = Elem{};
        elems_[elemCount_].id = id;
        ++elemCount_;
    }
    curElem_ = id;
    return true;
}

void CanvasView::endElem() {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    curElem_ = 0;
}

bool CanvasView::elemSetOffset(uint32_t id, int16_t ox, int16_t oy) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    Elem* e = findElem(id);
    if (!e) return false;
    e->ox = ox;
    e->oy = oy;
    return true;
}

bool CanvasView::elemMove(uint32_t id, int16_t dx, int16_t dy) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    Elem* e = findElem(id);
    if (!e) return false;
    e->ox = static_cast<int16_t>(e->ox + dx);
    e->oy = static_cast<int16_t>(e->oy + dy);
    return true;
}

bool CanvasView::elemShow(uint32_t id, bool visible) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    Elem* e = findElem(id);
    if (!e) return false;
    e->hidden = !visible;
    return true;
}

// Drop all draw commands tagged with `id` and re-pack the arenas.
bool CanvasView::dropElemCmds(uint32_t id) {
    uint16_t out = 0;
    for (uint16_t i = 0; i < cmdCount_; ++i) {
        if (cmds_[i].elemId != id) {
            if (out != i) cmds_[out] = cmds_[i];
            ++out;
        }
    }
    bool dropped = out != cmdCount_;
    cmdCount_ = out;
    if (dropped) compactArenas();
    return dropped;
}

bool CanvasView::elemRemove(uint32_t id) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    Elem* e = findElem(id);
    if (!e) return false;

    dropElemCmds(id);
    animator_.cancelForElem(id);

    uint8_t idx = static_cast<uint8_t>(e - elems_);
    for (uint8_t i = idx + 1; i < elemCount_; ++i) {
        elems_[i - 1] = elems_[i];
    }
    --elemCount_;
    elems_[elemCount_] = Elem{};
    if (curElem_ == id) curElem_ = 0;
    return true;
}

bool CanvasView::elemClear(uint32_t id) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!findElem(id)) return false;
    dropElemCmds(id);
    return true;
}

bool CanvasView::elemSetZ(uint32_t id, int8_t z) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    Elem* e = findElem(id);
    if (!e) return false;
    e->z = z;
    return true;
}

bool CanvasView::elemGetOffset(uint32_t id, int16_t* ox, int16_t* oy) const {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    const Elem* e = findElem(id);
    if (!e) return false;
    if (ox) *ox = e->ox;
    if (oy) *oy = e->oy;
    return true;
}

bool CanvasView::elemGetBounds(uint32_t id, int16_t* x, int16_t* y,
                               uint16_t* w, uint16_t* h) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    Elem* e = findElem(id);
    if (!e || !cmds_) return false;

    int32_t minX = INT32_MAX, minY = INT32_MAX;
    int32_t maxX = INT32_MIN, maxY = INT32_MIN;
    bool any = false;
    for (uint16_t i = 0; i < cmdCount_; ++i) {
        if (cmds_[i].elemId != id) continue;
        int16_t  cx, cy;
        uint16_t cw, ch;
        if (!cmdBounds(cmds_[i], &cx, &cy, &cw, &ch)) continue;
        any  = true;
        minX = std::min<int32_t>(minX, cx);
        minY = std::min<int32_t>(minY, cy);
        maxX = std::max<int32_t>(maxX, cx + cw);
        maxY = std::max<int32_t>(maxY, cy + ch);
    }
    if (!any) return false;
    if (x) *x = static_cast<int16_t>(minX + e->ox);
    if (y) *y = static_cast<int16_t>(minY + e->oy);
    if (w) *w = static_cast<uint16_t>(maxX - minX);
    if (h) *h = static_cast<uint16_t>(maxY - minY);
    return true;
}

// Recorded-coordinate bounding box of one command (element offset NOT applied).
bool CanvasView::cmdBounds(const DrawCmd& c, int16_t* x, int16_t* y,
                           uint16_t* w, uint16_t* h) {
    int32_t x0 = c.x, y0 = c.y, x1 = c.x, y1 = c.y;
    switch (c.type) {
        case CmdType::Text:
        case CmdType::TextAligned: {
            auto* g = gfx();
            if (!g) return false;
            const GFXfont* font = getGfxFont(c.fontId);
            g->setTextSize(c.textSize);
            int16_t  bx, by;
            uint16_t bw, bh;
            render::measureText(g, &textArena_[c.strOff], font, 0, 0,
                                &bx, &by, &bw, &bh);
            int16_t draw_x = c.x;
            if (c.align == 1) {
                draw_x = static_cast<int16_t>(c.x + (c.w - static_cast<int16_t>(bw)) / 2);
            } else if (c.align == 2) {
                draw_x = static_cast<int16_t>(c.x + c.w - static_cast<int16_t>(bw));
            }
            // measureText yields the box relative to the baseline cursor.
            x0 = draw_x + bx;
            y0 = c.y + by;
            x1 = x0 + bw;
            y1 = y0 + bh;
            break;
        }
        case CmdType::Rect:
        case CmdType::RoundRect:
            x1 = c.x + c.w;
            y1 = c.y + c.h;
            break;
        case CmdType::HLine:
            x1 = c.x + c.w;
            y1 = c.y + 1;
            break;
        case CmdType::VLine:
            x1 = c.x + 1;
            y1 = c.y + c.h;
            break;
        case CmdType::Pixel:
            x1 = c.x + 1;
            y1 = c.y + 1;
            break;
        case CmdType::Line:
            x0 = std::min(c.x, c.w);
            y0 = std::min(c.y, c.h);
            x1 = std::max(c.x, c.w) + 1;
            y1 = std::max(c.y, c.h) + 1;
            break;
        case CmdType::Circle:
            x0 = c.x - c.w;
            y0 = c.y - c.w;
            x1 = c.x + c.w + 1;
            y1 = c.y + c.w + 1;
            break;
        case CmdType::Triangle:
            x0 = std::min(c.x, std::min(c.w, c.x2));
            y0 = std::min(c.y, std::min(c.h, c.y2));
            x1 = std::max(c.x, std::max(c.w, c.x2)) + 1;
            y1 = std::max(c.y, std::max(c.h, c.y2)) + 1;
            break;
        case CmdType::Bitmap:
            x1 = c.x + c.w;
            y1 = c.y + c.h;
            break;
        case CmdType::Sprite: {
            anim::SpriteStore::FrameView fv;
            if (!sprites_.frameView(c.strOff, &fv)) return false;
            uint16_t bw = fv.scrollWindow != 0 ? fv.scrollWindow
                        : ((fv.flags & anim::SPRITE_FLAG_ROT_90) ? fv.h : fv.w);
            uint16_t bh = (fv.scrollWindow == 0 && (fv.flags & anim::SPRITE_FLAG_ROT_90))
                              ? fv.w : fv.h;
            x1 = c.x + bw * fv.scale;
            y1 = c.y + bh * fv.scale;
            break;
        }
    }
    *x = static_cast<int16_t>(x0);
    *y = static_cast<int16_t>(y0);
    *w = static_cast<uint16_t>(x1 - x0);
    *h = static_cast<uint16_t>(y1 - y0);
    return true;
}

// Re-pack both arenas after commands were dropped so removing and re-adding
// elements does not exhaust the append-only allocators. Commands keep their
// recording order, so arena spans stay ascending and moves only go left.
void CanvasView::compactArenas() {
    uint16_t textCursor = 0;
    uint16_t blobCursor = 0;
    for (uint16_t i = 0; i < cmdCount_; ++i) {
        DrawCmd& c = cmds_[i];
        if (c.type == CmdType::Text || c.type == CmdType::TextAligned) {
            uint16_t span = static_cast<uint16_t>(c.strLen + 1);  // incl. NUL
            if (c.strOff != textCursor) {
                memmove(&textArena_[textCursor], &textArena_[c.strOff], span);
                c.strOff = textCursor;
            }
            textCursor = static_cast<uint16_t>(textCursor + span);
        } else if (c.type == CmdType::Bitmap && c.strOff != BLOB_NONE) {
            if (c.strOff != blobCursor) {
                memmove(&blobArena_[blobCursor], &blobArena_[c.strOff], c.strLen);
                c.strOff = blobCursor;
            }
            blobCursor = static_cast<uint16_t>(blobCursor + c.strLen);
        }
    }
    textArenaUsed_ = textCursor;
    blobUsed_ = blobCursor;
}

// Shift a command copy by the owning element's offset. Size-like fields stay
// untouched; Line and Triangle store further points in w/h/x2/y2, so those
// shift along.
void CanvasView::applyElemOffset(DrawCmd& c, int16_t ox, int16_t oy) const {
    c.x = static_cast<int16_t>(c.x + ox);
    c.y = static_cast<int16_t>(c.y + oy);
    switch (c.type) {
        case CmdType::Line:
            c.w = static_cast<int16_t>(c.w + ox);
            c.h = static_cast<int16_t>(c.h + oy);
            break;
        case CmdType::Triangle:
            c.w = static_cast<int16_t>(c.w + ox);
            c.h = static_cast<int16_t>(c.h + oy);
            c.x2 = static_cast<int16_t>(c.x2 + ox);
            c.y2 = static_cast<int16_t>(c.y2 + oy);
            break;
        default:
            break;
    }
}

// Replay the display list in z layers: distinct element z values ascending,
// untagged commands and z==0 elements interleave in recording order within
// layer 0. With <= MAX_ELEMS layers the repeated sweep stays trivial.
void CanvasView::replayDisplayList() {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!gfx() || !cmds_) return;

    int16_t layers[MAX_ELEMS + 1];
    uint8_t layerCount = 0;
    layers[layerCount++] = 0;
    for (uint8_t i = 0; i < elemCount_; ++i) {
        int16_t z = elems_[i].z;
        bool seen = false;
        for (uint8_t j = 0; j < layerCount; ++j) {
            if (layers[j] == z) {
                seen = true;
                break;
            }
        }
        if (!seen) layers[layerCount++] = z;
    }
    // Insertion sort ascending; layerCount <= 17.
    for (uint8_t i = 1; i < layerCount; ++i) {
        int16_t v = layers[i];
        int8_t  j = static_cast<int8_t>(i - 1);
        while (j >= 0 && layers[j] > v) {
            layers[j + 1] = layers[j];
            --j;
        }
        layers[j + 1] = v;
    }

    for (uint8_t l = 0; l < layerCount; ++l) {
        for (uint16_t i = 0; i < cmdCount_; ++i) {
            DrawCmd c = cmds_[i];
            int16_t z = 0;
            if (c.elemId != 0) {
                Elem* e = findElem(c.elemId);
                if (e) {
                    if (e->hidden) continue;
                    z = e->z;
                    if (e->ox != 0 || e->oy != 0) applyElemOffset(c, e->ox, e->oy);
                }
            }
            if (z != layers[l]) continue;
            replayCmd(c);
        }
    }
}

void CanvasView::replayCmd(const DrawCmd& c) {
    auto* g = gfx();
    if (!g) return;
    // Shapes recorded with white ink (setInkWhite) erase instead of paint.
    const uint16_t ink = c.inverted ? EPD_WHITE : EPD_BLACK;
    {
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
                    if (c.shade >= 255) g->fillRect(c.x, yy, c.w, c.h, ink);
                    else if (c.shade > 0) render::fillRectDither(g, c.x, yy, c.w, c.h, c.shade, ink);
                } else {
                    g->drawRect(c.x, yy, c.w, c.h, ink);
                }
                break;
            }
            case CmdType::HLine:
                g->drawFastHLine(c.x, c.y + bodyTop(), c.w, ink);
                break;
            case CmdType::VLine:
                g->drawFastVLine(c.x, c.y + bodyTop(), c.h, ink);
                break;
            case CmdType::Pixel:
                g->drawPixel(c.x, c.y + bodyTop(), ink);
                break;
            case CmdType::Line:
                g->drawLine(c.x, c.y + bodyTop(), c.w, c.h + bodyTop(), ink);
                break;
            case CmdType::Circle:
                if (c.filled) {
                    if (c.shade >= 255) g->fillCircle(c.x, c.y + bodyTop(), c.w, ink);
                    else if (c.shade > 0) render::fillCircleDither(g, c.x, c.y + bodyTop(), c.w, c.shade, ink);
                } else {
                    g->drawCircle(c.x, c.y + bodyTop(), c.w, ink);
                }
                break;
            case CmdType::Triangle:
                if (c.filled) {
                    if (c.shade >= 255)
                        g->fillTriangle(c.x, c.y + bodyTop(), c.w, c.h + bodyTop(),
                                        c.x2, c.y2 + bodyTop(), ink);
                    else if (c.shade > 0)
                        render::fillTriangleDither(g, c.x, c.y + bodyTop(), c.w, c.h + bodyTop(),
                                           c.x2, c.y2 + bodyTop(), c.shade, ink);
                } else {
                    g->drawTriangle(c.x, c.y + bodyTop(), c.w, c.h + bodyTop(),
                                    c.x2, c.y2 + bodyTop(), ink);
                }
                break;
            case CmdType::RoundRect:
                if (c.filled) {
                    g->fillRoundRect(c.x, c.y + bodyTop(), c.w, c.h, c.x2, ink);
                } else {
                    g->drawRoundRect(c.x, c.y + bodyTop(), c.w, c.h, c.x2, ink);
                }
                break;
            case CmdType::Bitmap:
                g->drawBitmap(c.x, c.y + bodyTop(), &blobArena_[c.strOff],
                              c.w, c.h, ink);
                break;
            case CmdType::Sprite: {
                anim::SpriteStore::FrameView fv;
                if (!sprites_.frameView(c.strOff, &fv)) break;  // destroyed
                render::BlitOpts opts;
                opts.mask   = fv.mask;
                opts.opaque = (fv.flags & anim::SPRITE_FLAG_OPAQUE) != 0;
                opts.flipH  = (fv.flags & anim::SPRITE_FLAG_FLIP_H) != 0;
                opts.flipV  = (fv.flags & anim::SPRITE_FLAG_FLIP_V) != 0;
                opts.rot90  = (fv.flags & anim::SPRITE_FLAG_ROT_90) != 0;
                opts.scale  = fv.scale;
                if (fv.scrollWindow != 0) {
                    opts.srcX    = fv.scrollX;
                    opts.srcW    = fv.scrollWindow;
                    opts.srcSpan = static_cast<uint16_t>(fv.w + fv.scrollGap);
                }
                render::drawBitmapMasked(
                    g, c.x, static_cast<int16_t>(c.y + bodyTop()), fv.data,
                    static_cast<int16_t>(fv.w), static_cast<int16_t>(fv.h),
                    opts, EPD_BLACK, EPD_WHITE);
                break;
            }
        }
    }
}

void CanvasView::commit(bool full_refresh) {
    if (full_refresh) {
        needsFullRefresh_ = true;
    }
    markDirty();
}

// --- Sprites -----------------------------------------------------------------

bool CanvasView::ensureSpriteArena() {
    if (sprites_.hasArena()) return true;
    spriteArena_ = cdc::core::psramAlloc<uint8_t>(SPRITE_ARENA);
    if (!spriteArena_) {
        LOG_E("CanvasView", "sprite arena allocation failed");
        return false;
    }
    sprites_.setArena(spriteArena_.get(), SPRITE_ARENA);
    return true;
}

int32_t CanvasView::spriteCreate(uint16_t w, uint16_t h, uint16_t frame_count,
                                 const uint8_t* data, uint32_t len) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!ensureSpriteArena()) return -1;
    return sprites_.create(w, h, frame_count, data, len);
}

bool CanvasView::spriteSetMask(uint32_t handle, const uint8_t* mask, uint32_t len) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    return sprites_.setMask(handle, mask, len);
}

bool CanvasView::spriteSetFlags(uint32_t handle, uint8_t flags) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    return sprites_.setFlags(handle, flags);
}

bool CanvasView::spriteSetScale(uint32_t handle, uint8_t scale) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    return sprites_.setScale(handle, scale);
}

// Render the text once into a MonoSurface, keep it as a single-frame sprite
// and let the store's scroll mode slide a window through it. The gap equals
// the window, so the text fully leaves before re-entering.
int32_t CanvasView::marquee(int16_t x, int16_t y, int16_t window_w,
                            const char* text, uint16_t step_px, uint16_t frame_ms) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    auto* g = gfx();
    if (!g || !text || window_w <= 0 || step_px == 0) return 0;
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return -1;
    if (!ensureSpriteArena()) return -1;

    const GFXfont* font = getGfxFont(fontId_);
    g->setTextSize(textSize_);
    int16_t  bx, by;
    uint16_t bw, bh;
    render::measureText(g, text, font, 0, 0, &bx, &by, &bw, &bh);
    if (bw == 0 || bh == 0) return 0;
    // Cap the strip so one line can never exhaust the arena.
    if (bw > 1024) bw = 1024;

    MonoSurface strip(bw, static_cast<uint16_t>(bh + 2));
    if (!strip.ok()) return -1;
    strip.setFont(font);
    strip.setTextSize(textSize_);
    strip.setTextColor(1);
    strip.setTextWrap(false);
    strip.setCursor(static_cast<int16_t>(-bx), static_cast<int16_t>(-by));
    render::drawText(&strip, text, font);

    int32_t handle = sprites_.create(bw, static_cast<uint16_t>(bh + 2), 1,
                                     strip.buffer(), strip.byteSize());
    if (handle <= 0) return handle;
    sprites_.scroll(static_cast<uint32_t>(handle), static_cast<uint16_t>(window_w),
                    step_px, static_cast<uint16_t>(window_w), frame_ms, nowMs());

    DrawCmd c{};
    c.type   = CmdType::Sprite;
    c.x      = x;
    c.y      = y;
    c.strOff = static_cast<uint16_t>(handle);
    pushCmd(c);

    animActive_ = true;
    idleCleanupAtMs_ = 0;
    markDirty();
    return handle;
}

bool CanvasView::spriteSetFrame(uint32_t handle, uint16_t frame) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    return sprites_.setFrame(handle, frame);
}

int32_t CanvasView::spriteGetFrame(uint32_t handle) const {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    return sprites_.frame(handle);
}

bool CanvasView::spriteSetFrameDurations(uint32_t handle, const uint16_t* ms,
                                         uint16_t count) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    return sprites_.setFrameDurations(handle, ms, count);
}

bool CanvasView::spritePlay(uint32_t handle, uint8_t mode, uint16_t frame_ms,
                            uint16_t repeat, uint32_t done_action_id) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!sprites_.play(handle, mode, frame_ms, repeat, done_action_id, nowMs())) {
        return false;
    }
    animActive_ = true;
    idleCleanupAtMs_ = 0;
    markDirty();
    return true;
}

bool CanvasView::spriteStop(uint32_t handle) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    return sprites_.stop(handle);
}

bool CanvasView::spriteDestroy(uint32_t handle) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    return sprites_.destroy(handle);
}

bool CanvasView::spriteFrameView(uint32_t handle,
                                 anim::SpriteStore::FrameView* out) const {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    return sprites_.frameView(handle, out);
}

void CanvasView::drawSprite(int16_t x, int16_t y, uint32_t handle) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!cmds_ || cmdCount_ >= MAX_CMDS) return;
    anim::SpriteStore::FrameView fv;
    if (!sprites_.frameView(handle, &fv)) return;
    DrawCmd c{};
    c.type   = CmdType::Sprite;
    c.x      = x;
    c.y      = y;
    c.strOff = static_cast<uint16_t>(handle);
    pushCmd(c);
}

// --- Tweens ------------------------------------------------------------------

uint32_t CanvasView::animStart(const anim::TweenConfig& cfg) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!findElem(cfg.elemId)) return 0;
    uint32_t handle = animator_.start(cfg, nowMs());
    if (handle != 0) {
        animActive_ = true;
        idleCleanupAtMs_ = 0;
        markDirty();
    }
    return handle;
}

uint32_t CanvasView::animBlink(uint32_t elem_id, uint16_t period_ms,
                               uint16_t count, uint32_t done_action_id) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    if (!findElem(elem_id)) return 0;
    uint32_t handle = animator_.blink(elem_id, period_ms, count,
                                      done_action_id, nowMs());
    if (handle != 0) {
        animActive_ = true;
        idleCleanupAtMs_ = 0;
    }
    return handle;
}

bool CanvasView::animCancel(uint32_t handle) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    return animator_.cancel(handle);
}

bool CanvasView::animPause(uint32_t handle, bool paused) {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    return animator_.pause(handle, paused, nowMs());
}

int8_t CanvasView::animState(uint32_t handle) const {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    return animator_.state(handle);
}

uint8_t CanvasView::animActiveCount() const {
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    uint8_t n = animator_.activeCount();
    // Playing sprites count as active animations for the caller.
    return sprites_.anyPlaying() ? static_cast<uint8_t>(n + 1) : n;
}

bool CanvasView::setAnimPolicy(uint8_t policy, uint8_t max_fps) {
    if (policy > ANIM_REFRESH_LIGHT || max_fps > ANIM_FPS_MAX) return false;
    cdc::core::RecursiveMutexGuard guard(editMutex_);
    animPolicy_ = policy;
    uint8_t fps = max_fps == 0 ? ANIM_FPS_DEFAULT : max_fps;
    animFrameIntervalMs_ = static_cast<uint16_t>(1000 / fps);
    return true;
}

// --- anim::AnimTarget (invoked under editMutex_) -------------------------------

bool CanvasView::getElemOffset(uint32_t elemId, int16_t* ox, int16_t* oy) {
    Elem* e = findElem(elemId);
    if (!e) return false;
    *ox = e->ox;
    *oy = e->oy;
    return true;
}

bool CanvasView::setElemOffset(uint32_t elemId, int16_t ox, int16_t oy) {
    Elem* e = findElem(elemId);
    if (!e) return false;
    e->ox = ox;
    e->oy = oy;
    return true;
}

bool CanvasView::setElemVisible(uint32_t elemId, bool visible) {
    Elem* e = findElem(elemId);
    if (!e) return false;
    e->hidden = !visible;
    return true;
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
    // Time passed under a covering view/modal must not fast-forward
    // animations: shift every time base by the pause duration.
    if (pausedAtMs_ != 0) {
        uint32_t delta = nowMs() - pausedAtMs_;
        pausedAtMs_ = 0;
        cdc::core::RecursiveMutexGuard guard(editMutex_);
        animator_.shiftTime(delta);
        sprites_.shiftTime(delta);
        if (idleCleanupAtMs_ != 0) idleCleanupAtMs_ += delta;
    }
}

void CanvasView::onPause() {
    pausedAtMs_ = nowMs();
    ViewBase::onPause();
}

void CanvasView::onExit() {
    if (auto* kp = hal::getKeypadInstance()) {
        kp->setDeferShortPress(hal::IKeypad::DEFER_SRC_VIEW, false);
        kp->setKeyRepeat(0, 0);
    }
    // Stop the engines without firing completion events into a view that is
    // going away.
    {
        cdc::core::RecursiveMutexGuard guard(editMutex_);
        animator_.reset();
        if (sprites_.hasArena()) sprites_.setArena(spriteArena_.get(), SPRITE_ARENA);
        animActive_ = false;
        idleCleanupAtMs_ = 0;
    }
    ViewBase::onExit();
}

// Animation clock: runs on the UI task inside ViewStack::dispatchTick, i.e.
// in the same loop iteration as a subsequent render. Advancing at the FPS cap
// (time-based, so slow refreshes drop frames instead of slowing motion) and
// completion dispatch happens outside the edit mutex.
void CanvasView::onTick(uint32_t nowMs) {
    if (pausedAtMs_ != 0) return;  // covered by a modal; frozen

    anim::Completion done[ANIM_DONE_CAP];
    uint8_t doneCount = 0;
    bool cleanup = false;

    {
        cdc::core::RecursiveMutexGuard guard(editMutex_);
        bool wasActive = animator_.activeCount() > 0 || sprites_.anyPlaying();
        if (wasActive
            && static_cast<uint32_t>(nowMs - lastAnimStepMs_) >= animFrameIntervalMs_) {
            lastAnimStepMs_ = nowMs;
            bool changed = false;
            uint8_t n = 0;
            changed |= animator_.advance(nowMs, *this, done,
                                         ANIM_DONE_CAP, &n);
            doneCount = n;
            n = 0;
            changed |= sprites_.advance(nowMs, done + doneCount,
                                        static_cast<uint8_t>(ANIM_DONE_CAP - doneCount),
                                        &n);
            doneCount = static_cast<uint8_t>(doneCount + n);
            if (changed) {
                markDirty();
                if (animPolicy_ == ANIM_REFRESH_AUTO
                    && ++animCommitCount_ >= ANIM_HYGIENE_COMMITS) {
                    // Endless animations never reach the idle cleanup; restore
                    // the panel's DC balance with a rare single flash.
                    animCommitCount_ = 0;
                    cleanup = true;
                }
            }
        }
        bool nowActive = animator_.activeCount() > 0 || sprites_.anyPlaying();
        if (wasActive && !nowActive && animPolicy_ == ANIM_REFRESH_AUTO) {
            idleCleanupAtMs_ = nowMs + ANIM_CLEANUP_IDLE_MS;
        }
        if (nowActive) idleCleanupAtMs_ = 0;
        animActive_ = nowActive;
    }

    if (idleCleanupAtMs_ != 0
        && static_cast<int32_t>(nowMs - idleCleanupAtMs_) >= 0) {
        idleCleanupAtMs_ = 0;
        cleanup = true;
    }

    // Outside the lock: plugin dispatch may re-enter canvas host calls.
    for (uint8_t i = 0; i < doneCount; ++i) {
        if (animCb_) animCb_(done[i].doneActionId, done[i].handle, done[i].refId);
    }
    if (cleanup) {
        ViewStack::instance().forceRefresh(hal::RefreshMode::FAST);
    }
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
    // The replay leaves the plugin's font/size/color set, but the footer below
    // sets its own style and any view pushed on top is reset centrally in
    // ViewStack::render, so no local restore is needed here.

    const char* hint = customFooter_ ? customFooter_ : footer_;
    render::drawFooterBar(g, width, height, nullptr, hint, hint != nullptr);

    dirty_ = false;
}

}  // namespace cdc::ui
