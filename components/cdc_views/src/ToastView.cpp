/**
 * ToastView Implementation
 *
 * Temporary overlay message with auto-dismiss
 */

#include "cdc_views/ToastView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_hal/IDisplay.h"
#include <goodisplay/gdey029T94.h>
#include <cstring>
#include "esp_timer.h"

namespace cdc::ui {

/**
 * \brief Initializes toast message content and timing behavior.
 * \param message Toast message text.
 * \param icon Icon type to display.
 * \param durationMs Auto-dismiss duration in milliseconds.
 * \param dismissible Whether key presses can dismiss the toast.
 * \return void
 */
void ToastView::init(const char* message, Icon icon, uint16_t durationMs, bool dismissible) {
    if (message) {
        strncpy(message_, message, MAX_MSG_LEN - 1);
        message_[MAX_MSG_LEN - 1] = '\0';
    } else {
        message_[0] = '\0';
    }

    icon_ = icon;
    durationMs_ = durationMs;
    dismissible_ = dismissible;
    startMs_ = 0;
    started_ = false;
    expired_ = false;
    dirty_ = true;
}

/**
 * \brief Updates auto-dismiss timeout state.
 * \param nowMs Current monotonic time in milliseconds.
 * \return void
 */
void ToastView::onTick(uint32_t nowMs) {
    // started_ (not startMs_ == 0) marks "first render happened": a toast
    // rendered at uptime 0 would otherwise never arm its auto-dismiss.
    if (!started_ || expired_) return;
    if (durationMs_ == 0) return;
    if (nowMs < startMs_) return;
    static constexpr uint32_t MIN_DISPLAY_MS = 1000;
    uint32_t effective = durationMs_ < MIN_DISPLAY_MS ? MIN_DISPLAY_MS : durationMs_;
    if (nowMs - startMs_ >= effective) {
        expired_ = true;
        ViewStack::instance().hideModal();
    }
}

/**
 * \brief Handles key input for optional toast dismissal.
 * \param key Pressed key code.
 * \return Input handling result for the view stack.
 */
InputResult ToastView::onKey(char key) {
    // Any Y or N key dismisses the toast (if dismissible)
    if (dismissible_ && (key == KEY_YES || key == KEY_NO)) {
        expired_ = true;
        ViewStack::instance().hideModal();
        return InputResult::CONSUMED;
    }
    return InputResult::IGNORED;
}

/**
 * \brief Renders the toast overlay.
 * \param partial Indicates partial/full redraw mode.
 * \return void
 */
void ToastView::render(bool partial) {
    (void)partial;

    if (!started_) {
        started_ = true;
        startMs_ = esp_timer_get_time() / 1000;
    }

    hal::IDisplay* display = hal::getDisplayInstance();
    if (!display) return;

    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;

    const uint16_t width = display->getWidth();
    const uint16_t height = display->getHeight();

    // Calculate centered position
    int boxX = (width - BOX_WIDTH) / 2;
    int boxY = (height - BOX_HEIGHT) / 2;

    // Draw white box with double black border
    render::drawDialogFrame(gfx, boxX, boxY, BOX_WIDTH, BOX_HEIGHT);

    gfx->setTextColor(EPD_BLACK);
    gfx->setTextSize(1);

    // Text position (adjusted if icon present). y is the TOP of the glyph for
    // size-1 glcdfont (8px tall), so subtract half the height to vertically center.
    int textX = boxX + 15;
    int textY = boxY + (BOX_HEIGHT / 2) - 4;

    // Draw icon if present
    if (icon_ != Icon::NONE) {
        int iconX = boxX + 20;
        int iconY = boxY + (BOX_HEIGHT / 2);

        switch (icon_) {
            case Icon::SUCCESS:
                // Checkmark
                gfx->drawLine(iconX - 5, iconY, iconX - 2, iconY + 4, EPD_BLACK);
                gfx->drawLine(iconX - 2, iconY + 4, iconX + 6, iconY - 5, EPD_BLACK);
                // Thicker
                gfx->drawLine(iconX - 5, iconY + 1, iconX - 2, iconY + 5, EPD_BLACK);
                gfx->drawLine(iconX - 2, iconY + 5, iconX + 6, iconY - 4, EPD_BLACK);
                break;

            case Icon::ERROR:
                // X mark
                gfx->drawLine(iconX - 5, iconY - 5, iconX + 5, iconY + 5, EPD_BLACK);
                gfx->drawLine(iconX - 5, iconY + 5, iconX + 5, iconY - 5, EPD_BLACK);
                // Thicker
                gfx->drawLine(iconX - 4, iconY - 5, iconX + 6, iconY + 5, EPD_BLACK);
                gfx->drawLine(iconX - 4, iconY + 5, iconX + 6, iconY - 5, EPD_BLACK);
                break;

            case Icon::INFO:
                // Circle with i
                gfx->drawCircle(iconX, iconY, 6, EPD_BLACK);
                gfx->fillRect(iconX - 1, iconY - 3, 2, 2, EPD_BLACK);  // Dot
                gfx->fillRect(iconX - 1, iconY, 2, 5, EPD_BLACK);       // Stem
                break;
            case Icon::TASK:
                // Simple hourglass icon
                gfx->drawLine(iconX - 5, iconY - 6, iconX + 5, iconY - 6, EPD_BLACK);
                gfx->drawLine(iconX - 5, iconY + 6, iconX + 5, iconY + 6, EPD_BLACK);
                gfx->drawLine(iconX - 5, iconY - 6, iconX + 5, iconY + 6, EPD_BLACK);
                gfx->drawLine(iconX + 5, iconY - 6, iconX - 5, iconY + 6, EPD_BLACK);
                gfx->fillTriangle(iconX - 3, iconY - 4, iconX + 3, iconY - 4, iconX, iconY - 1, EPD_BLACK);
                gfx->fillTriangle(iconX - 3, iconY + 4, iconX + 3, iconY + 4, iconX, iconY + 1, EPD_BLACK);
                break;
            case Icon::ALERT:
                // Warning triangle with exclamation
                gfx->drawTriangle(iconX, iconY - 7, iconX - 6, iconY + 6, iconX + 6, iconY + 6, EPD_BLACK);
                gfx->fillRect(iconX - 1, iconY - 2, 2, 5, EPD_BLACK);
                gfx->fillRect(iconX - 1, iconY + 4, 2, 2, EPD_BLACK);
                break;

            default:
                break;
        }

        textX = boxX + 40;  // Shift text right when icon present
    }

    // Draw message text. Adafruit-GFX print() resets cursor_x to 0 on '\n',
    // which would shoot the second line to the left edge of the screen. Render
    // each line manually so it stays inside the modal frame, and shift the
    // first line up to keep the whole block vertically centred.
    constexpr int kLineHeight = 10;  // size-1 font (~8px) + 2px spacing

    size_t lineCount = 1;
    for (const char* p = message_; *p; ++p) {
        if (*p == '\n') ++lineCount;
    }

    int blockY = textY - static_cast<int>((lineCount - 1) * kLineHeight / 2);
    const char* lineStart = message_;
    int lineY = blockY;
    for (const char* p = message_;; ++p) {
        if (*p == '\n' || *p == '\0') {
            char lineBuf[96];
            size_t len = static_cast<size_t>(p - lineStart);
            if (len >= sizeof(lineBuf)) len = sizeof(lineBuf) - 1;
            memcpy(lineBuf, lineStart, len);
            lineBuf[len] = '\0';
            gfx->setCursor(textX, lineY);
            // Clip to the box interior: text drawn past the frame is never
            // cleared by drawDialogFrame, so on a partial refresh the overflow
            // of the previous message stays on screen (ghosting).
            render::printTruncated(gfx, lineBuf, (boxX + BOX_WIDTH) - textX - 8);
            if (*p == '\0') break;
            lineStart = p + 1;
            lineY += kLineHeight;
        }
    }

    dirty_ = false;
}

/**
 * \brief Convenience helper functions.
 */

static ToastView s_sharedToast;

/**
 * \brief Shows the shared toast instance with custom icon and behavior.
 * \param message Toast message text.
 * \param icon Icon type to display.
 * \param durationMs Auto-dismiss duration in milliseconds.
 * \param dismissible Whether key presses can dismiss the toast.
 * \return void
 */
static void showToastInternal(const char* message, ToastView::Icon icon, uint16_t durationMs,
                              bool dismissible = true) {
    s_sharedToast.init(message, icon, durationMs, dismissible);
    ViewStack::instance().showModal(&s_sharedToast);
    ViewStack::instance().render();  // Immediate render for toast
}

/**
 * \brief Shows a plain toast message.
 * \param message Toast message text.
 * \param durationMs Auto-dismiss duration in milliseconds.
 * \return void
 */
void showToast(const char* message, uint16_t durationMs) {
    showToastInternal(message, ToastView::Icon::NONE, durationMs);
}

/**
 * \brief Shows a success toast message.
 * \param message Toast message text.
 * \param durationMs Auto-dismiss duration in milliseconds.
 * \return void
 */
void showToastSuccess(const char* message, uint16_t durationMs) {
    showToastInternal(message, ToastView::Icon::SUCCESS, durationMs);
}

/**
 * \brief Shows an error toast message.
 * \param message Toast message text.
 * \param durationMs Auto-dismiss duration in milliseconds.
 * \return void
 */
void showToastError(const char* message, uint16_t durationMs) {
    showToastInternal(message, ToastView::Icon::ERROR, durationMs);
}

/**
 * \brief Shows an informational toast message.
 * \param message Toast message text.
 * \param durationMs Auto-dismiss duration in milliseconds.
 * \return void
 */
void showToastInfo(const char* message, uint16_t durationMs) {
    showToastInternal(message, ToastView::Icon::INFO, durationMs);
}

/**
 * \brief Shows a task/progress toast message.
 * \param message Toast message text.
 * \param durationMs Auto-dismiss duration in milliseconds.
 * \return void
 */
void showToastTask(const char* message, uint16_t durationMs) {
    showToastInternal(message, ToastView::Icon::TASK, durationMs);
}

/**
 * \brief Shows an alert toast message.
 * \param message Toast message text.
 * \param durationMs Auto-dismiss duration in milliseconds.
 * \return void
 */
void showToastAlert(const char* message, uint16_t durationMs) {
    showToastInternal(message, ToastView::Icon::ALERT, durationMs);
}

/**
 * \brief Shows a non-dismissible alert toast.
 * \param message Toast message text.
 * \return void
 */
void showToastAlertSticky(const char* message) {
    showToastInternal(message, ToastView::Icon::ALERT, 0, false);
}

} // namespace cdc::ui
