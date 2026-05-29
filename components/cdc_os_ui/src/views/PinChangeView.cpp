/**
 * PinChangeView Implementation
 *
 * PIN change wizard with three steps.
 */

#include "cdc_os_ui/views/PinChangeView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_core/PinManager.h"
#include "cdc_ui/I18n.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"
#include "esp_timer.h"
#include <goodisplay/gdey029T94.h>
#include <cstring>

static const char* TAG = "PinChangeView";

/**
 * \brief Display layout constants.
 */
static constexpr int TITLE_Y = 15;
static constexpr int STEP_Y = 30;
static constexpr int PIN_Y = 55;
static constexpr int PIN_DOT_SIZE = 12;
static constexpr int PIN_DOT_SPACING = 16;
static constexpr int MESSAGE_Y = 90;

namespace cdc::ui {

/**
 * \brief Initializes PIN-change wizard state.
 * \param minLength Minimum required PIN length.
 * \param maxLength Maximum allowed PIN length.
 * \return void
 */
void PinChangeView::init(uint8_t minLength, uint8_t maxLength) {
    minLength_ = minLength < 4 ? 4 : minLength;
    maxLength_ = maxLength > MAX_PIN_LENGTH ? MAX_PIN_LENGTH : maxLength;
    step_ = Step::CURRENT_PIN;
    clearBuffer();
    memset(currentPin_, 0, sizeof(currentPin_));
    memset(newPin_, 0, sizeof(newPin_));
    memset(confirmPin_, 0, sizeof(confirmPin_));
    message_ = nullptr;
    messageShownMs_ = 0;
    pinChanged_ = false;
    dirty_ = true;
}

/**
 * \brief Resets wizard state when entering the view.
 * \param context Optional view context (unused).
 * \return void
 */
void PinChangeView::onEnter(void* context) {
    (void)context;
    init(minLength_, maxLength_);
}

/**
 * \brief Clears the input buffer for the current wizard step.
 * \return void
 */
void PinChangeView::clearBuffer() {
    char* buf = getCurrentBuffer();
    if (buf) {
        memset(buf, 0, MAX_PIN_LENGTH + 1);
    }
    length_ = 0;
}

/**
 * \brief Returns mutable buffer for current wizard step.
 * \return Pointer to active step buffer.
 */
char* PinChangeView::getCurrentBuffer() {
    switch (step_) {
        case Step::CURRENT_PIN: return currentPin_;
        case Step::NEW_PIN: return newPin_;
        case Step::CONFIRM_PIN: return confirmPin_;
    }
    return currentPin_;
}

/**
 * \brief Returns localized title for current wizard step.
 * \return Step title string.
 */
const char* PinChangeView::getStepTitle() const {
    switch (step_) {
        case Step::CURRENT_PIN: return ui::tr("core.current_pin");
        case Step::NEW_PIN: return ui::tr("core.new_pin");
        case Step::CONFIRM_PIN: return ui::tr("core.confirm_pin");
    }
    return "";
}

/**
 * \brief Returns remaining retry count for current PIN verification.
 * \return Remaining retries.
 */
uint8_t PinChangeView::getRetriesRemaining() const {
    if (onRetries_) return onRetries_();
    return core::PinManager::instance().getBadgeRetries();
}

/**
 * \brief Appends one digit to current step buffer.
 * \param digit Numeric digit character.
 * \return void
 */
void PinChangeView::addDigit(char digit) {
    if (length_ >= maxLength_) return;

    char* buf = getCurrentBuffer();
    buf[length_++] = digit;
    buf[length_] = '\0';
    dirty_ = true;
}

/**
 * \brief Removes last digit from current step buffer.
 * \return void
 */
void PinChangeView::backspace() {
    if (length_ > 0) {
        char* buf = getCurrentBuffer();
        buf[--length_] = '\0';
        dirty_ = true;
    }
}

/**
 * \brief Shows transient status/error message.
 * \param msg Message text.
 * \return void
 */
void PinChangeView::showMessage(const char* msg) {
    message_ = msg;
    messageShownMs_ = esp_timer_get_time() / 1000;
    dirty_ = true;
}

/**
 * \brief Validates and processes current wizard step confirmation.
 * \return void
 */
void PinChangeView::confirmStep() {
    switch (step_) {
        case Step::CURRENT_PIN: {
            // Verify current PIN
            if (length_ < minLength_) {
                showMessage(ui::tr("core.pin_too_short"));
                clearBuffer();
                return;
            }

            bool ok = onVerify_ ? onVerify_(currentPin_) : core::PinManager::instance().verifyBadgePin(currentPin_);
            bool blocked = onBlocked_ ? onBlocked_() : core::PinManager::instance().isBadgeBlocked();
            if (!ok) {
                if (blocked) {
                    showMessage(ui::tr("core.locked_out"));
                } else {
                    showMessage(ui::tr("core.wrong_pin"));
                }
                clearBuffer();
                return;
            }

            // Move to new PIN step
            step_ = Step::NEW_PIN;
            clearBuffer();
            message_ = nullptr;
            LOG_I(TAG, "Current PIN verified, entering new PIN");
            dirty_ = true;
            break;
        }

        case Step::NEW_PIN: {
            if (length_ < minLength_) {
                showMessage(ui::tr("core.pin_too_short"));
                clearBuffer();
                return;
            }

            // Move to confirm step
            step_ = Step::CONFIRM_PIN;
            clearBuffer();
            message_ = nullptr;
            LOG_I(TAG, "New PIN entered, confirming");
            dirty_ = true;
            break;
        }

        case Step::CONFIRM_PIN: {
            if (length_ < minLength_) {
                showMessage(ui::tr("core.pin_too_short"));
                clearBuffer();
                return;
            }

            // Check if PINs match
            if (strcmp(newPin_, confirmPin_) != 0) {
                showMessage(ui::tr("core.pin_mismatch"));
                // Go back to new PIN step
                step_ = Step::NEW_PIN;
                memset(newPin_, 0, sizeof(newPin_));
                clearBuffer();
                LOG_W(TAG, "PIN mismatch, re-enter new PIN");
                return;
            }

            // Change the PIN
            bool changed = onChange_
                ? onChange_(currentPin_, newPin_)
                : core::PinManager::instance().setBadgePin(newPin_);
            if (changed) {
                pinChanged_ = true;
                showMessage(ui::tr("core.pin_changed"));
                LOG_I(TAG, "PIN changed successfully");
            } else {
                showMessage(ui::tr("core.error_generic"));
                LOG_E(TAG, "Failed to set new PIN");
            }
            break;
        }
    }
}

/**
 * \brief Handles message timeout and completion callbacks.
 * \param nowMs Current monotonic time in milliseconds.
 * \return void
 */
void PinChangeView::onTick(uint32_t nowMs) {
    if (messageShownMs_ > 0 && message_ != nullptr) {
        if (nowMs - messageShownMs_ >= MESSAGE_DISPLAY_MS) {
            message_ = nullptr;
            messageShownMs_ = 0;
            dirty_ = true;

            // If PIN was changed, pop the view
            if (pinChanged_ && onComplete_) {
                onComplete_(true);
            }
        }
    }
}

/**
 * \brief Handles key input for PIN-change flow.
 * \param key Pressed key code.
 * \return Input handling result for the view stack.
 */
InputResult PinChangeView::onKey(char key) {
    // Don't accept input if locked out
    bool blocked = onBlocked_ ? onBlocked_() : core::PinManager::instance().isBadgeBlocked();
    if (blocked) {
        return InputResult::REQUEST_POP;
    }

    // Handle digits
    if (key >= '0' && key <= '9') {
        addDigit(key);
        return InputResult::CONSUMED;
    }

    switch (key) {
        case KEY_NO: // Backspace or cancel
            if (length_ > 0) {
                backspace();
            } else if (step_ == Step::CURRENT_PIN) {
                // Cancel from first step
                if (onComplete_) {
                    onComplete_(false);
                }
                return InputResult::REQUEST_POP;
            } else {
                // Go back to previous step
                if (step_ == Step::CONFIRM_PIN) {
                    step_ = Step::NEW_PIN;
                    length_ = strlen(newPin_);
                } else if (step_ == Step::NEW_PIN) {
                    step_ = Step::CURRENT_PIN;
                    length_ = strlen(currentPin_);
                }
                dirty_ = true;
            }
            return InputResult::CONSUMED;

        case KEY_YES: // Confirm
            if (length_ >= minLength_) {
                confirmStep();
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
const char* PinChangeView::getFooterHint() const {
    return ui::tr("core.hint_pin_input");
}

/**
 * \brief Renders PIN-change wizard UI.
 * \param partial Indicates partial/full redraw mode.
 * \return void
 */
void PinChangeView::render(bool partial) {
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
    int16_t x1, y1;
    uint16_t w, h;

    gfx->setTextSize(1);
    const char* title = title_ ? title_ : ui::tr("core.change_pin");
    render::drawHeaderCentered(gfx, title, TITLE_Y, width);

    const char* stepTitle = getStepTitle();
    char stepStr[48];
    snprintf(stepStr, sizeof(stepStr), "%d/3: %s", static_cast<int>(step_) + 1, stepTitle);
    gfx->getTextBounds(stepStr, 0, 0, &x1, &y1, &w, &h);
    gfx->setCursor((width - w) / 2, STEP_Y);
    render::printText(gfx, stepStr);

    int dotsToShow = length_ > 8 ? length_ : 8;
    int totalWidth = dotsToShow * PIN_DOT_SIZE + (dotsToShow - 1) * (PIN_DOT_SPACING - PIN_DOT_SIZE);
    int startX = (width - totalWidth) / 2;

    for (int i = 0; i < dotsToShow; i++) {
        int x = startX + i * PIN_DOT_SPACING;
        int y = PIN_Y;

        if (i < length_) {
            gfx->fillCircle(x + PIN_DOT_SIZE / 2, y + PIN_DOT_SIZE / 2, PIN_DOT_SIZE / 2 - 1, EPD_BLACK);
        } else {
            gfx->drawCircle(x + PIN_DOT_SIZE / 2, y + PIN_DOT_SIZE / 2, PIN_DOT_SIZE / 2 - 1, EPD_BLACK);
        }
    }

    if (step_ == Step::CURRENT_PIN) {
        uint8_t retries = getRetriesRemaining();
        char retriesStr[24];
        snprintf(retriesStr, sizeof(retriesStr), "%s: %d", ui::tr("core.retries"), retries);
        gfx->setTextSize(1);
        gfx->getTextBounds(retriesStr, 0, 0, &x1, &y1, &w, &h);
        gfx->setCursor((width - w) / 2, PIN_Y + 25);
        gfx->print(retriesStr);
    }

    gfx->fillRect(0, MESSAGE_Y - 2, width, 20, EPD_WHITE);
    if (message_) {
        gfx->setTextSize(1);
        gfx->getTextBounds(message_, 0, 0, &x1, &y1, &w, &h);
        gfx->setCursor((width - w) / 2, MESSAGE_Y);
        render::printText(gfx, message_);
    }

    const char* hint = getFooterHint();
    render::drawFooterBar(gfx, width, height, nullptr, hint, false);

    dirty_ = false;
}

} // namespace cdc::ui
