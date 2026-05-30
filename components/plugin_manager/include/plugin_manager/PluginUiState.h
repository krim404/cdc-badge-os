/**
 * \file PluginUiState.h
 * \brief Singleton that owns plugin-pushed UI views (lists, confirms, inputs).
 *
 * Bridges between the host_ui_* C-API surface called from WASM and the
 * cdc::ui::ViewStack. Holds the per-view callback wiring + the small amount
 * of mutable state the host needs (last entered text/int/date/time, the
 * exclusive-lock token, and the inactivity action id) so host_api_ui_views.cpp
 * stays a thin extern-C adapter.
 */

#pragma once

#include "plugin_manager/Raii.h"
#include "plugin_manager/host_api.h"

#include "cdc_views/ConfirmView.h"
#include "cdc_views/ListView.h"
#include "cdc_views/ContextMenuView.h"
#include "cdc_views/T9InputView.h"
#include "cdc_views/PinEntryView.h"
#include "cdc_views/SliderView.h"
#include "cdc_views/ColorPickerView.h"
#include "cdc_views/DateInputView.h"
#include "cdc_views/TimeInputView.h"
#include "cdc_views/CanvasView.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cdc::plugin_manager {

class PluginUiState {
public:
    [[nodiscard]] static PluginUiState& instance() noexcept;

    // View push helpers - return HOST_OK or HOST_ERR_*.
    // `replace_top` means: if our previous list view is the current top of the
    // view stack, swap it out (no second push). Lets a plugin "refresh" its
    // own list without growing the stack.
    [[nodiscard]] int pushList   (const char* title, const ui_item_t* items, uint16_t count,
                                  uint32_t select_action_id, uint32_t menu_action_id,
                                  bool replace_top = false);
    /// Redraw a single row of the current plugin list in place (partial refresh).
    [[nodiscard]] int updateListItem(uint16_t index, const ui_item_t* item);
    /// Insert a row at `index` into the current plugin list (rebuild + partial refresh).
    [[nodiscard]] int insertListItem(uint16_t index, const ui_item_t* item);
    /// Remove the row at `index` from the current plugin list (rebuild + partial refresh).
    [[nodiscard]] int removeListItem(uint16_t index);
    [[nodiscard]] int pushContextMenu(const char* title, const ui_item_t* items, uint16_t count,
                                       uint32_t select_action_id);
    [[nodiscard]] int pushConfirm(const char* text, uint8_t icon, uint32_t action_id);
    [[nodiscard]] int pushT9     (const char* title, const char* initial,
                                  uint16_t max_len, uint32_t action_id);
    [[nodiscard]] int pushPin    (const char* title, uint8_t max_len, uint8_t max_attempts,
                                  uint32_t action_id);
    [[nodiscard]] int pushSlider (const char* title, int32_t min, int32_t max, int32_t init,
                                  int32_t step, const char* unit, uint32_t action_id);
    [[nodiscard]] int pushDate   (const char* title, uint8_t d, uint8_t m, uint16_t y,
                                  uint32_t action_id);
    [[nodiscard]] int pushTime   (const char* title, uint8_t h, uint8_t m, uint32_t action_id);
    [[nodiscard]] int pushColorPicker(uint8_t r, uint8_t g, uint8_t b, uint32_t action_id);

    [[nodiscard]] int pushCanvas (const char* title, uint32_t key_action_id,
                                  uint32_t widget_action_id);
    [[nodiscard]] cdc::ui::CanvasView* canvasView();

    [[nodiscard]] int acquireExclusive();
    [[nodiscard]] int releaseExclusive();
    [[nodiscard]] int setInactivity(uint32_t timeout_ms, uint32_t action_id);

    [[nodiscard]] int consumeInputText(char* out, size_t out_size);
    [[nodiscard]] int consumeInputInt (int32_t* out);
    [[nodiscard]] int setViewFooter   (const char* hint);
    [[nodiscard]] int setViewEmpty    (const char* text);

    /// Drop ownership of every plugin-pushed view and reset transient state.
    /// Called by PluginManager when the active plugin stops; views that may
    /// still be on the view stack (popped naturally or not) are destroyed
    /// only here, never inline while user input is being dispatched.
    void resetForPluginStop();

private:
    PluginUiState() = default;
    ~PluginUiState() = default;
    PluginUiState(const PluginUiState&) = delete;
    PluginUiState& operator=(const PluginUiState&) = delete;

    struct ListState {
        std::unique_ptr<cdc::ui::ListView> view;
        PsramUniquePtr<cdc::ui::ListItem>  items;      // capacity-sized
        PsramUniquePtr<char>               title_buf;
        PsramUniquePtr<char>               footer_buf;
        PsramUniquePtr<char>               empty_buf;
        PsramUniquePtr<uint32_t>           item_ids;   // capacity-sized
        // One owned label buffer per row (size == count); items[i].label points
        // at labels[i]. Kept aligned with items on insert/remove so rows move in
        // place without rebuilding a packed string pool or re-init'ing the view.
        std::vector<PsramUniquePtr<char>>  labels;
        uint32_t                           select_action_id = 0;
        uint32_t                           menu_action_id   = 0;
        uint16_t                           count            = 0;
        uint16_t                           capacity         = 0;
    };
    struct ContextMenuState {
        std::unique_ptr<cdc::ui::ContextMenuView> view;
        PsramUniquePtr<cdc::ui::ContextMenuItem>  items;
        PsramUniquePtr<char>                      string_pool;
        PsramUniquePtr<char>                      title_buf;
        PsramUniquePtr<uint32_t>                  item_ids;
        uint32_t                                  select_action_id = 0;
        uint16_t                                  count            = 0;
    };
    struct ConfirmState {
        std::unique_ptr<cdc::ui::ConfirmView> view;
        PsramUniquePtr<char>                  footer_buf;
        uint32_t                              action_id = 0;
    };
    struct InputState {
        std::unique_ptr<cdc::ui::T9InputView>      t9_view;
        std::unique_ptr<cdc::ui::PinEntryView>     pin_view;
        std::unique_ptr<cdc::ui::SliderView>       slider_view;
        std::unique_ptr<cdc::ui::DateInputView>    date_view;
        std::unique_ptr<cdc::ui::TimeInputView>    time_view;
        std::unique_ptr<cdc::ui::ColorPickerView>  color_view;
        PsramUniquePtr<char>                       footer_buf;
        PsramUniquePtr<char>                       title_buf;
        PsramUniquePtr<char>                       unit_buf;
        uint32_t                                   action_id = 0;
        std::string                                last_text;
        int32_t                                    last_int  = 0;
        bool                                       has_int   = false;
        uint32_t                                   last_date = 0;
        uint16_t                                   last_time = 0;
    };
    struct CanvasState {
        std::unique_ptr<cdc::ui::CanvasView> view;
        PsramUniquePtr<char>                 title_buf;
        PsramUniquePtr<char>                 footer_buf;
        uint32_t                             key_action_id    = 0;
        uint32_t                             widget_action_id = 0;
    };

    // Callbacks invoked by ViewStack-managed views. They forward via the
    // singleton so the underlying cdc::ui types stay unaware of plugins.
    static void onListSelect(uint16_t index, void* userData);
    static void onListMenu  (uint16_t index, void* userData);
    static void onConfirmYes(void* userData);
    static void onConfirmNo (void* userData);
    static void onT9Save    (const char* text);
    static bool onPinVerify (const char* pin);
    static void onSliderSave(uint16_t value);
    static void onDateSave  (uint8_t day, uint8_t month, uint16_t year);
    static void onTimeSave  (uint8_t hour, uint8_t minute);
    static void onColorSave (uint8_t r, uint8_t g, uint8_t b);
    static void onInactivity();
    static void onCanvasKey   (char key, uint32_t focused_widget);
    static void onCanvasWidget(uint32_t widget_id, cdc::ui::CanvasView::WidgetEvent event);

    /// Grow the active list's capacity arrays (and re-point the view) so an
    /// insert has room. Returns false on OOM. Caller must hold listEditMutex.
    bool growList(uint16_t need);

public:
    void dispatchContextSelect(uint8_t idx);

private:

    std::unique_ptr<ListState> list_;
    ContextMenuState ctxmenu_{};
    ConfirmState     confirm_{};
    InputState       input_{};
    CanvasState      canvas_{};
    // Earlier ListStates that were superseded by another pushList while still
    // on the view stack. Destroyed in bulk on plugin stop so views that
    // ViewStack still references are never freed mid-dispatch.
    std::vector<std::unique_ptr<ListState>>     list_graveyard_;
    const void*  exclusive_token_   = nullptr;
    uint32_t     inactivity_action_ = 0;
};

}  // namespace cdc::plugin_manager
