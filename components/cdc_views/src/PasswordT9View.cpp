#include "cdc_views/PasswordT9View.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_ui/I18n.h"
#include "cdc_hal/IDisplay.h"
#include <goodisplay/gdey029T94.h>
#include <cstdio>

namespace cdc::ui {

/** \brief Layout constants mirror the ones used by T9InputView. */
static constexpr int TITLE_Y      = 5;
static constexpr int TEXT_Y       = 50;
static constexpr int TEXT_MARGIN  = 10;

/**
 * \brief Renders title, masked text box and footer.
 *
 * Layout mirrors T9InputView; the only differences are the character
 * substitution and the per-row reveal/cursor handling.
 */
void PasswordT9View::render(bool partial) {
    auto* display = hal::getDisplayInstance();
    if (!display) return;
    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;

    const uint16_t width  = display->getWidth();
    const uint16_t height = display->getHeight();

    if (!partial) gfx->fillScreen(EPD_WHITE);

    gfx->setTextColor(EPD_BLACK);
    gfx->setTextSize(1);

    render::drawHeaderLeft(gfx, title_, TEXT_MARGIN, TITLE_Y, width);

    // Input box
    gfx->fillRect(TEXT_MARGIN, TEXT_Y - 5, width - TEXT_MARGIN * 2, 30, EPD_WHITE);
    gfx->drawRect(TEXT_MARGIN - 2, TEXT_Y - 7, width - TEXT_MARGIN * 2 + 4, 34, EPD_BLACK);
    gfx->setCursor(TEXT_MARGIN + 2, TEXT_Y);

    if (len_ == 0 && placeholder_) {
        gfx->setTextColor(EPD_DARKGREY);
        render::printText(gfx, placeholder_);
        gfx->setTextColor(EPD_BLACK);
    } else {
        for (uint16_t i = 0; i < len_; i++) {
            const bool isCursorChar = cursorActive_ && i == len_ - 1;
            const bool showPlain    = revealed_ || isCursorChar;
            char c = showPlain ? text_[i] : maskChar_;

            if (isCursorChar) {
                int16_t x = gfx->getCursorX();
                int16_t y = gfx->getCursorY();
                gfx->fillRect(x, y - 2, 8, 14, EPD_BLACK);
                gfx->setTextColor(EPD_WHITE);
                gfx->print(c);
                gfx->setTextColor(EPD_BLACK);
            } else {
                gfx->print(c);
            }
        }
        if (!cursorActive_) {
            gfx->print("|");
        }
    }

    char countStr[32];
    snprintf(countStr, sizeof(countStr), "%u/%u %s",
             len_, maxLen_, revealed_ ? "(shown)" : "(hidden)");
    const char* hint = getFooterHint();
    render::drawFooterBar(gfx, width, height, countStr, hint, true);

    dirty_ = false;
}

/**
 * \brief Long-press on Y toggles the reveal state. Other keys delegate to T9.
 */
InputResult PasswordT9View::onLongPress(char key) {
    if (key == KEY_YES) {
        revealed_ = !revealed_;
        markDirty();
        return InputResult::CONSUMED;
    }
    return T9InputView::onLongPress(key);
}

const char* PasswordT9View::getFooterHint() const {
    return tr(revealed_ ? "core.hint_password_revealed"
                        : "core.hint_password_hidden");
}

} // namespace cdc::ui
