#pragma once

#include "cdc_ui/IView.h"
#include <cstdint>

namespace cdc::ui {

/**
 * Context menu item
 */
struct ContextMenuItem {
    const char* label;
    void (*callback)();
};

/**
 * ContextMenuView - Quick popup menu overlay
 *
 * Shows a centered popup menu that can be triggered from anywhere.
 * Displayed as a modal overlay on top of the current view.
 *
 * Keys:
 *   2 = Up
 *   8 = Down
 *   Y = Select
 *   N = Cancel (close menu)
 */
class ContextMenuView : public ViewBase {
public:
    static constexpr uint8_t MAX_ITEMS = 8;
    static constexpr uint8_t VISIBLE_ITEMS = 4;

    /**
     * Initialize context menu
     * @param title Menu title
     * @param items Array of menu items
     * @param count Number of items
     */
    void init(const char* title, const ContextMenuItem* items, uint8_t count);

    /**
     * Get current selection
     */
    uint8_t getSelection() const { return selection_; }

    // IView implementation
    void render(bool partial) override;
    InputResult onKey(char key) override;
    void onTick(uint32_t nowMs) override;
    const char* getName() const override { return "ContextMenuView"; }
    const char* getFooterHint() const override { return nullptr; }

private:
    const char* title_ = nullptr;
    // Items are copied so callers can safely use stack arrays; the i18n
    // strings the labels point to are stable for the program lifetime.
    ContextMenuItem items_[MAX_ITEMS] = {};
    uint8_t itemCount_ = 0;
    uint8_t selection_ = 0;
    uint8_t scrollPos_ = 0;
    // Uptime (ms) of the last interaction; 0 until the first tick after show.
    // Used for the auto-dismiss inactivity timeout.
    uint32_t lastActivityMs_ = 0;

    void navigate(bool down);
    void select();
};

// ============================================================================
// Convenience Functions
// ============================================================================

/**
 * Show a context menu as modal overlay.
 * The menu will close after selection or cancel.
 *
 * @param title Menu title
 * @param items Array of menu items
 * @param count Number of items
 * @return Pointer to the ContextMenuView
 *
 * Example:
 *   static ContextMenuItem items[] = {
 *       {"Copy", []() { doCopy(); }},
 *       {"Paste", []() { doPaste(); }},
 *       {"Delete", []() { doDelete(); }}
 *   };
 *   showContextMenu("Actions", items, 3);
 */
ContextMenuView* showContextMenu(const char* title, const ContextMenuItem* items, uint8_t count);

/**
 * Hide the context menu (if visible)
 */
void hideContextMenu();

} // namespace cdc::ui
