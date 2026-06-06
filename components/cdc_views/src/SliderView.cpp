/**
 * SliderView Implementation
 *
 * Value adjustment with visual progress bar.
 */

#include "cdc_views/SliderView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_ui/I18n.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_hal/IKeypad.h"
#include "cdc_log.h"
#include <goodisplay/gdey029T94.h>
#include <cstdio>
#include <algorithm>

static constexpr uint32_t REPEAT_INITIAL_MS = 350;
static constexpr uint32_t REPEAT_PERIOD_MS = 80;

static const char* TAG = "SliderView";

/**
 * \brief Display layout constants.
 */
static constexpr int TITLE_Y = 20;
static constexpr int VALUE_Y = 55;
static constexpr int BAR_Y = 85;
static constexpr int BAR_HEIGHT = 20;
static constexpr int BAR_MARGIN = 20;

namespace cdc::ui {

/**
 * \brief Initializes slider bounds, value, and display options.
 * \param title Title text shown in the view.
 * \param minVal Minimum slider value.
 * \param maxVal Maximum slider value.
 * \param initial Initial slider value.
 * \param step Default step size.
 * \param unit Optional value unit suffix.
 * \return void
 */
void SliderView::init(const char* title, uint16_t minVal, uint16_t maxVal,
                      uint16_t initial, uint16_t step, const char* unit) {
    title_ = title;
    minValue_ = minVal;
    maxValue_ = maxVal;
    value_ = std::clamp(initial, minVal, maxVal);
    step_ = step > 0 ? step : 1;
    unit_ = unit;
    displayOffset_ = 0;
    zeroLabel_ = nullptr;
    onCancel_ = nullptr;
    dirty_ = true;
}

/**
 * \brief Sets slider value with range clamping.
 * \param value Target slider value.
 * \return void
 */
void SliderView::setValue(uint16_t value) {
    value = std::clamp(value, minValue_, maxValue_);
    if (value_ != value) {
        value_ = value;
        dirty_ = true;
    }
}

/**
 * \brief Adjusts slider value up or down.
 * \param increase `true` to increase, `false` to decrease.
 * \return void
 */
void SliderView::adjust(bool increase) {
    uint16_t newValue = value_;

    // Get step size (dynamic or fixed)
    uint16_t currentStep = stepCallback_ ? stepCallback_(value_, increase) : step_;

    if (increase) {
        uint16_t next = value_ + currentStep;
        newValue = (next > maxValue_) ? maxValue_ : next;
    } else {
        newValue = (value_ > currentStep) ? value_ - currentStep : minValue_;
    }

    if (newValue != value_) {
        value_ = newValue;
        dirty_ = true;
        LOG_D(TAG, "Slider adjusted to %d (step=%d)", value_, currentStep);

        // Call change callback for real-time updates (e.g., brightness preview)
        if (onChange_) {
            onChange_(value_);
        }
    }
}

/**
 * \brief Handles key input for slider adjustment and confirmation.
 * \param key Pressed key code.
 * \return Input handling result for the view stack.
 */
InputResult SliderView::onKey(char key) {
    switch (key) {
        case '6': // Right = Increase
            adjust(true);
            repeatStartMs_ = 0;
            return InputResult::CONSUMED;

        case '4': // Left = Decrease
            adjust(false);
            repeatStartMs_ = 0;
            return InputResult::CONSUMED;

        case KEY_YES: // Save
            ViewStack::instance().pop();
            if (onSave_) {
                onSave_(value_);
            }
            return InputResult::CONSUMED;

        case KEY_NO: // Cancel
            ViewStack::instance().pop();
            if (onCancel_) {
                onCancel_();
            }
            return InputResult::CONSUMED;

        default:
            return InputResult::IGNORED;
    }
}

void SliderView::onTick(uint32_t nowMs) {
    auto* kp = cdc::hal::getKeypadInstance();
    if (!kp) return;

    bool keyLeft  = kp->isKeyPressed(cdc::hal::Key::KEY_4);
    bool keyRight = kp->isKeyPressed(cdc::hal::Key::KEY_6);

    if (!keyLeft && !keyRight) {
        repeatStartMs_ = 0;
        return;
    }

    if (repeatStartMs_ == 0) {
        repeatStartMs_ = nowMs;
        lastRepeatMs_ = nowMs;
        return;
    }

    if (nowMs - repeatStartMs_ < REPEAT_INITIAL_MS) return;
    if (nowMs - lastRepeatMs_ < REPEAT_PERIOD_MS) return;

    lastRepeatMs_ = nowMs;
    adjust(keyRight);
}

/**
 * \brief Returns localized footer hint text.
 * \return Footer hint string.
 */
const char* SliderView::getFooterHint() const {
    return ui::tr("core.hint_brightness");
}

/**
 * \brief Renders slider title, value text, progress bar, and footer.
 * \param partial Indicates partial/full redraw mode.
 * \return void
 */
void SliderView::render(bool partial) {
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

    // Title (centered)
    if (title_) {
        gfx->setTextSize(1);
        render::drawHeaderCentered(gfx, title_, TITLE_Y, width);
    }

    // Value display (centered, larger)
    char valueStr[32];
    int16_t displayValue = static_cast<int16_t>(value_) + displayOffset_;

    if (value_ == 0 && zeroLabel_) {
        // Special label for zero
        snprintf(valueStr, sizeof(valueStr), "%s", zeroLabel_);
    } else if (unit_) {
        snprintf(valueStr, sizeof(valueStr), "%d %s", displayValue, unit_);
    } else {
        snprintf(valueStr, sizeof(valueStr), "%d", displayValue);
    }

    gfx->setTextSize(2);
    int16_t x1, y1;
    uint16_t w, h;
    gfx->getTextBounds(valueStr, 0, 0, &x1, &y1, &w, &h);
    gfx->fillRect(0, VALUE_Y - 5, width, h + 10, EPD_WHITE);
    gfx->setCursor((width - w) / 2, VALUE_Y);
    gfx->print(valueStr);

    int barWidth = width - 2 * BAR_MARGIN;
    gfx->drawRect(BAR_MARGIN, BAR_Y, barWidth, BAR_HEIGHT, EPD_BLACK);

    int fillWidth = 0;
    if (maxValue_ > minValue_) {
        fillWidth = (value_ - minValue_) * (barWidth - 4) / (maxValue_ - minValue_);
    }
    gfx->fillRect(BAR_MARGIN + 2, BAR_Y + 2, fillWidth, BAR_HEIGHT - 4, EPD_BLACK);

    // Draw horizontal arrow indicators [4] < ... > [6]
    gfx->setTextSize(1);

    // Left arrow and [4] label
    int arrowY = BAR_Y + BAR_HEIGHT / 2;
    gfx->fillTriangle(
        BAR_MARGIN - 15, arrowY,
        BAR_MARGIN - 5, arrowY - 5,
        BAR_MARGIN - 5, arrowY + 5,
        EPD_BLACK
    );
    gfx->setCursor(BAR_MARGIN - 17, BAR_Y + BAR_HEIGHT + 8);
    gfx->print("[4]");

    // Right arrow and [6] label
    int barRight = BAR_MARGIN + barWidth;
    gfx->fillTriangle(
        barRight + 15, arrowY,
        barRight + 5, arrowY - 5,
        barRight + 5, arrowY + 5,
        EPD_BLACK
    );
    gfx->setCursor(barRight + 5, BAR_Y + BAR_HEIGHT + 8);
    gfx->print("[6]");

    const char* hint = getFooterHint();
    render::drawFooterBar(gfx, width, height, nullptr, hint, false);

    dirty_ = false;
}

/**
 * \brief Convenience factory/helper function.
 */

static SliderView s_sharedSlider;

/**
 * \brief Shows a shared slider view instance.
 * \param title View title text.
 * \param minVal Minimum slider value.
 * \param maxVal Maximum slider value.
 * \param initial Initial slider value.
 * \param step Default step size.
 * \param unit Optional value unit suffix.
 * \param onSave Save callback.
 * \param onChange Optional live-change callback.
 * \return Pointer to the shared `SliderView` instance.
 */
SliderView* showSlider(const char* title, uint16_t minVal, uint16_t maxVal,
                       uint16_t initial, uint16_t step, const char* unit,
                       SliderView::SaveCallback onSave,
                       SliderView::ChangeCallback onChange) {
    s_sharedSlider.init(title, minVal, maxVal, initial, step, unit);
    s_sharedSlider.setOnSave(onSave);
    if (onChange) {
        s_sharedSlider.setOnChange(onChange);
    }
    ViewStack::instance().push(&s_sharedSlider);
    return &s_sharedSlider;
}

} // namespace cdc::ui
