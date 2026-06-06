/**
 * DateInputView Implementation
 *
 * Date input with day/month/year fields.
 */

#include "cdc_views/DateInputView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"
#include <goodisplay/gdey029T94.h>
#include <cstdio>

static const char* TAG = "DateInputView";

/**
 * \brief Display layout constants.
 */
static constexpr int TITLE_Y = 20;
static constexpr int DATE_Y = 60;
static constexpr int UNDERLINE_Y = DATE_Y + 20;
static constexpr int HINT_Y = 90;

namespace cdc::ui {

/**
 * \brief Initializes date input state.
 * \param title View title text.
 * \param day Initial day value.
 * \param month Initial month value.
 * \param year Initial year value.
 * \return void
 */
void DateInputView::init(const char* title, uint8_t day, uint8_t month, uint16_t year) {
    title_ = title;
    day_ = (day >= 1 && day <= 31) ? day : 1;
    month_ = (month >= 1 && month <= 12) ? month : 1;
    year_ = year;
    currentField_ = Field::DAY;
    digitPos_ = 0;
    onCancel_ = nullptr;
    dirty_ = true;
}

/**
 * \brief Moves focus to the next date field.
 * \return void
 */
void DateInputView::nextField() {
    if (currentField_ == Field::DAY) {
        currentField_ = Field::MONTH;
    } else if (currentField_ == Field::MONTH) {
        currentField_ = Field::YEAR;
    }
    digitPos_ = 0;
    dirty_ = true;
}

/**
 * \brief Moves focus to the previous date field.
 * \return void
 */
void DateInputView::prevField() {
    if (currentField_ == Field::YEAR) {
        currentField_ = Field::MONTH;
    } else if (currentField_ == Field::MONTH) {
        currentField_ = Field::DAY;
    }
    digitPos_ = 0;
    dirty_ = true;
}

/**
 * \brief Clears the currently selected date field.
 * \return void
 */
void DateInputView::clearField() {
    switch (currentField_) {
        case Field::DAY:
            day_ = 0;
            break;
        case Field::MONTH:
            month_ = 0;
            break;
        case Field::YEAR:
            year_ = 0;
            break;
    }
    digitPos_ = 0;
    dirty_ = true;
}

/**
 * \brief Inserts a numeric digit into the active date field.
 * \param digit Numeric character (`'0'`..`'9'`).
 * \return void
 */
void DateInputView::enterDigit(char digit) {
    uint8_t d = digit - '0';

    switch (currentField_) {
        case Field::DAY: {
            if (digitPos_ == 0) {
                day_ = d * 10;
                digitPos_ = 1;
            } else {
                day_ = (day_ / 10) * 10 + d;
                if (day_ < 1) day_ = 1;
                if (day_ > 31) day_ = 31;
                nextField();  // Auto-advance
            }
            break;
        }
        case Field::MONTH: {
            if (digitPos_ == 0) {
                month_ = d * 10;
                digitPos_ = 1;
            } else {
                month_ = (month_ / 10) * 10 + d;
                if (month_ < 1) month_ = 1;
                if (month_ > 12) month_ = 12;
                nextField();  // Auto-advance
            }
            break;
        }
        case Field::YEAR: {
            if (digitPos_ == 0) {
                year_ = d * 1000;
                digitPos_ = 1;
            } else if (digitPos_ == 1) {
                year_ = (year_ / 1000) * 1000 + d * 100;
                digitPos_ = 2;
            } else if (digitPos_ == 2) {
                year_ = (year_ / 100) * 100 + d * 10;
                digitPos_ = 3;
            } else {
                year_ = (year_ / 10) * 10 + d;
                digitPos_ = 0;  // Wrap around, stay in year
            }
            break;
        }
    }

    dirty_ = true;
}

/**
 * \brief Validates and clamps date values to supported bounds.
 * \return `true` after values were normalized.
 */
bool DateInputView::validateAndClamp() {
    if (day_ < 1) day_ = 1;
    if (day_ > 31) day_ = 31;
    if (month_ < 1) month_ = 1;
    if (month_ > 12) month_ = 12;
    if (year_ < 2000) year_ = 2000;
    if (year_ > 2099) year_ = 2099;
    return true;
}

/**
 * \brief Handles key input for the date editor.
 * \param key Pressed key code.
 * \return Input handling result for the view stack.
 */
InputResult DateInputView::onKey(char key) {
    // Digit input (all 0-9 keys are digits, auto-advances between fields)
    if (key >= '0' && key <= '9') {
        enterDigit(key);
        return InputResult::CONSUMED;
    }

    switch (key) {
        case KEY_NO:  // Clear or cancel
            if (digitPos_ > 0 ||
                (currentField_ == Field::DAY && day_ > 0) ||
                (currentField_ == Field::MONTH && month_ > 0) ||
                (currentField_ == Field::YEAR && year_ > 0)) {
                clearField();
                return InputResult::CONSUMED;
            }
            cdc::ui::ViewStack::instance().pop();
            if (onCancel_) {
                onCancel_();
            }
            return InputResult::CONSUMED;

        case KEY_YES:  // Confirm
            validateAndClamp();
            LOG_I(TAG, "Date confirmed: %02d.%02d.%04d", day_, month_, year_);
            cdc::ui::ViewStack::instance().pop();
            if (onConfirm_) {
                onConfirm_(day_, month_, year_);
            }
            return InputResult::CONSUMED;

        default:
            return InputResult::IGNORED;
    }
}

/**
 * \brief Returns localized footer hint text.
 * \return Footer hint string.
 */
const char* DateInputView::getFooterHint() const {
    return ui::tr("core.hint_date_input");
}

/**
 * \brief Renders the date input view.
 * \param partial Indicates partial/full redraw mode.
 * \return void
 */
void DateInputView::render(bool partial) {
    hal::IDisplay* display = hal::getDisplayInstance();
    if (!display) return;

    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;

    const uint16_t width = display->getWidth();
    const uint16_t height = display->getHeight();

    if (!partial) {
        gfx->fillScreen(EPD_WHITE);
    }

    gfx->setTextColor(EPD_BLACK);

    if (title_) {
        gfx->setTextSize(1);
        render::drawHeaderCentered(gfx, title_, TITLE_Y, width);
    }

    char dateStr[20];
    snprintf(dateStr, sizeof(dateStr), "%02d / %02d / %04d", day_, month_, year_);

    gfx->setTextSize(2);
    int16_t x1, y1;
    uint16_t w, h;
    gfx->getTextBounds(dateStr, 0, 0, &x1, &y1, &w, &h);
    int startX = (width - w) / 2;
    gfx->setCursor(startX, DATE_Y);
    gfx->print(dateStr);

    gfx->fillRect(0, UNDERLINE_Y, width, 4, EPD_WHITE);

    int charWidth = 12;
    int underlineX = startX;
    int underlineW = 0;

    switch (currentField_) {
        case Field::DAY:
            underlineX = startX;
            underlineW = 2 * charWidth;
            break;
        case Field::MONTH:
            underlineX = startX + 5 * charWidth;
            underlineW = 2 * charWidth;
            break;
        case Field::YEAR:
            underlineX = startX + 10 * charWidth;
            underlineW = 4 * charWidth;
            break;
    }

    gfx->fillRect(underlineX, UNDERLINE_Y, underlineW, 3, EPD_BLACK);

    const char* hint = getFooterHint();
    render::drawFooterBar(gfx, width, height, nullptr, hint, false);

    dirty_ = false;
}

} // namespace cdc::ui
