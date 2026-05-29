/**
 * PinEntryView Implementation
 *
 * Secure PIN code input with masked display.
 * Errors are shown via MessageBox overlay.
 */

#include "cdc_views/PinEntryView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/MessageBox.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_core/PinManager.h"
#include "cdc_ui/I18n.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"
#include <goodisplay/gdey029T94.h>
#include <cstring>

static const char* TAG = "PinEntryView";

/**
 * \brief Display layout constants.
 */
static constexpr int TITLE_Y = 15;
static constexpr int PIN_Y = 50;
static constexpr int PIN_DOT_SIZE = 16;
static constexpr int PIN_DOT_SPACING = 24;
static constexpr int RETRIES_Y = 80;

/** \brief Toast message display durations in milliseconds. */
static constexpr uint32_t TOAST_DURATION_SHORT_MS = 1500;
static constexpr uint32_t TOAST_DURATION_LONG_MS = 3000;

/** \brief Refresh interval for the lockout countdown display. */
static constexpr uint32_t LOCKOUT_REFRESH_MS = 1000;

namespace cdc::ui {

/**
 * \brief Initializes PIN entry configuration and clears current input.
 * \param title Title text shown in the view.
 * \param maxPinLength Maximum accepted PIN length.
 * \param maxAttempts Maximum allowed attempts before lockout handling.
 * \return void
 */
void PinEntryView::init(const char* title, uint8_t maxPinLength, uint8_t maxAttempts) {
    title_ = title;
    maxLength_ = maxPinLength > MAX_PIN_LENGTH ? MAX_PIN_LENGTH : maxPinLength;
    maxAttempts_ = maxAttempts;
    minLength_ = core::PinManager::BADGE_PIN_MIN;  // Default minimum from PinManager
    clear();
    dirty_ = true;
}

/**
 * \brief Resets entry state when the view is entered.
 * \param context Optional view context (unused).
 * \return void
 */
void PinEntryView::onEnter(void* context) {
    (void)context;
    clear();
    dirty_ = true;
}

/**
 * \brief Clears the internal PIN buffer.
 * \return void
 */
void PinEntryView::clear() {
    memset(buffer_, 0, sizeof(buffer_));
    length_ = 0;
}

/**
 * \brief Appends a digit to the PIN buffer.
 * \param digit Numeric digit character.
 * \return void
 */
void PinEntryView::addDigit(char digit) {
    if (length_ >= maxLength_ || lockedOut_) return;

    buffer_[length_++] = digit;
    buffer_[length_] = '\0';
    dirty_ = true;
}

/**
 * \brief Removes the last digit from the PIN buffer.
 * \return void
 */
void PinEntryView::backspace() {
    if (length_ > 0 && !lockedOut_) {
        buffer_[--length_] = '\0';
        dirty_ = true;
    }
}

/**
 * \brief Updates lockout state and periodic countdown refresh.
 * \param nowMs Current monotonic time in milliseconds.
 * \return void
 */
void PinEntryView::onTick(uint32_t nowMs) {
    (void)nowMs;
    core::PinManager& pm = core::PinManager::instance();

    // Check if blocked status changed
    bool blocked = pm.isBadgeBlocked();
    if (blocked != lockedOut_) {
        lockedOut_ = blocked;
        dirty_ = true;
    }

    // Update display every second during lockout to show countdown
    if (lockedOut_) {
        static uint32_t lastUpdate = 0;
        if (nowMs - lastUpdate >= LOCKOUT_REFRESH_MS) {
            lastUpdate = nowMs;
            dirty_ = true;
        }
    }
}

/**
 * \brief Returns remaining lockout time.
 * \return Remaining lockout duration in milliseconds.
 */
uint32_t PinEntryView::getLockoutRemaining() const {
    return core::PinManager::instance().getLockoutRemainingMs();
}

/**
 * \brief Verifies the entered PIN via callback and updates UI state.
 * \return void
 */
void PinEntryView::verify() {
    if (length_ < minLength_) {
        if (showMessages_) {
            showMessage(ui::tr("core.pin_too_short"), MessageIcon::WARNING, TOAST_DURATION_SHORT_MS);
        }
        return;
    }

    if (!onVerify_) {
        // No verify callback, just accept
        if (onSuccess_) {
            onSuccess_();
        }
        return;
    }

    bool valid = onVerify_(buffer_);
    if (valid) {
        LOG_I(TAG, "PIN verified successfully");
        if (onSuccess_) {
            onSuccess_();
        }
    } else {
        attempts_++;
        LOG_W(TAG, "PIN verification failed, attempt %d", attempts_);

        // Check PinManager for block status (it manages retries persistently)
        core::PinManager& pm = core::PinManager::instance();
        if (pm.isBadgeBlocked()) {
            lockedOut_ = true;
            if (showMessages_) {
                showMessage(ui::tr("core.locked_out"), MessageIcon::ERROR, TOAST_DURATION_LONG_MS);
            }
        } else {
            if (showMessages_) {
                showMessage(ui::tr("core.wrong_pin"), MessageIcon::ERROR, TOAST_DURATION_SHORT_MS);
            }
        }
        clear();
        dirty_ = true;
        if (onFailure_) {
            onFailure_(lockedOut_);
        }
    }
}

/**
 * \brief Handles key input for PIN entry and actions.
 * \param key Pressed key code.
 * \return Input handling result for the view stack.
 */
InputResult PinEntryView::onKey(char key) {
    if (lockedOut_) {
        return InputResult::IGNORED;
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
            } else {
                if (onCancel_) {
                    onCancel_();
                    return InputResult::CONSUMED;
                }
                return InputResult::REQUEST_POP;
            }
            return InputResult::CONSUMED;

        case KEY_YES: // Confirm
            verify();
            return InputResult::CONSUMED;

        default:
            return InputResult::IGNORED;
    }
}

/**
 * \brief Returns localized footer hint text.
 * \return Footer hint string.
 */
const char* PinEntryView::getFooterHint() const {
    return ui::tr("core.hint_pin_input");
}

/**
 * \brief Renders PIN dots, status text, and footer hint.
 * \param partial Indicates partial/full redraw mode.
 * \return void
 */
void PinEntryView::render(bool partial) {
    hal::IDisplay* display = hal::getDisplayInstance();
    if (!display) {
        LOG_E(TAG, "display is null!");
        return;
    }

    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) {
        LOG_E(TAG, "gfx handle is null!");
        return;
    }

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

    // PIN dots (centered)
    int totalWidth = maxLength_ * PIN_DOT_SIZE + (maxLength_ - 1) * (PIN_DOT_SPACING - PIN_DOT_SIZE);
    int startX = (width - totalWidth) / 2;

    for (uint8_t i = 0; i < maxLength_; i++) {
        int x = startX + i * PIN_DOT_SPACING;
        int y = PIN_Y;

        if (i < length_) {
            // Filled dot for entered digits
            gfx->fillCircle(x + PIN_DOT_SIZE / 2, y + PIN_DOT_SIZE / 2, PIN_DOT_SIZE / 2 - 1, EPD_BLACK);
        } else {
            // Empty dot for remaining positions
            gfx->drawCircle(x + PIN_DOT_SIZE / 2, y + PIN_DOT_SIZE / 2, PIN_DOT_SIZE / 2 - 1, EPD_BLACK);
        }
    }

    // Show remaining retries or blocked status with countdown
    {
        core::PinManager& pm = core::PinManager::instance();
        gfx->setTextSize(1);
        int16_t x1, y1;
        uint16_t w, h;
        char statusStr[32];

        if (pm.isBadgeBlocked()) {
            uint32_t remainingMs = pm.getLockoutRemainingMs();
            uint32_t remainingSec = (remainingMs + 999) / 1000;  // Round up
            snprintf(statusStr, sizeof(statusStr), "%s: %lus", ui::tr("core.locked_out"), remainingSec);
        } else {
            uint8_t retries = pm.getBadgeRetries();
            snprintf(statusStr, sizeof(statusStr), "%s: %d", ui::tr("core.retries"), retries);
        }

        gfx->getTextBounds(statusStr, 0, 0, &x1, &y1, &w, &h);
        gfx->setCursor((width - w) / 2, RETRIES_Y);
        render::printText(gfx, statusStr);
    }

    // Footer hint
    const char* hint = getFooterHint();
    render::drawFooterBar(gfx, width, height, nullptr, hint, false);

    dirty_ = false;
}

/**
 * \brief Convenience factory/helper function.
 */

static PinEntryView s_sharedPinEntry;

/**
 * \brief Shows a shared PIN entry view instance.
 * \param title View title text.
 * \param onVerify Verification callback.
 * \param onSuccess Success callback.
 * \param maxLength Maximum PIN length.
 * \param minLength Minimum PIN length.
 * \param maxAttempts Maximum retry attempts.
 * \return Pointer to the shared `PinEntryView` instance.
 */
PinEntryView* showPinEntry(const char* title,
                           PinEntryView::VerifyCallback onVerify,
                           PinEntryView::SuccessCallback onSuccess,
                           uint8_t maxLength,
                           uint8_t minLength,
                           uint8_t maxAttempts) {
    s_sharedPinEntry.init(title, maxLength, maxAttempts);
    s_sharedPinEntry.setMinLength(minLength);
    s_sharedPinEntry.setOnVerify(onVerify);
    s_sharedPinEntry.setOnSuccess(onSuccess);
    ViewStack::instance().push(&s_sharedPinEntry);
    return &s_sharedPinEntry;
}

} // namespace cdc::ui
