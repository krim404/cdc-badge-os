/**
 * \file host_api_ui_views.cpp
 * \brief extern-C adapter that forwards plugin UI calls to PluginUiState.
 *
 * Push a view, register a forwarding callback, and the host dispatches
 * `plugin_on_action(action_id, ...)` to the active plugin when the user
 * acts. All state lives in PluginUiState - this TU only marshals C arguments
 * into method calls on the singleton.
 */

#include "plugin_manager/PluginUiState.h"
#include "plugin_manager/host_api.h"

extern "C" {

int host_ui_push_list(const char* title, const ui_item_t* items, uint16_t count,
                      uint32_t select_action_id, uint32_t menu_action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .pushList(title, items, count, select_action_id, menu_action_id, false);
}

int host_ui_replace_list(const char* title, const ui_item_t* items, uint16_t count,
                         uint32_t select_action_id, uint32_t menu_action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .pushList(title, items, count, select_action_id, menu_action_id, true);
}

int host_ui_set_view_footer(const char* hint)
{
    return cdc::plugin_manager::PluginUiState::instance().setViewFooter(hint);
}

int host_ui_set_view_empty(const char* text)
{
    return cdc::plugin_manager::PluginUiState::instance().setViewEmpty(text);
}

int host_ui_set_view_lifecycle(uint32_t hide_action_id, uint32_t show_action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .setViewLifecycle(hide_action_id, show_action_id);
}

int host_ui_update_list_item(uint16_t index, const ui_item_t* item)
{
    return cdc::plugin_manager::PluginUiState::instance().updateListItem(index, item);
}

int host_ui_insert_list_item(uint16_t index, const ui_item_t* item)
{
    return cdc::plugin_manager::PluginUiState::instance().insertListItem(index, item);
}

int host_ui_remove_list_item(uint16_t index)
{
    return cdc::plugin_manager::PluginUiState::instance().removeListItem(index);
}

int host_ui_push_context_menu(const char* title, const ui_item_t* items, uint16_t count,
                              uint32_t select_action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .pushContextMenu(title, items, count, select_action_id);
}

int host_ui_push_confirm(const char* text, uint8_t icon, uint32_t action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .pushConfirm(text, icon, action_id);
}

int host_ui_push_t9_input(const char* title, const char* initial,
                          uint16_t max_len, uint32_t action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .pushT9(title, initial, max_len, action_id);
}

int host_ui_push_password(const char* title, const char* initial,
                          uint16_t max_len, uint32_t action_id)
{
    // V1: reuse the T9 input - a dedicated masked variant comes later.
    return host_ui_push_t9_input(title, initial, max_len, action_id);
}

int host_ui_push_pin_entry(const char* title, uint8_t max_len, uint8_t max_attempts,
                           uint32_t action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .pushPin(title, max_len, max_attempts, action_id);
}

int host_ui_push_slider(const char* title, int32_t min, int32_t max, int32_t init,
                        int32_t step, const char* unit, uint32_t action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .pushSlider(title, min, max, init, step, unit, action_id);
}

int host_ui_push_color_picker(uint8_t r, uint8_t g, uint8_t b, uint32_t action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .pushColorPicker(r, g, b, action_id);
}

int host_ui_push_date(const char* title, uint8_t d, uint8_t m, uint16_t y, uint32_t action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .pushDate(title, d, m, y, action_id);
}

int host_ui_push_time(const char* title, uint8_t h, uint8_t m, uint32_t action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .pushTime(title, h, m, action_id);
}

int host_ui_acquire_exclusive(void)
{
    return cdc::plugin_manager::PluginUiState::instance().acquireExclusive();
}

int host_ui_release_exclusive(void)
{
    return cdc::plugin_manager::PluginUiState::instance().releaseExclusive();
}

int host_ui_set_inactivity(uint32_t timeout_ms, uint32_t action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .setInactivity(timeout_ms, action_id);
}

int host_ui_consume_input_text(char* out, size_t out_size)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .consumeInputText(out, out_size);
}

int host_ui_consume_input_int(int32_t* out)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .consumeInputInt(out);
}

}  // extern "C"
