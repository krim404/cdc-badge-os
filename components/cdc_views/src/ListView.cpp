/**
 * ListView Implementation
 *
 * Highly reusable scrollable selection menu.
 * Display is 296x128, VISIBLE_ITEMS is fixed at 4.
 */

#include "cdc_views/ListView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/LayoutConstants.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_ui/I18n.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"
#include <goodisplay/gdey029T94.h>

static const char* TAG = "ListView";

/**
 * \brief View-local layout constants for 296x128 panels.
 *
 * Shared SCROLL_INDICATOR_WIDTH comes from cdc::ui::layout in
 * LayoutConstants.h.
 */
static constexpr int TITLE_Y = 5;
static constexpr int LIST_START_Y = 30;
static constexpr int ITEM_PADDING_X = 10;
using cdc::ui::layout::SCROLL_INDICATOR_WIDTH;

/**
 * \brief Visible item count derived from available list area.
 *
 * Available height: 128 - 30 (header) - 16 (footer) = 82px.
 * With `itemHeight_=18`: 82 / 18 = 4 visible rows.
 */
static constexpr uint8_t VISIBLE_ITEMS = 4;

namespace cdc::ui {

/**
 * \brief Initializes list data and selection state.
 * \param title List title text.
 * \param items Item array pointer.
 * \param count Number of items in `items`.
 * \return void
 */
void ListView::init(const char* title, const ListItem* items, uint16_t count) {
    title_ = title;
    items_ = items;
    itemCount_ = count > MAX_ITEMS ? MAX_ITEMS : count;

    // Only reset position if not preserving (for back-navigation)
    if (!preservePosition_) {
        selection_ = 0;
        scrollPos_ = 0;
    } else {
        preservePosition_ = false;
        if (selection_ >= itemCount_) {
            selection_ = itemCount_ > 0 ? itemCount_ - 1 : 0;
        }
        ensureVisible();
    }
    visibleItems_ = VISIBLE_ITEMS;
    dirty_ = true;

    LOG_D(TAG, "init: title='%s', items=%d, visible=%d", title, itemCount_, visibleItems_);
}

/**
 * \brief Sets the selected item index.
 * \param index Target selection index.
 * \return void
 */
void ListView::setSelection(uint16_t index) {
    if (index < itemCount_ && index != selection_) {
        selection_ = index;
        ensureVisible();
        dirty_ = true;
    }
}

/**
 * \brief Returns the currently selected item.
 * \return Pointer to selected item or `nullptr`.
 */
const ListItem* ListView::getSelectedItem() const {
    if (items_ && selection_ < itemCount_) {
        return &items_[selection_];
    }
    return nullptr;
}

/**
 * \brief Navigates the selection up or down.
 * \param down `true` to move down, `false` to move up.
 * \return void
 */
void ListView::navigate(bool down) {
    if (itemCount_ == 0) return;

    if (down) {
        if (selection_ < itemCount_ - 1) {
            selection_++;
        } else {
            // Wrap-around: bottom to top
            selection_ = 0;
            scrollPos_ = 0;
        }
    } else {
        if (selection_ > 0) {
            selection_--;
        } else {
            // Wrap-around: top to bottom
            selection_ = itemCount_ - 1;
        }
    }

    ensureVisible();
    dirty_ = true;

    LOG_D(TAG, "navigate: sel=%d, scroll=%d", selection_, scrollPos_);
}

/**
 * \brief Adjusts scroll position so the selected item is visible.
 * \return void
 */
void ListView::ensureVisible() {
    if (selection_ >= scrollPos_ + visibleItems_) {
        scrollPos_ = selection_ - visibleItems_ + 1;
    }
    if (selection_ < scrollPos_) {
        scrollPos_ = selection_;
    }
}

/**
 * \brief Handles key input for list navigation and actions.
 * \param key Pressed key code.
 * \return Input handling result for the view stack.
 */
InputResult ListView::onKey(char key) {
    switch (key) {
        case KEY_UP:
            navigate(false);
            return InputResult::CONSUMED;

        case KEY_DOWN:
            navigate(true);
            return InputResult::CONSUMED;

        case KEY_YES: // Select
            if (onSelect_ && items_ && selection_ < itemCount_) {
                onSelect_(selection_, items_[selection_].userData);
            }
            return InputResult::CONSUMED;

        case KEY_BACK: // Context menu
            if (onMenu_) {
                if (items_ && selection_ < itemCount_) {
                    onMenu_(selection_, items_[selection_].userData);
                } else {
                    // Empty list: surface a "no-selection" menu opportunity.
                    onMenu_(0xFFFF, nullptr);
                }
                return InputResult::CONSUMED;
            }
            return InputResult::IGNORED;

        case KEY_NO: // Back
            return InputResult::REQUEST_POP;

        default:
            return InputResult::IGNORED;
    }
}

/**
 * \brief Returns footer hint text for this list.
 * \return Footer hint string.
 */
const char* ListView::getFooterHint() const {
    if (customFooter_) {
        return customFooter_;
    }
    if (customHint_) {
        return customHint_;
    }
    return ui::tr("core.hint_ok_back");
}

/**
 * \brief Renders list rows, selection, scroll indicators, and footer.
 * \param partial Indicates partial/full redraw mode.
 * \return void
 */
void ListView::render(bool partial) {
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
    gfx->setTextWrap(false);

    // Title + underline
    render::drawHeaderLeft(gfx, title_, ITEM_PADDING_X, TITLE_Y, width);

    // Items
    const int rowWidth = width - SCROLL_INDICATOR_WIDTH;
    for (uint8_t i = 0; i < visibleItems_; i++) {
        uint16_t itemIndex = scrollPos_ + i;
        int y = LIST_START_Y + i * itemHeight_;

        // Clear item area
        gfx->fillRect(0, y, rowWidth, itemHeight_, EPD_WHITE);

        if (itemIndex >= itemCount_) continue;

        const ListItem& item = items_[itemIndex];
        bool isSelected = (itemIndex == selection_);

        if (isSelected) {
            gfx->fillRect(2, y + 1, rowWidth - 4, itemHeight_ - 2, EPD_BLACK);
            gfx->setTextColor(EPD_WHITE);
        } else {
            gfx->setTextColor(EPD_BLACK);
        }

        bool handled = false;
        if (itemRenderer_) {
            handled = itemRenderer_(gfx, item, itemIndex,
                                    0, y, rowWidth, itemHeight_,
                                    isSelected, itemRendererCtx_);
        }

        if (!handled) {
            int textX = ITEM_PADDING_X;
            if (item.icon) {
                char iconStr[2] = {static_cast<char>(item.icon), '\0'};
                gfx->setCursor(textX, y + 4);
                render::printText(gfx, iconStr);
                if (item.iconDisabled) {
                    uint16_t color = isSelected ? EPD_WHITE : EPD_BLACK;
                    gfx->drawLine(textX, y + 10, textX + 6, y + 10, color);
                }
                textX += 10;
            } else {
                uint16_t bullet_color = isSelected ? EPD_WHITE : EPD_BLACK;
                gfx->fillCircle(textX + 2, y + itemHeight_ / 2, 2, bullet_color);
                textX += 9;
            }

            gfx->setCursor(textX, y + 4);
            if (item.label) {
                render::printTruncated(gfx, item.label, rowWidth - textX - 2);
            }
        }
    }

    // Empty placeholder (after item rects so it's not overpainted)
    if (itemCount_ == 0 && emptyText_) {
        int16_t x1, y1;
        uint16_t w, h;
        gfx->setTextColor(EPD_BLACK);
        gfx->setTextSize(1);
        gfx->getTextBounds(emptyText_, 0, 0, &x1, &y1, &w, &h);
        int x = (width - static_cast<int>(w)) / 2;
        int y = LIST_START_Y + (visibleItems_ * itemHeight_) / 2 - h / 2;
        gfx->setCursor(x < 0 ? 0 : x, y);
        render::printText(gfx, emptyText_);
    }

    // Scroll indicators
    if (itemCount_ > visibleItems_) {
        int indicatorX = width - SCROLL_INDICATOR_WIDTH;
        int listHeight = visibleItems_ * itemHeight_;
        render::drawScrollIndicator(gfx, indicatorX, LIST_START_Y, listHeight,
                                    itemCount_, visibleItems_, scrollPos_);
    }

    // Footer with position counter
    char positionStr[16];
    const char* prefix = nullptr;
    if (itemCount_ > 0) {
        snprintf(positionStr, sizeof(positionStr), "%u/%u  ", selection_ + 1, itemCount_);
        prefix = positionStr;
    }
    const char* hint = getFooterHint();
    render::drawFooterBar(gfx, width, height, prefix, hint, true);

    dirty_ = false;
}

/**
 * \brief Convenience factory/helper function.
 */

static ListView s_sharedListView;

/**
 * \brief Shows a shared list view instance with selection callback.
 * \param title List title text.
 * \param items Item array pointer.
 * \param count Number of items.
 * \param onSelect Selection callback.
 * \param hint Optional footer hint override.
 * \return Pointer to the shared `ListView` instance.
 */
ListView* showListView(const char* title, const ListItem* items, uint16_t count,
                       ListView::SelectCallback onSelect, const char* hint) {
    s_sharedListView.init(title, items, count);
    s_sharedListView.setOnSelect(onSelect);
    if (hint) {
        s_sharedListView.setHint(hint);
    }
    ViewStack::instance().push(&s_sharedListView);
    return &s_sharedListView;
}

} // namespace cdc::ui
