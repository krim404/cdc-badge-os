/**
 * InfoView Implementation
 *
 * Scrollable text display for help screens, about pages, etc.
 */

#include "cdc_views/InfoView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/LayoutConstants.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_ui/I18n.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"
#include <goodisplay/gdey029T94.h>
#include <cstring>

static const char* TAG = "InfoView";

/**
 * \brief View-local layout constants.
 *
 * Shared values (FOOTER_HEIGHT, SCROLL_INDICATOR_WIDTH) come from
 * cdc::ui::layout in LayoutConstants.h.
 */
static constexpr int TITLE_Y = 5;
static constexpr int TEXT_START_Y = 28;
static constexpr int TEXT_MARGIN = 8;
using cdc::ui::layout::FOOTER_HEIGHT;
using cdc::ui::layout::SCROLL_INDICATOR_WIDTH;

namespace cdc::ui {

/**
 * \brief Initializes title/text buffers and resets scrolling state.
 * \param title Title text for the info view.
 * \param text Body text content.
 * \return void
 */
namespace {

constexpr uint16_t WRAP_COLS = 44;

void wrapInPlace(char* buf, uint16_t maxCols) {
    if (!buf || maxCols == 0) return;
    uint16_t col = 0;
    char* lastSpace = nullptr;
    for (char* p = buf; *p; ++p) {
        if (*p == '\n') {
            col = 0;
            lastSpace = nullptr;
            continue;
        }
        if (col >= maxCols && lastSpace) {
            *lastSpace = '\n';
            col = static_cast<uint16_t>(p - lastSpace - 1);
            lastSpace = nullptr;
        }
        if (*p == ' ') lastSpace = p;
        ++col;
    }
}

}  // namespace

void InfoView::init(const char* title, const char* text) {
    if (title) {
        strncpy(titleBuf_, title, MAX_TITLE_LEN - 1);
        titleBuf_[MAX_TITLE_LEN - 1] = '\0';
    } else {
        titleBuf_[0] = '\0';
    }

    if (text) {
        strncpy(textBuf_, text, MAX_TEXT_LEN - 1);
        textBuf_[MAX_TEXT_LEN - 1] = '\0';
    } else {
        textBuf_[0] = '\0';
    }

    wrapInPlace(textBuf_, WRAP_COLS);

    scrollLine_ = 0;
    totalLines_ = countLines();
    customHint_ = nullptr;
    dirty_ = true;

    LOG_D(TAG, "init: title='%s', lines=%d", titleBuf_, totalLines_);
}

/**
 * \brief Counts newline-separated lines in the current text buffer.
 * \return Number of text lines.
 */
uint16_t InfoView::countLines() const {
    if (textBuf_[0] == '\0') return 0;

    uint16_t lines = 1;
    const char* p = textBuf_;
    while (*p) {
        if (*p == '\n') lines++;
        p++;
    }
    return lines;
}

/**
 * \brief Scrolls content up or down with wrap-around behavior.
 * \param down `true` to scroll down, `false` to scroll up.
 * \return void
 */
void InfoView::scroll(bool down) {
    if (totalLines_ <= VISIBLE_LINES) return;

    if (down) {
        if (scrollLine_ < totalLines_ - VISIBLE_LINES) {
            scrollLine_++;
        } else {
            // Wrap to top
            scrollLine_ = 0;
        }
    } else {
        if (scrollLine_ > 0) {
            scrollLine_--;
        } else {
            // Wrap to bottom
            scrollLine_ = totalLines_ - VISIBLE_LINES;
        }
    }

    dirty_ = true;
}

/**
 * \brief Handles key input for scrolling and optional callbacks.
 * \param key Pressed key code.
 * \return Input handling result for the view stack.
 */
InputResult InfoView::onKey(char key) {
    if (key == KEY_YES && onYes_) {
        onYes_(callbackUserData_);
        return InputResult::CONSUMED;
    }
    if (key == KEY_NO && onNo_) {
        onNo_(callbackUserData_);
        return InputResult::CONSUMED;
    }

    switch (key) {
        case KEY_UP:
            scroll(false);
            return InputResult::CONSUMED;

        case KEY_DOWN:
            scroll(true);
            return InputResult::CONSUMED;

        case KEY_NO:  // Back
        case KEY_YES: // Also back (info is read-only)
            return InputResult::REQUEST_POP;

        default:
            return InputResult::IGNORED;
    }
}

/**
 * \brief Returns the footer hint text.
 * \return Footer hint string.
 */
const char* InfoView::getFooterHint() const {
    if (customHint_) {
        return customHint_;
    }
    return ui::tr("core.hint_scroll_back");
}

/**
 * \brief Renders the info view including title, body, scroll indicator, and footer.
 * \param partial Indicates partial/full redraw mode.
 * \return void
 */
void InfoView::render(bool partial) {
    hal::IDisplay* display = hal::getDisplayInstance();
    if (!display) return;

    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;

    const uint16_t width = display->getWidth();
    const uint16_t height = display->getHeight();

    if (!partial) {
        gfx->fillScreen(EPD_WHITE);
    }

    gfx->setFont(nullptr);  // 6x8 built-in (CP437): never inherit a leaked GFX font
    gfx->setTextColor(EPD_BLACK);
    gfx->setTextSize(1);

    // Title + underline
    const char* title = (titleBuf_[0] != '\0') ? titleBuf_ : nullptr;
    render::drawHeaderLeft(gfx, title, TEXT_MARGIN, TITLE_Y, width);

    // Text area dimensions
    int textAreaWidth = width - TEXT_MARGIN * 2 - SCROLL_INDICATOR_WIDTH;
    int textAreaHeight = height - TEXT_START_Y - FOOTER_HEIGHT;

    // Clear text area
    gfx->fillRect(TEXT_MARGIN, TEXT_START_Y, textAreaWidth, textAreaHeight, EPD_WHITE);

    // Render visible lines
    if (textBuf_[0] != '\0') {
        const char* lineStart = textBuf_;
        uint16_t currentLine = 0;
        int y = TEXT_START_Y;

        // Skip to scroll position
        while (currentLine < scrollLine_ && *lineStart) {
            if (*lineStart == '\n') currentLine++;
            lineStart++;
        }

        // Render visible lines
        for (uint8_t i = 0; i < VISIBLE_LINES && *lineStart; i++) {
            gfx->setCursor(TEXT_MARGIN, y);

            // Find end of line
            const char* lineEnd = lineStart;
            while (*lineEnd && *lineEnd != '\n') lineEnd++;

            // Print line (character by character to handle no null-terminator)
            while (lineStart < lineEnd) {
                gfx->print(*lineStart);
                lineStart++;
            }

            // Skip newline
            if (*lineStart == '\n') lineStart++;

            y += LINE_HEIGHT;
        }
    }

    // Scroll indicators (if needed)
    if (totalLines_ > VISIBLE_LINES) {
        int indicatorX = width - SCROLL_INDICATOR_WIDTH;
        int listHeight = VISIBLE_LINES * LINE_HEIGHT;
        render::drawScrollIndicator(gfx, indicatorX, TEXT_START_Y, listHeight,
                                    totalLines_, VISIBLE_LINES, scrollLine_);
    }

    // Footer
    char posStr[20];  // Max: "65535-65535/65535  \0"
    const char* prefix = nullptr;
    if (totalLines_ > VISIBLE_LINES) {
        snprintf(posStr, sizeof(posStr), "%u-%u/%u  ",
                 scrollLine_ + 1,
                 scrollLine_ + VISIBLE_LINES > totalLines_ ? totalLines_ : scrollLine_ + VISIBLE_LINES,
                 totalLines_);
        prefix = posStr;
    }
    const char* hint = getFooterHint();
    render::drawFooterBar(gfx, width, height, prefix, hint, true);

    dirty_ = false;
}

/**
 * \brief Convenience factory/helper function.
 */

static InfoView s_sharedInfoView;

/**
 * \brief Shows a shared info view instance and pushes it onto the view stack.
 * \param title Title text.
 * \param text Body text.
 * \param hint Optional custom footer hint.
 * \return Pointer to the shared `InfoView` instance.
 */
InfoView* showInfo(const char* title, const char* text, const char* hint) {
    s_sharedInfoView.init(title, text);
    if (hint) {
        s_sharedInfoView.setHint(hint);
    }
    ViewStack::instance().push(&s_sharedInfoView);
    return &s_sharedInfoView;
}

} // namespace cdc::ui
