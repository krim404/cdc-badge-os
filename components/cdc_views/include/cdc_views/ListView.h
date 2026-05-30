#pragma once

#include "cdc_ui/IView.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdint>

class Gdey029T94;

namespace cdc::ui {

/**
 * List item for ListView
 */
struct ListItem {
    const char* label;          // Display text
    uint8_t icon = 0;           // Icon type (0 = none)
    bool iconDisabled = false;  // Draw icon crossed-out
    void* userData = nullptr;   // Optional user data
};

/**
 * ListView - Highly dynamic scrollable selection menu
 *
 * Reusable component for any list-based UI.
 * Dynamically calculates visible items based on display size.
 *
 * Keys:
 *   2 = Up
 *   8 = Down
 *   Y = Select (triggers callback)
 *   N = Back (REQUEST_POP)
 */
class ListView : public ViewBase {
public:
    static constexpr uint16_t MAX_ITEMS = 2048;
    static constexpr uint8_t DEFAULT_ITEM_HEIGHT = 18;
    // The actual visible-row count is the file-scope VISIBLE_ITEMS in
    // ListView.cpp (fixed at 4 for the 296x128 panel).

    /**
     * Selection callback
     * @param index Selected item index
     * @param userData User data from ListItem
     */
    using SelectCallback = void(*)(uint16_t index, void* userData);

    /**
     * Context/menu callback (e.g., key '3')
     * @param index Selected item index
     * @param userData User data from ListItem
     */
    using MenuCallback = void(*)(uint16_t index, void* userData);

    /**
     * Optional per-row renderer.
     * Return true if the row is fully rendered, false to fall back to default.
     */
    using ItemRenderCallback = bool(*)(Gdey029T94* gfx,
                                       const ListItem& item,
                                       uint16_t index,
                                       int x, int y, int w, int h,
                                       bool selected,
                                       void* userCtx);

    /**
     * Initialize list view
     * @param title List title
     * @param items Array of items (pointer is stored, not copied)
     * @param count Number of items
     */
    void init(const char* title, const ListItem* items, uint16_t count);

    /**
     * Set selection callback
     */
    void setOnSelect(SelectCallback callback) { onSelect_ = callback; }

    /**
     * Set context menu callback (key '3')
     */
    void setOnMenu(MenuCallback callback) { onMenu_ = callback; }

    /**
     * Set an optional recursive mutex serialising buffer access.
     *
     * When set (non-null), render(), navigation and the item-buffer reads in
     * onKey() acquire it. A cross-task writer that re-points the backing array
     * via init()/repaintPartial() must hold the same mutex around the swap, so
     * the UI task never reads a half-replaced or freed buffer. Null (default)
     * disables locking entirely for single-task lists.
     */
    void setEditMutex(SemaphoreHandle_t mutex) { editMutex_ = mutex; }

    /**
     * Set optional row renderer
     */
    void setItemRenderer(ItemRenderCallback callback, void* userCtx = nullptr) {
        itemRenderer_ = callback;
        itemRendererCtx_ = userCtx;
    }

    /**
     * Set custom footer hint text (nullptr = default)
     */
    void setHint(const char* hint) { customHint_ = hint; }

    /**
     * Set placeholder text shown in the body area when the list is empty.
     */
    void setEmptyText(const char* text) { emptyText_ = text; }

    /**
     * Set custom item height (0 = auto-calculate)
     */
    void setItemHeight(uint8_t height) { itemHeight_ = height > 0 ? height : DEFAULT_ITEM_HEIGHT; }

    /**
     * Get current selection index
     */
    uint16_t getSelection() const { return selection_; }

    /**
     * Set current selection
     */
    void setSelection(uint16_t index);

    /**
     * Get item count
     */
    uint16_t getItemCount() const { return itemCount_; }

    /**
     * Get selected item
     */
    const ListItem* getSelectedItem() const;

    /**
     * Redraw a single list row in place and partial-refresh the panel.
     *
     * The backing item (in the array passed to init(), whose pointer is stored)
     * must already be updated by the caller; this only repaints that one row,
     * avoiding a full render() of the whole view. No-op when the item is not
     * currently on screen (it will appear on the next scroll/render). Must be
     * called from the UI task.
     *
     * @param index Item index to redraw.
     */
    void updateItem(uint16_t index);

    /**
     * Insert-notify: the backing array (passed to init(), pointer stored) has
     * had one item inserted at `index` (shifting later items down); the caller
     * must have done that shift and kept capacity. Bumps the count, keeps the
     * currently-selected item selected, and partial-repaints. Must be called
     * from the UI task.
     *
     * @param index Position the new item was inserted at (clamped to count).
     */
    void insertItem(uint16_t index);

    /**
     * Remove-notify: the backing array has had the item at `index` removed
     * (shifting later items up); the caller must have done that shift. Drops
     * the count, keeps the selection valid, and partial-repaints. Must be
     * called from the UI task.
     *
     * @param index Index of the removed item.
     */
    void removeItem(uint16_t index);

    /**
     * Repaint the visible rows in partial-refresh mode and flush immediately.
     * For out-of-band updates (after re-pointing items via init()) that should
     * not wait for the next render cycle. Must be called from the UI task.
     */
    void repaintPartial();

    /**
     * Preserve current position (selection and scroll) on next init
     * Call this before init() to retain position when returning to the list
     */
    void preservePosition() { preservePosition_ = true; }

    // IView implementation
    void render(bool partial) override;
    InputResult onKey(char key) override;
    InputResult onLongPress(char key) override;
    const char* getName() const override { return "ListView"; }
    const char* getFooterHint() const override;

private:
    const char* title_ = nullptr;
    const char* customHint_ = nullptr;
    const char* emptyText_ = nullptr;
    const ListItem* items_ = nullptr;
    uint16_t itemCount_ = 0;
    uint16_t selection_ = 0;
    uint16_t scrollPos_ = 0;
    SelectCallback onSelect_ = nullptr;
    MenuCallback onMenu_ = nullptr;
    ItemRenderCallback itemRenderer_ = nullptr;
    void* itemRendererCtx_ = nullptr;
    bool preservePosition_ = false;
    uint8_t itemHeight_ = DEFAULT_ITEM_HEIGHT;
    // Optional, borrowed (not owned): serialises item-buffer access with a
    // cross-task writer. Null disables locking. See setEditMutex().
    SemaphoreHandle_t editMutex_ = nullptr;

    // Calculated at render time based on display dimensions
    uint8_t visibleItems_ = 4;

    void navigate(bool down);
    void ensureVisible();
    void drawRow(Gdey029T94* gfx, uint16_t itemIndex, int y, int rowWidth);
};

// ============================================================================
// Convenience Functions
// ============================================================================

/**
 * Show a list menu and push it to the ViewStack.
 * Simplest possible API: "here's the array and callback, show it".
 *
 * @param title List title
 * @param items Array of items (pointer is stored)
 * @param count Number of items
 * @param onSelect Callback when item is selected
 * @param hint Optional custom footer hint (nullptr = default)
 * @return Pointer to the ListView (for further configuration if needed)
 *
 * Example:
 *   static ListItem items[] = { {"Option 1"}, {"Option 2"} };
 *   showListView("Title", items, 2, [](uint16_t idx, void*) { ... });
 */
ListView* showListView(const char* title, const ListItem* items, uint16_t count,
                       ListView::SelectCallback onSelect, const char* hint = nullptr);

} // namespace cdc::ui
