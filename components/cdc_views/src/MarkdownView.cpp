/**
 * MarkdownView Implementation
 *
 * Renders a parsed Markdown source as scrollable, multi-font visual rows.
 */

#include "cdc_views/MarkdownView.h"

#include "cdc_views/Fonts.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/LayoutConstants.h"
#include "cdc_views/HtmlViewerHook.h"
#include "cdc_views/MarkdownParser.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"

#include <goodisplay/gdey029T94.h>

#include <cstring>
#include <cstdio>

static const char* TAG = "MarkdownView";

static constexpr int TITLE_Y = 5;
static constexpr int TEXT_START_Y = 28;
static constexpr int TEXT_MARGIN = 8;
static constexpr int INDENT_PX = 10;       // per nesting level
static constexpr int BLANK_HEIGHT = 6;
static constexpr int RULE_HEIGHT = 10;
using cdc::ui::layout::FOOTER_HEIGHT;
using cdc::ui::layout::SCROLL_INDICATOR_WIDTH;

namespace cdc::ui {

namespace {

/// Monospace glyph advance for a font id (built-in is 6px).
int charWidth(uint8_t fontId) {
    const GFXfont* f = getGfxFont(fontId);
    if (!f || !f->glyph) return 6;
    int adv = f->glyph[0].xAdvance;
    return adv > 0 ? adv : 6;
}

/// Row height in pixels for a font id (built-in classic font is 8px tall).
int fontHeight(uint8_t fontId) {
    const GFXfont* f = getGfxFont(fontId);
    if (!f) return 12;
    return f->yAdvance;
}

/// Baseline offset (pixels below the row top) for drawing with the font.
int ascentOffset(Gdey029T94* gfx, uint8_t fontId) {
    const GFXfont* f = getGfxFont(fontId);
    if (!f) return 0;  // built-in font uses a top-left origin
    int16_t x1 = 0, y1 = 0;
    uint16_t w = 0, h = 0;
    render::measureText(gfx, "Mg", f, 0, 0, &x1, &y1, &w, &h);
    return -y1;
}

/// Copies a row span into `out`, dropping inline emphasis markers (* _ `).
void stripInline(const char* text, uint16_t len, char* out, size_t outCap) {
    size_t o = 0;
    for (uint16_t i = 0; i < len && o + 1 < outCap; ++i) {
        const char c = text[i];
        if (c == '*' || c == '_' || c == '`' || c == '\x10' || c == '\x11' || c == '\x12') continue;
        out[o++] = c;
    }
    out[o] = '\0';
}

}  // namespace

void MarkdownView::addRow(const char* text, uint16_t len, uint8_t font, uint8_t indent,
                          MdLineKind kind, bool inverted, bool first) {
    if (!rows_ || rowCount_ >= MAX_ROWS) {
        truncated_ = true;
        return;
    }
    Row& r = rows_.get()[rowCount_++];
    r.text = text;
    r.len = len;
    r.font = font;
    r.indent = indent;
    r.kind = kind;
    r.inverted = inverted;
    r.first = first;
}

void MarkdownView::appendParsed(const StyledLine& line) {
    if (line.kind == MdLineKind::Blank || line.kind == MdLineKind::Rule) {
        addRow(line.text, 0, mdfont::Builtin, line.indent, line.kind, false, true);
        return;
    }

    const int markerCols = (line.kind == MdLineKind::Bullet ||
                            line.kind == MdLineKind::Ordered ||
                            line.kind == MdLineKind::Quote)
                               ? 2
                               : 0;
    const int cw = charWidth(line.font);
    int avail = textAreaWidth_ - line.indent * INDENT_PX - markerCols * cw;
    int maxCols = avail / (cw > 0 ? cw : 6);
    if (maxCols < 4) maxCols = 4;

    // Word-wrap the span into <= maxCols chunks, breaking on spaces.
    uint16_t start = 0;
    bool first = true;
    while (start < line.len) {
        uint16_t remaining = static_cast<uint16_t>(line.len - start);
        uint16_t take = remaining <= maxCols ? remaining : static_cast<uint16_t>(maxCols);
        if (take < remaining) {
            // back up to the last space within [start, start+take]
            uint16_t brk = take;
            while (brk > 0 && line.text[start + brk] != ' ') --brk;
            if (brk > 0) take = brk;
        }
        addRow(line.text + start, take, line.font, line.indent, line.kind,
               line.inverted, first);
        start = static_cast<uint16_t>(start + take);
        while (start < line.len && line.text[start] == ' ') ++start;  // skip the break space
        first = false;
    }
    if (line.len == 0) {
        addRow(line.text, 0, line.font, line.indent, line.kind, line.inverted, true);
    }
}

namespace {
struct RowSink : StyledLineSink {
    MarkdownView* view;
    explicit RowSink(MarkdownView* v) : view(v) {}
    void emit(const StyledLine& l) override { view->appendParsed(l); }
};
}  // namespace

void MarkdownView::init(const char* title, const char* src, size_t len) {
    if (title) {
        strncpy(titleBuf_, title, MAX_TITLE_LEN - 1);
        titleBuf_[MAX_TITLE_LEN - 1] = '\0';
    } else {
        titleBuf_[0] = '\0';
    }

    if (!srcBuf_) srcBuf_ = cdc::core::psramAlloc<char>(MAX_SOURCE);
    if (!rows_) rows_ = cdc::core::psramAlloc<Row>(MAX_ROWS);
    if (!interact_) interact_ = cdc::core::psramAlloc<InteractRef>(MAX_INTERACT);

    rowCount_ = 0;
    scrollRow_ = 0;
    truncated_ = false;
    plain_ = false;
    highlightRow_ = -1;
    interactCount_ = 0;
    interactSel_ = -1;
    inlineLinkCount_ = 0;
    linkPoolLen_ = 0;
    checkSave_ = nullptr;
    checkSaveUd_ = nullptr;

    hal::IDisplay* display = hal::getDisplayInstance();
    const uint16_t width = display ? display->getWidth() : 296;
    textAreaWidth_ = width - TEXT_MARGIN * 2 - SCROLL_INDICATOR_WIDTH;

    if (srcBuf_ && rows_) {
        size_t n = len < (MAX_SOURCE - 1) ? len : (MAX_SOURCE - 1);
        std::memcpy(srcBuf_.get(), src ? src : "", n);
        srcBuf_.get()[n] = '\0';
        rewriteInlineLinks();  // "[text](url)" -> "\x10text\x11", url pool
        size_t pn = std::strlen(srcBuf_.get());

        RowSink sink(this);
        auto r = parseMarkdown(srcBuf_.get(), pn, sink, MAX_SOURCE);
        if (r.truncated || len >= MAX_SOURCE) truncated_ = true;
    }

    rebuildInteractive();
    dirty_ = true;
    LOG_D(TAG, "init: title='%s', rows=%u, trunc=%d", titleBuf_, rowCount_, truncated_);
}

void MarkdownView::initPlain(const char* title, const char* src, size_t len) {
    if (title) {
        strncpy(titleBuf_, title, MAX_TITLE_LEN - 1);
        titleBuf_[MAX_TITLE_LEN - 1] = '\0';
    } else {
        titleBuf_[0] = '\0';
    }

    if (!srcBuf_) srcBuf_ = cdc::core::psramAlloc<char>(MAX_SOURCE);
    if (!rows_) rows_ = cdc::core::psramAlloc<Row>(MAX_ROWS);
    if (!interact_) interact_ = cdc::core::psramAlloc<InteractRef>(MAX_INTERACT);

    rowCount_ = 0;
    scrollRow_ = 0;
    truncated_ = false;
    plain_ = true;
    highlightRow_ = -1;
    interactCount_ = 0;
    interactSel_ = -1;
    inlineLinkCount_ = 0;
    linkPoolLen_ = 0;
    checkSave_ = nullptr;
    checkSaveUd_ = nullptr;

    hal::IDisplay* display = hal::getDisplayInstance();
    const uint16_t width = display ? display->getWidth() : 296;
    textAreaWidth_ = width - TEXT_MARGIN * 2 - SCROLL_INDICATOR_WIDTH;

    if (srcBuf_ && rows_) {
        size_t n = len < (MAX_SOURCE - 1) ? len : (MAX_SOURCE - 1);
        if (len >= MAX_SOURCE) truncated_ = true;
        std::memcpy(srcBuf_.get(), src ? src : "", n);
        srcBuf_.get()[n] = '\0';

        const int cw = charWidth(mdfont::Builtin);
        int maxCols = textAreaWidth_ / (cw > 0 ? cw : 6);
        if (maxCols < 4) maxCols = 4;

        const char* base = srcBuf_.get();
        uint32_t off = 0;
        while (off < n) {
            uint32_t nl = off;
            while (nl < n && base[nl] != '\n') ++nl;
            uint16_t lineLen = static_cast<uint16_t>(nl - off);
            if (lineLen == 0) {
                addRow(base + off, 0, mdfont::Builtin, 0, MdLineKind::Paragraph, false, true);
            }
            uint16_t start = 0;
            bool first = true;
            while (start < lineLen) {
                uint16_t remaining = static_cast<uint16_t>(lineLen - start);
                uint16_t take = remaining <= maxCols ? remaining : static_cast<uint16_t>(maxCols);
                if (take < remaining) {
                    uint16_t brk = take;
                    while (brk > 0 && base[off + start + brk] != ' ') --brk;
                    if (brk > 0) take = brk;
                }
                addRow(base + off + start, take, mdfont::Builtin, 0, MdLineKind::Paragraph, false,
                       first);
                start = static_cast<uint16_t>(start + take);
                while (start < lineLen && base[off + start] == ' ') ++start;
                first = false;
            }
            off = (nl < n) ? nl + 1 : nl;
        }
    }

    dirty_ = true;
    LOG_D(TAG, "initPlain: title='%s', rows=%u, trunc=%d", titleBuf_, rowCount_, truncated_);
}

uint16_t MarkdownView::visibleRowCount() const {
    return rowCount_;
}

void MarkdownView::onExit() {
    srcBuf_.reset();
    rows_.reset();
    interact_.reset();
    linkPool_.reset();
    linkUrlOff_.reset();
    rowCount_ = 0;
    interactCount_ = 0;
    interactSel_ = -1;
    inlineLinkCount_ = 0;
    linkPoolLen_ = 0;
}

const char* MarkdownView::getFooterHint() const {
    return ui::tr("core.hint_scroll_back");
}

InputResult MarkdownView::onKey(char key) {
    switch (key) {
        case KEY_UP:
            if (scrollRow_ > 0) { scrollRow_--; dirty_ = true; }
            return InputResult::CONSUMED;
        case KEY_DOWN:
            if (scrollRow_ + 1 < rowCount_) { scrollRow_++; dirty_ = true; }
            return InputResult::CONSUMED;
        case '4':
            if (interactCount_ == 0) return InputResult::IGNORED;
            cycleInteractive(-1);
            return InputResult::CONSUMED;
        case '6':
            if (interactCount_ == 0) return InputResult::IGNORED;
            cycleInteractive(+1);
            return InputResult::CONSUMED;
        case KEY_YES:
            if (!selectedRef()) return InputResult::IGNORED;
            activateSelected();
            return InputResult::CONSUMED;
        case KEY_NO:
            return InputResult::REQUEST_POP;
        default:
            return InputResult::IGNORED;
    }
}

InputResult MarkdownView::onLongPress(char key) {
    if (rowCount_ == 0) return InputResult::IGNORED;
    switch (key) {
        case KEY_UP:   scrollRow_ = 0; dirty_ = true; return InputResult::CONSUMED;
        case KEY_DOWN: scrollRow_ = static_cast<uint16_t>(rowCount_ - 1); dirty_ = true; return InputResult::CONSUMED;
        default:       return InputResult::IGNORED;
    }
}

void MarkdownView::selectMarker(uint16_t linkNum) {
    highlightRow_ = -1;
    if (linkNum != 0 && rows_) {
        char needle[12];
        std::snprintf(needle, sizeof(needle), "[%u]", static_cast<unsigned>(linkNum));
        size_t nl = std::strlen(needle);
        for (uint16_t i = 0; i < rowCount_ && highlightRow_ < 0; ++i) {
            const Row& r = rows_.get()[i];
            if (r.len < nl) continue;
            for (uint16_t j = 0; j + nl <= r.len; ++j) {
                if (std::memcmp(r.text + j, needle, nl) == 0) {
                    highlightRow_ = static_cast<int>(i);
                    break;
                }
            }
        }
        if (highlightRow_ >= 0) scrollRow_ = static_cast<uint16_t>(highlightRow_);
    }
    dirty_ = true;
}

void MarkdownView::mdRowInfo(uint16_t i, const char*& text, uint16_t& len, bool& first,
                             uint32_t& srcOff) const {
    const Row& r = rows_.get()[i];
    text = r.text;
    len = r.len;
    first = r.first;
    srcOff = srcBuf_ ? static_cast<uint32_t>(r.text - srcBuf_.get()) : 0;
}

void MarkdownView::addInteract(uint16_t row, Interact kind, uint16_t id, uint32_t srcOff) {
    if (!interact_ || interactCount_ >= MAX_INTERACT) return;
    interact_.get()[interactCount_++] = InteractRef{row, kind, id, srcOff};
}

const MarkdownView::InteractRef* MarkdownView::selectedRef() const {
    if (interactSel_ < 0 || interactSel_ >= static_cast<int>(interactCount_)) return nullptr;
    return &interact_.get()[interactSel_];
}

namespace {
// A task-list checkbox at the start of a list item: "[ ]", "[x]" or "[X]".
bool checkboxAt(const char* t, uint16_t len) {
    return len >= 3 && t[0] == '[' && t[2] == ']' &&
           (t[1] == ' ' || t[1] == 'x' || t[1] == 'X');
}
}  // namespace

void MarkdownView::rewriteInlineLinks() {
    inlineLinkCount_ = 0;
    linkPoolLen_ = 0;
    if (!srcBuf_) return;
    char* s = srcBuf_.get();
    size_t rd = 0, wr = 0;
    while (s[rd]) {
        bool image = (s[rd] == '!' && s[rd + 1] == '[');
        if (s[rd] == '[' || image) {
            size_t tb = image ? rd + 2 : rd + 1;  // text begin
            size_t te = tb;
            while (s[te] && s[te] != ']' && s[te] != '\n') ++te;
            if (s[te] == ']' && s[te + 1] == '(') {
                size_t ub = te + 2, ue = ub;  // url
                while (s[ue] && s[ue] != ')' && s[ue] != '\n') ++ue;
                size_t urlLen = ue - ub;
                if (s[ue] == ')' && urlLen > 0) {
                    if (!linkPool_) linkPool_ = cdc::core::psramAlloc<char>(kLinkPoolCap);
                    if (!linkUrlOff_) linkUrlOff_ = cdc::core::psramAlloc<uint16_t>(MAX_INTERACT);
                    bool room = linkPool_ && linkUrlOff_ && inlineLinkCount_ < MAX_INTERACT &&
                                linkPoolLen_ + urlLen + 1 <= kLinkPoolCap;
                    if (room) {
                        linkUrlOff_.get()[inlineLinkCount_++] = linkPoolLen_;
                        std::memcpy(linkPool_.get() + linkPoolLen_, s + ub, urlLen);
                        linkPool_.get()[linkPoolLen_ + urlLen] = '\0';
                        linkPoolLen_ = static_cast<uint16_t>(linkPoolLen_ + urlLen + 1);
                        s[wr++] = image ? '\x12' : '\x10';
                        for (size_t k = tb; k < te; ++k) s[wr++] = s[k];
                        s[wr++] = '\x11';
                        rd = ue + 1;
                        continue;
                    }
                }
            }
        }
        s[wr++] = s[rd++];
    }
    s[wr] = '\0';
}

void MarkdownView::rebuildInteractive() {
    clearInteract();
    if (!rows_ || plain_) return;
    uint16_t ordinal = 0;
    for (uint16_t i = 0; i < rowCount_; ++i) {
        const Row& r = rows_.get()[i];
        const uint32_t base = srcBuf_ ? static_cast<uint32_t>(r.text - srcBuf_.get()) : 0;

        // Task-list checkbox: only on a list item, so a "[x](url)" link is not one.
        if (r.first && (r.kind == MdLineKind::Bullet || r.kind == MdLineKind::Ordered) &&
            checkboxAt(r.text, r.len)) {
            addInteract(i, Interact::Check, 0, base + 1);
        }

        // Inline links/images are delimited (\x10 link / \x12 image, both end \x11).
        for (uint16_t j = 0; j < r.len; ++j) {
            char c = r.text[j];
            if (c == '\x10') addInteract(i, Interact::Link, ordinal++, base + j);
            else if (c == '\x12') addInteract(i, Interact::Image, ordinal++, base + j);
        }
    }
}

void MarkdownView::cycleInteractive(int dir) {
    if (interactCount_ == 0) return;
    if (interactSel_ < 0) {
        interactSel_ = (dir > 0) ? 0 : interactCount_ - 1;
    } else {
        interactSel_ = ((interactSel_ + dir) % interactCount_ + interactCount_) % interactCount_;
    }
    const InteractRef& it = interact_.get()[interactSel_];
    highlightRow_ = static_cast<int>(it.row);
    scrollRow_ = it.row;
    dirty_ = true;
}

bool MarkdownView::flipCheckChar(uint32_t srcOff) {
    if (!srcBuf_ || srcOff == 0) return false;
    char* p = srcBuf_.get() + srcOff;
    bool nowChecked = (*p == ' ');
    *p = nowChecked ? 'x' : ' ';
    dirty_ = true;
    return nowChecked;
}

void MarkdownView::fireCheckSave() {
    if (checkSave_ && srcBuf_) checkSave_(checkSaveUd_, srcBuf_.get(), std::strlen(srcBuf_.get()));
}

void MarkdownView::activateSelected() {
    const InteractRef* it = selectedRef();
    if (!it) return;
    if (it->kind == Interact::Check) {
        flipCheckChar(it->srcOff);
        fireCheckSave();
    } else if (it->kind == Interact::Link || it->kind == Interact::Image) {
        if (!linkPool_ || !linkUrlOff_ || it->id >= inlineLinkCount_) return;
        const char* url = linkPool_.get() + linkUrlOff_.get()[it->id];
        if (it->kind == Interact::Link) {
            if (auto open = urlOpener()) open(url);
        } else {
            if (auto open = imageOpener()) open(url);
        }
    }
}

void MarkdownView::drawStyledRow(void* gfxv, const char* text, uint16_t len, int x, int yTop,
                                 uint16_t fg) {
    auto* gfx = static_cast<Gdey029T94*>(gfxv);
    gfx->setFont(nullptr);
    bool bold = false, strike = false, link = false;
    int cx = x;
    for (uint16_t i = 0; i < len;) {
        char c = text[i];
        if (c == '\x10' || c == '\x12') { link = true; ++i; continue; }   // link / image start
        if (c == '\x11') { link = false; ++i; continue; }                 // link / image end
        if (c == '`') { ++i; continue; }                                  // inline code marker
        if (c == '*' || c == '_') {
            if (i + 1 < len && text[i + 1] == c) { bold = !bold; i += 2; }  // ** / __ = bold
            else ++i;  // single * / _ = italic: not renderable in the bitmap font, shown plain
            continue;
        }
        if (c == '~' && i + 1 < len && text[i + 1] == '~') { strike = !strike; i += 2; continue; }
        gfx->setCursor(cx, yTop);
        gfx->write(static_cast<uint8_t>(c));
        if (bold) { gfx->setCursor(cx + 1, yTop); gfx->write(static_cast<uint8_t>(c)); }  // faux-bold
        if (link) gfx->drawLine(cx, yTop + 7, cx + 5, yTop + 7, fg);
        if (strike) gfx->drawLine(cx, yTop + 3, cx + 5, yTop + 3, fg);
        cx += 6;
        ++i;
    }
}

void MarkdownView::render(bool partial) {
    hal::IDisplay* display = hal::getDisplayInstance();
    if (!display) return;
    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;

    const uint16_t width = display->getWidth();
    const uint16_t height = display->getHeight();

    if (!partial) {
        gfx->fillScreen(EPD_WHITE);
    } else {
        gfx->fillRect(0, 0, width, height - FOOTER_HEIGHT, EPD_WHITE);
    }

    gfx->setFont(nullptr);
    gfx->setTextColor(EPD_BLACK);
    gfx->setTextSize(1);

    const char* title = (titleBuf_[0] != '\0') ? titleBuf_ : nullptr;
    render::drawHeaderLeft(gfx, title, TEXT_MARGIN, TITLE_Y, width);

    const int areaBottom = height - FOOTER_HEIGHT;
    int y = TEXT_START_Y;
    char buf[96];

    uint16_t shown = 0;
    for (uint16_t i = scrollRow_; i < rowCount_; ++i) {
        const Row& r = rows_.get()[i];

        if (r.kind == MdLineKind::Blank) {
            if (y + BLANK_HEIGHT > areaBottom) break;
            y += BLANK_HEIGHT;
            ++shown;
            continue;
        }
        if (r.kind == MdLineKind::Rule) {
            if (y + RULE_HEIGHT > areaBottom) break;
            gfx->drawLine(TEXT_MARGIN, y + RULE_HEIGHT / 2,
                          TEXT_MARGIN + textAreaWidth_, y + RULE_HEIGHT / 2, EPD_BLACK);
            y += RULE_HEIGHT;
            ++shown;
            continue;
        }

        const int h = fontHeight(r.font);
        if (y + h > areaBottom) break;

        int x = TEXT_MARGIN + r.indent * INDENT_PX;

        // Inverse background for code rows and the selected-marker row.
        const bool inv = r.inverted || (static_cast<int>(i) == highlightRow_);
        if (inv) {
            gfx->fillRect(x, y, textAreaWidth_ - r.indent * INDENT_PX, h, EPD_BLACK);
            gfx->setTextColor(EPD_WHITE);
        }

        // Block marker on the first visual row of a list item / quote.
        if (r.first && (r.kind == MdLineKind::Bullet || r.kind == MdLineKind::Ordered)) {
            gfx->setFont(nullptr);
            gfx->setCursor(x, y);
            gfx->print(static_cast<char>(0x07));  // CP437 bullet
            x += 2 * 6;
        } else if (r.kind == MdLineKind::Quote) {
            gfx->setFont(nullptr);
            gfx->setCursor(x, y);
            gfx->print(static_cast<char>(0xB3));  // CP437 vertical bar
            x += 2 * 6;
        }

        if (!plain_ && r.font == mdfont::Builtin) {
            // Body text: inline styling (bold/strikethrough + underlined links).
            drawStyledRow(gfx, r.text, r.len, x, y, inv ? EPD_WHITE : EPD_BLACK);
        } else {
            if (plain_) {
                uint16_t cl =
                    r.len < sizeof(buf) - 1 ? r.len : static_cast<uint16_t>(sizeof(buf) - 1);
                std::memcpy(buf, r.text, cl);
                buf[cl] = '\0';
            } else {
                stripInline(r.text, r.len, buf, sizeof(buf));
            }
            const GFXfont* font = getGfxFont(r.font);
            gfx->setFont(font);
            gfx->setCursor(x, y + ascentOffset(gfx, r.font));
            render::drawText(gfx, buf, font);
        }

        if (inv) gfx->setTextColor(EPD_BLACK);

        gfx->setFont(nullptr);
        y += h;
        ++shown;
    }

    if (rowCount_ > shown || scrollRow_ > 0) {
        const int indicatorX = width - SCROLL_INDICATOR_WIDTH;
        const int listHeight = areaBottom - TEXT_START_Y;
        render::drawScrollIndicator(gfx, indicatorX, TEXT_START_Y, listHeight,
                                    rowCount_, shown ? shown : 1, scrollRow_);
    }

    char posStr[24];
    const char* prefix = nullptr;
    if (truncated_) {
        prefix = ui::tr("core.md_truncated");
    } else if (rowCount_ > shown) {
        snprintf(posStr, sizeof(posStr), "%u/%u  ", scrollRow_ + 1, rowCount_);
        prefix = posStr;
    }
    render::drawFooterBar(gfx, width, height, prefix, getFooterHint(), true);

    dirty_ = false;
}

static MarkdownView s_sharedMarkdownView;

MarkdownView* showMarkdown(const char* title, const char* src, size_t len) {
    s_sharedMarkdownView.init(title, src, len);
    ViewStack::instance().push(&s_sharedMarkdownView);
    return &s_sharedMarkdownView;
}

MarkdownView* showPlainText(const char* title, const char* src, size_t len) {
    s_sharedMarkdownView.initPlain(title, src, len);
    ViewStack::instance().push(&s_sharedMarkdownView);
    return &s_sharedMarkdownView;
}

} // namespace cdc::ui
