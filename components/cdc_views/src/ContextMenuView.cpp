/**
 * ContextMenuView Implementation
 *
 * Quick popup menu displayed as modal overlay.
 */

#include "cdc_views/ContextMenuView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"
#include <goodisplay/gdey029T94.h>
#include <cstring>
#include <algorithm>

static const char* TAG = "ContextMenuView";

/**
 * \brief Layout constants.
 */
static constexpr int BOX_PADDING = 8;
static constexpr int TITLE_HEIGHT = 18;
static constexpr int ITEM_HEIGHT = 16;
static constexpr int MIN_BOX_WIDTH = 120;
static constexpr int MAX_BOX_WIDTH = 200;

/** \brief Auto-dismiss timeout after the last interaction (ms). */
static constexpr uint32_t kMenuTimeoutMs = 60000;

namespace cdc::ui {

/**
 * \brief Initializes context menu content and selection state.
 * \param title Menu title text.
 * \param items Menu item array.
 * \param count Number of menu items.
 * \return void
 */
void ContextMenuView::init(const char* title, const ContextMenuItem* items, uint8_t count) {
    title_ = title;
    itemCount_ = count > MAX_ITEMS ? MAX_ITEMS : count;
    for (uint8_t i = 0; i < itemCount_; i++) {
        items_[i] = items[i];
    }
    selection_ = 0;
    scrollPos_ = 0;
    lastActivityMs_ = 0;
    dirty_ = true;

    LOG_D(TAG, "init: title='%s', items=%d", title ? title : "(null)", itemCount_);
}

/**
 * \brief Moves menu selection up or down with wrap-around.
 * \param down `true` to move down, `false` to move up.
 * \return void
 */
void ContextMenuView::navigate(bool down) {
    if (itemCount_ == 0) return;

    // Restart the inactivity timeout on navigation.
    lastActivityMs_ = 0;

    if (down) {
        if (selection_ < itemCount_ - 1) {
            selection_++;
        } else {
            selection_ = 0;  // Wrap around
            scrollPos_ = 0;
        }
    } else {
        if (selection_ > 0) {
            selection_--;
        } else {
            selection_ = itemCount_ - 1;  // Wrap around
        }
    }

    // Ensure visible
    if (selection_ >= scrollPos_ + VISIBLE_ITEMS) {
        scrollPos_ = selection_ - VISIBLE_ITEMS + 1;
    }
    if (selection_ < scrollPos_) {
        scrollPos_ = selection_;
    }

    dirty_ = true;
    LOG_D(TAG, "navigate: sel=%d, scroll=%d", selection_, scrollPos_);
}

/**
 * \brief Executes the currently selected context-menu item.
 * \return void
 */
void ContextMenuView::select() {
    if (selection_ < itemCount_) {
        const ContextMenuItem& item = items_[selection_];
        LOG_D(TAG, "select: item='%s'", item.label ? item.label : "(null)");

        // Hide menu first
        hideContextMenu();

        // Then call callback
        if (item.callback) {
            item.callback();
        }
    }
}

/**
 * \brief Handles key input for context menu navigation and actions.
 * \param key Pressed key code.
 * \return Input handling result for the view stack.
 */
InputResult ContextMenuView::onKey(char key) {
    switch (key) {
        case KEY_UP:
            navigate(false);
            return InputResult::CONSUMED;

        case KEY_DOWN:
            navigate(true);
            return InputResult::CONSUMED;

        case KEY_YES: // Select
            select();
            return InputResult::CONSUMED;

        case KEY_NO: // Cancel
            hideContextMenu();
            return InputResult::CONSUMED;

        default:
            return InputResult::IGNORED;
    }
}

/**
 * \brief Auto-dismisses the menu after a period of inactivity.
 * \param nowMs Current uptime in milliseconds.
 * \return void
 */
void ContextMenuView::onTick(uint32_t nowMs) {
    if (lastActivityMs_ == 0) {
        lastActivityMs_ = nowMs;
        return;
    }
    if (nowMs - lastActivityMs_ >= kMenuTimeoutMs) {
        hideContextMenu();
    }
}

/**
 * \brief Renders the context menu popup.
 * \param partial Indicates partial/full redraw mode.
 * \return void
 */
void ContextMenuView::render(bool partial) {
    hal::IDisplay* display = hal::getDisplayInstance();
    if (!display) return;

    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;

    const uint16_t screenWidth = display->getWidth();
    const uint16_t screenHeight = display->getHeight();

    // Calculate box dimensions
    int16_t x1, y1;
    uint16_t maxLabelWidth = 0;

    gfx->setTextSize(1);

    // Find widest label
    for (uint8_t i = 0; i < itemCount_; i++) {
        if (items_[i].label) {
            uint16_t w, h;
            gfx->getTextBounds(items_[i].label, 0, 0, &x1, &y1, &w, &h);
            if (w > maxLabelWidth) maxLabelWidth = w;
        }
    }

    // Also check title width
    if (title_) {
        uint16_t w, h;
        gfx->getTextBounds(title_, 0, 0, &x1, &y1, &w, &h);
        if (w > maxLabelWidth) maxLabelWidth = w;
    }

    uint16_t boxWidth = maxLabelWidth + BOX_PADDING * 2 + 10;
    if (boxWidth < MIN_BOX_WIDTH) boxWidth = MIN_BOX_WIDTH;
    if (boxWidth > MAX_BOX_WIDTH) boxWidth = MAX_BOX_WIDTH;

    uint8_t visibleCount = std::min(itemCount_, VISIBLE_ITEMS);
    uint16_t boxHeight = TITLE_HEIGHT + (visibleCount * ITEM_HEIGHT) + BOX_PADDING * 2;

    // Center the box
    int boxX = (screenWidth - boxWidth) / 2;
    int boxY = (screenHeight - boxHeight) / 2;

    // Draw box background
    render::drawDialogFrame(gfx, boxX, boxY, boxWidth, boxHeight);

    // Draw title
    gfx->setTextColor(EPD_WHITE);
    gfx->fillRect(boxX + 2, boxY + 2, boxWidth - 4, TITLE_HEIGHT, EPD_BLACK);
    gfx->setCursor(boxX + BOX_PADDING, boxY + 4);
    if (title_) {
        render::printText(gfx, title_);
    }

    // Draw items
    int itemY = boxY + TITLE_HEIGHT + BOX_PADDING;
    for (uint8_t i = 0; i < visibleCount; i++) {
        uint8_t itemIndex = scrollPos_ + i;
        if (itemIndex >= itemCount_) break;

        const ContextMenuItem& item = items_[itemIndex];

        // Clear item area
        gfx->fillRect(boxX + BOX_PADDING - 2, itemY, boxWidth - BOX_PADDING * 2 + 4, ITEM_HEIGHT, EPD_WHITE);

        // Highlight selected
        if (itemIndex == selection_) {
            gfx->fillRect(boxX + BOX_PADDING - 2, itemY, boxWidth - BOX_PADDING * 2 + 4, ITEM_HEIGHT - 1, EPD_BLACK);
            gfx->setTextColor(EPD_WHITE);
        } else {
            gfx->setTextColor(EPD_BLACK);
        }

        gfx->setCursor(boxX + BOX_PADDING, itemY + 2);
        if (item.label) {
            render::printText(gfx, item.label);
        }

        itemY += ITEM_HEIGHT;
    }

    // Scroll indicators if needed
    if (itemCount_ > VISIBLE_ITEMS) {
        int indicatorX = boxX + boxWidth - BOX_PADDING;
        int indicatorY = boxY + TITLE_HEIGHT + BOX_PADDING;

        // Up arrow
        if (scrollPos_ > 0) {
            gfx->fillTriangle(
                indicatorX, indicatorY + 6,
                indicatorX - 3, indicatorY + 2,
                indicatorX + 3, indicatorY + 2,
                EPD_BLACK
            );
        }

        // Down arrow
        if (scrollPos_ + VISIBLE_ITEMS < itemCount_) {
            int arrowY = boxY + boxHeight - BOX_PADDING - 8;
            gfx->fillTriangle(
                indicatorX, arrowY,
                indicatorX - 3, arrowY + 4,
                indicatorX + 3, arrowY + 4,
                EPD_BLACK
            );
        }
    }

    dirty_ = false;
}

/**
 * \brief Convenience helper functions.
 */

static ContextMenuView s_sharedContextMenu;

/**
 * \brief Shows the shared context menu instance as modal.
 * \param title Menu title text.
 * \param items Menu item array.
 * \param count Number of menu items.
 * \return Pointer to the shared `ContextMenuView` instance.
 */
ContextMenuView* showContextMenu(const char* title, const ContextMenuItem* items, uint8_t count) {
    s_sharedContextMenu.init(title, items, count);
    ViewStack::instance().showModal(&s_sharedContextMenu);
    return &s_sharedContextMenu;
}

/**
 * \brief Hides the active context menu modal.
 * \return void
 */
void hideContextMenu() {
    ViewStack::instance().hideModal();
}

} // namespace cdc::ui
