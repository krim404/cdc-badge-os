/**
 * MessageBox Implementation
 *
 * System feedback overlay with optional icon and auto-dismiss.
 * Rendered as modal over the current view.
 */

#include "cdc_views/MessageBox.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"
#include <goodisplay/gdey029T94.h>
#include <cstring>

static const char* TAG = "MessageBox";

/**
 * \brief Display layout constants.
 */
static constexpr int BOX_PADDING = 12;
static constexpr int ICON_SIZE = 16;
static constexpr int ICON_MARGIN = 8;
static constexpr int MIN_BOX_WIDTH = 120;
static constexpr int MAX_BOX_WIDTH = 260;

namespace cdc::ui {

/**
 * \brief Initializes message box state.
 * \param message Message text to display.
 * \param icon Icon type for the message.
 * \param timeoutMs Auto-close timeout in milliseconds.
 * \return void
 */
void MessageBox::init(const char* message, MessageIcon icon, uint32_t timeoutMs) {
    message_ = message;
    icon_ = icon;
    timeoutMs_ = timeoutMs;
    startTimeMs_ = 0;  // Will be set on first tick
    onClose_ = nullptr;
    dirty_ = true;

    LOG_D(TAG, "init: msg='%s', icon=%d, timeout=%lu",
             message ? message : "(null)", static_cast<int>(icon), timeoutMs);
}

/**
 * \brief Updates timeout-based auto-dismiss behavior.
 * \param nowMs Current monotonic time in milliseconds.
 * \return void
 */
void MessageBox::onTick(uint32_t nowMs) {
    // Initialize start time on first tick
    if (startTimeMs_ == 0) {
        startTimeMs_ = nowMs;
    }

    // Check timeout
    if (timeoutMs_ > 0 && (nowMs - startTimeMs_) >= timeoutMs_) {
        LOG_D(TAG, "Timeout reached, hiding");
        if (onClose_) {
            onClose_();
        }
        hideMessage();
    }
}

/**
 * \brief Handles key input for manual dismissal.
 * \param key Pressed key code.
 * \return Input handling result for the view stack.
 */
InputResult MessageBox::onKey(char key) {
    // Any key dismisses (Y or N)
    if (key == KEY_YES || key == KEY_NO) {
        LOG_D(TAG, "Key '%c' pressed, hiding", key);
        if (onClose_) {
            onClose_();
        }
        hideMessage();
        return InputResult::CONSUMED;
    }
    return InputResult::IGNORED;
}

/**
 * \brief Renders the modal message box.
 * \param partial Indicates partial/full redraw mode.
 * \return void
 */
void MessageBox::render(bool partial) {
    hal::IDisplay* display = hal::getDisplayInstance();
    if (!display) return;

    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;

    const uint16_t screenWidth = display->getWidth();
    const uint16_t screenHeight = display->getHeight();

    // Calculate text dimensions
    int16_t x1, y1;
    uint16_t textWidth, textHeight;
    gfx->setTextSize(1);

    if (message_) {
        gfx->getTextBounds(message_, 0, 0, &x1, &y1, &textWidth, &textHeight);
    } else {
        textWidth = 0;
        textHeight = 12;
    }

    // Calculate box dimensions
    uint16_t contentWidth = textWidth;
    if (icon_ != MessageIcon::NONE) {
        contentWidth += ICON_SIZE + ICON_MARGIN;
    }

    uint16_t boxWidth = contentWidth + BOX_PADDING * 2;
    if (boxWidth < MIN_BOX_WIDTH) boxWidth = MIN_BOX_WIDTH;
    if (boxWidth > MAX_BOX_WIDTH) boxWidth = MAX_BOX_WIDTH;

    uint16_t boxHeight = textHeight + BOX_PADDING * 2;
    if (boxHeight < 40) boxHeight = 40;

    // Center the box
    int boxX = (screenWidth - boxWidth) / 2;
    int boxY = (screenHeight - boxHeight) / 2;

    // Draw box background (white with black border)
    render::drawDialogFrame(gfx, boxX, boxY, boxWidth, boxHeight);

    // Calculate content position
    int contentX = boxX + BOX_PADDING;
    int contentY = boxY + (boxHeight - textHeight) / 2;

    // Draw icon if present
    if (icon_ != MessageIcon::NONE) {
        int iconX = contentX;
        int iconY = boxY + (boxHeight - ICON_SIZE) / 2;

        switch (icon_) {
            case MessageIcon::SUCCESS:
                // Checkmark
                gfx->drawLine(iconX + 2, iconY + 8, iconX + 6, iconY + 12, EPD_BLACK);
                gfx->drawLine(iconX + 6, iconY + 12, iconX + 14, iconY + 4, EPD_BLACK);
                gfx->drawLine(iconX + 2, iconY + 9, iconX + 6, iconY + 13, EPD_BLACK);
                gfx->drawLine(iconX + 6, iconY + 13, iconX + 14, iconY + 5, EPD_BLACK);
                break;

            case MessageIcon::ERROR:
                // X mark
                gfx->drawLine(iconX + 2, iconY + 2, iconX + 14, iconY + 14, EPD_BLACK);
                gfx->drawLine(iconX + 14, iconY + 2, iconX + 2, iconY + 14, EPD_BLACK);
                gfx->drawLine(iconX + 3, iconY + 2, iconX + 14, iconY + 13, EPD_BLACK);
                gfx->drawLine(iconX + 13, iconY + 2, iconX + 2, iconY + 13, EPD_BLACK);
                break;

            case MessageIcon::INFO:
                // Circle with "i"
                gfx->drawCircle(iconX + 8, iconY + 8, 7, EPD_BLACK);
                gfx->fillRect(iconX + 7, iconY + 4, 2, 2, EPD_BLACK);  // Dot
                gfx->fillRect(iconX + 7, iconY + 7, 2, 5, EPD_BLACK);  // Line
                break;

            case MessageIcon::WARNING:
                // Triangle with "!"
                gfx->drawTriangle(
                    iconX + 8, iconY + 1,
                    iconX + 1, iconY + 14,
                    iconX + 15, iconY + 14,
                    EPD_BLACK
                );
                gfx->fillRect(iconX + 7, iconY + 5, 2, 5, EPD_BLACK);  // Line
                gfx->fillRect(iconX + 7, iconY + 11, 2, 2, EPD_BLACK); // Dot
                break;

            default:
                break;
        }

        contentX += ICON_SIZE + ICON_MARGIN;
    }

    // Draw message text
    gfx->setTextColor(EPD_BLACK);
    gfx->setCursor(contentX, contentY);
    if (message_) {
        render::printText(gfx, message_);
    }

    dirty_ = false;
}

/**
 * \brief Convenience helper functions.
 */

static MessageBox s_sharedMessageBox;

/**
 * \brief Shows the shared modal message box.
 * \param message Message text.
 * \param icon Icon type.
 * \param timeoutMs Auto-close timeout in milliseconds.
 * \param onClose Optional close callback.
 * \return void
 */
void showMessage(const char* message, MessageIcon icon,
                 uint32_t timeoutMs, MessageBox::CloseCallback onClose) {
    s_sharedMessageBox.init(message, icon, timeoutMs);
    s_sharedMessageBox.setOnClose(onClose);
    ViewStack::instance().showModal(&s_sharedMessageBox);
}

/**
 * \brief Hides the currently shown modal message box.
 * \return void
 */
void hideMessage() {
    ViewStack::instance().hideModal();
}

} // namespace cdc::ui
