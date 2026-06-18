/**
 * \file WamrImports.cpp
 * \brief NativeSymbol table bound under WAMR module "cdc".
 *
 * Each wrapper here translates between WAMR's calling convention
 * (wasm_exec_env_t + WASM-validated argument types) and the host_api_*.cpp
 * implementations. Wrappers stay thin - the actual logic always lives in
 * the host_api_<family>.cpp files.
 */

#include "plugin_manager/WamrImports.h"
#include "plugin_manager/host_api.h"
#include "cdc_core/Raii.h"
#include "cdc_views/ListView.h"
#include "cdc_views/ContextMenuView.h"

extern "C" {
#include "wasm_export.h"
void plg_log_info (const char* msg);
void plg_log_warn (const char* msg);
void plg_log_error(const char* msg);
}

#include <cstdio>
#include <cstring>

namespace cdc::plugin_manager {

// Wrapper signature notation:
//   i = i32, I = i64, $ = null-terminated string, *~ = buffer+length.
// WAMR injects wasm_exec_env_t as the first C argument automatically.

#define W(name, fn, sig)  { name, (void*)fn, sig, nullptr }

// WAMR validates only 1 byte for a bare '*' pointer parameter (a pointer not
// immediately followed by '~length' in the signature). Any host call that reads
// or writes more than 1 byte through such a pointer must re-validate the full
// extent against the calling instance's linear memory, otherwise a plugin can
// place the pointer at the end of linear memory and drive an out-of-bounds
// access into native heap. Returns false (caller maps to HOST_ERR_INVALID_ARG)
// when [ptr, ptr+len) is not fully inside the plugin's linear memory.
static inline bool wbuf_ok(wasm_exec_env_t env, const void* ptr, size_t len)
{
    if (len == 0) return true;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    return ptr && wasm_runtime_validate_native_addr(inst, const_cast<void*>(ptr),
                                                     static_cast<uint64_t>(len));
}

// -- Logging ----------------------------------------------------------------

static void w_host_log(wasm_exec_env_t, uint32_t level, const char* tag, const char* msg)
{ host_log(static_cast<uint8_t>(level), tag, msg); }

// -- Time / Power -----------------------------------------------------------

static uint64_t w_host_uptime_ms(wasm_exec_env_t)        { return host_uptime_ms(); }
static int64_t  w_host_unix_time(wasm_exec_env_t)        { return host_unix_time(); }
static int32_t  w_host_is_time_set(wasm_exec_env_t)      { return host_is_time_set() ? 1 : 0; }
static int32_t  w_host_timezone_offset(wasm_exec_env_t)  { return host_timezone_offset(); }

static uint32_t w_host_battery_mv(wasm_exec_env_t)          { return host_battery_mv(); }
static uint32_t w_host_battery_pct(wasm_exec_env_t)         { return host_battery_pct(); }
static int32_t  w_host_is_usb_connected(wasm_exec_env_t)    { return host_is_usb_connected(); }
static uint32_t w_host_power_source(wasm_exec_env_t)        { return host_power_source(); }
static uint32_t w_host_charge_status(wasm_exec_env_t)       { return host_charge_status(); }
static int32_t  w_host_is_battery_low(wasm_exec_env_t)      { return host_is_battery_low(); }
static int32_t  w_host_is_battery_critical(wasm_exec_env_t) { return host_is_battery_critical(); }
static void     w_host_set_sleep_inhibit(wasm_exec_env_t, uint32_t on) { host_set_sleep_inhibit(on); }

// -- UI ---------------------------------------------------------------------

static int32_t w_host_ui_push_toast(wasm_exec_env_t, const char* t, uint32_t icon, uint32_t ms)
{ return host_ui_push_toast(t, icon, static_cast<uint16_t>(ms)); }

static int32_t w_host_ui_push_message(wasm_exec_env_t, const char* t, uint32_t icon, uint32_t ms)
{ return host_ui_push_message(t, icon, ms); }

static int32_t w_host_ui_push_info(wasm_exec_env_t, const char* title, const char* body)
{ return host_ui_push_info(title, body); }

static int32_t w_host_ui_push_confirm(wasm_exec_env_t, const char* text, uint32_t icon, uint32_t action_id)
{ return host_ui_push_confirm(text, icon, action_id); }

static int32_t w_host_browser_open(wasm_exec_env_t, const char* url)
{ return host_browser_open(url); }

namespace {
int translate_and_call(wasm_exec_env_t exec_env, const char* title, const ui_item_t* items,
                       uint32_t count, uint32_t sel, uint32_t menu, bool replace)
{
    if (count == 0 || !items) {
        return replace ? host_ui_replace_list(title, items, 0, sel, menu)
                       : host_ui_push_list   (title, items, 0, sel, menu);
    }
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (count > cdc::ui::ListView::MAX_ITEMS) count = cdc::ui::ListView::MAX_ITEMS;
    if (!wasm_runtime_validate_native_addr(inst, const_cast<ui_item_t*>(items),
                                           static_cast<uint64_t>(count) * sizeof(ui_item_t)))
        return HOST_ERR_INVALID_ARG;
    auto native = cdc::core::psramAlloc<ui_item_t>(count);
    if (!native) return HOST_ERR_NO_MEMORY;
    for (uint32_t i = 0; i < count; ++i) {
        native[i] = items[i];
        uint32_t off = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(items[i].label));
        if (off != 0 && wasm_runtime_validate_app_str_addr(inst, off)) {
            native[i].label = static_cast<const char*>(wasm_runtime_addr_app_to_native(inst, off));
        } else {
            native[i].label = "";
        }
    }
    return replace ? host_ui_replace_list(title, native.get(), static_cast<uint16_t>(count), sel, menu)
                   : host_ui_push_list   (title, native.get(), static_cast<uint16_t>(count), sel, menu);
}
}  // namespace

static int32_t w_host_ui_push_list(wasm_exec_env_t exec_env, const char* title, const ui_item_t* items,
                                   uint32_t count, uint32_t sel, uint32_t menu)
{
    return translate_and_call(exec_env, title, items, count, sel, menu, false);
}

static int32_t w_host_ui_replace_list(wasm_exec_env_t exec_env, const char* title, const ui_item_t* items,
                                      uint32_t count, uint32_t sel, uint32_t menu)
{
    return translate_and_call(exec_env, title, items, count, sel, menu, true);
}

static int32_t w_host_ui_set_view_footer(wasm_exec_env_t, const char* hint)
{
    return host_ui_set_view_footer(hint);
}

static int32_t w_host_ui_set_view_empty(wasm_exec_env_t, const char* text)
{
    return host_ui_set_view_empty(text);
}

static int32_t w_host_ui_set_view_lifecycle(wasm_exec_env_t, uint32_t hide_action_id,
                                            uint32_t show_action_id)
{
    return host_ui_set_view_lifecycle(hide_action_id, show_action_id);
}

static int32_t w_host_ui_update_list_item(wasm_exec_env_t exec_env, uint32_t index,
                                          const ui_item_t* item)
{
    if (!item) return HOST_ERR_INVALID_ARG;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_native_addr(inst, const_cast<ui_item_t*>(item), sizeof(ui_item_t)))
        return HOST_ERR_INVALID_ARG;
    ui_item_t native = *item;
    uint32_t off = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(item->label));
    if (off != 0 && wasm_runtime_validate_app_str_addr(inst, off)) {
        native.label = static_cast<const char*>(wasm_runtime_addr_app_to_native(inst, off));
    } else {
        native.label = "";
    }
    return host_ui_update_list_item(static_cast<uint16_t>(index), &native);
}

static int32_t w_host_ui_insert_list_item(wasm_exec_env_t exec_env, uint32_t index,
                                          const ui_item_t* item)
{
    if (!item) return HOST_ERR_INVALID_ARG;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    if (!wasm_runtime_validate_native_addr(inst, const_cast<ui_item_t*>(item), sizeof(ui_item_t)))
        return HOST_ERR_INVALID_ARG;
    ui_item_t native = *item;
    uint32_t off = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(item->label));
    if (off != 0 && wasm_runtime_validate_app_str_addr(inst, off)) {
        native.label = static_cast<const char*>(wasm_runtime_addr_app_to_native(inst, off));
    } else {
        native.label = "";
    }
    return host_ui_insert_list_item(static_cast<uint16_t>(index), &native);
}

static int32_t w_host_ui_remove_list_item(wasm_exec_env_t, uint32_t index)
{
    return host_ui_remove_list_item(static_cast<uint16_t>(index));
}

static int32_t w_host_ui_push_context_menu(wasm_exec_env_t exec_env, const char* title,
                                            const ui_item_t* items, uint32_t count, uint32_t sel)
{
    if (count == 0 || !items) return host_ui_push_context_menu(title, items, 0, sel);
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    constexpr uint32_t kMaxItems = cdc::ui::ContextMenuView::MAX_ITEMS;
    if (count > kMaxItems) count = kMaxItems;
    if (!wasm_runtime_validate_native_addr(inst, const_cast<ui_item_t*>(items),
                                           static_cast<uint64_t>(count) * sizeof(ui_item_t)))
        return HOST_ERR_INVALID_ARG;
    ui_item_t native[kMaxItems];
    for (uint32_t i = 0; i < count; ++i) {
        native[i] = items[i];
        uint32_t off = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(items[i].label));
        if (off != 0 && wasm_runtime_validate_app_str_addr(inst, off)) {
            native[i].label = static_cast<const char*>(wasm_runtime_addr_app_to_native(inst, off));
        } else {
            native[i].label = "";
        }
    }
    return host_ui_push_context_menu(title, native, static_cast<uint16_t>(count), sel);
}

static int32_t w_host_ui_pop(wasm_exec_env_t)            { return host_ui_pop(); }
static int32_t w_host_ui_pop_to_plugin(wasm_exec_env_t)  { return host_ui_pop_to_plugin(); }
static int32_t w_host_ui_repaint(wasm_exec_env_t)        { return host_ui_repaint(); }
static int32_t w_host_ui_wink(wasm_exec_env_t, uint32_t count, uint32_t period_ms)
{ return host_ui_wink(static_cast<uint8_t>(count), static_cast<uint16_t>(period_ms)); }

static int32_t w_host_ui_push_t9_input(wasm_exec_env_t, const char* title, const char* initial,
                                       uint32_t max_len, uint32_t action_id)
{ return host_ui_push_t9_input(title, initial, static_cast<uint16_t>(max_len), action_id); }

static int32_t w_host_ui_push_password(wasm_exec_env_t, const char* title, const char* initial,
                                       uint32_t max_len, uint32_t action_id)
{ return host_ui_push_password(title, initial, static_cast<uint16_t>(max_len), action_id); }

static int32_t w_host_ui_push_pin_entry(wasm_exec_env_t, const char* title, uint32_t max_len,
                                        uint32_t max_attempts, uint32_t action_id)
{ return host_ui_push_pin_entry(title, static_cast<uint8_t>(max_len),
                                static_cast<uint8_t>(max_attempts), action_id); }

static int32_t w_host_ui_push_slider(wasm_exec_env_t, const char* title, int32_t min, int32_t max,
                                     int32_t init, int32_t step, const char* unit, uint32_t action_id)
{ return host_ui_push_slider(title, min, max, init, step, unit, action_id); }

static int32_t w_host_ui_push_color_picker(wasm_exec_env_t, uint32_t r, uint32_t g,
                                            uint32_t b, uint32_t action_id)
{ return host_ui_push_color_picker(static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                                    static_cast<uint8_t>(b), action_id); }

static int32_t w_host_ui_consume_input_text(wasm_exec_env_t, char* out, uint32_t out_size)
{ return host_ui_consume_input_text(out, out_size); }

static int32_t w_host_ui_consume_input_int(wasm_exec_env_t exec_env, int32_t* out)
{
    if (!wbuf_ok(exec_env, out, sizeof(int32_t))) return HOST_ERR_INVALID_ARG;
    return host_ui_consume_input_int(out);
}

// -- Canvas view -----------------------------------------------------------

static int32_t w_host_view_canvas_push(wasm_exec_env_t, const char* title,
                                        uint32_t key_action_id, uint32_t widget_action_id)
{ return host_view_canvas_push(title, key_action_id, widget_action_id); }

static int32_t w_host_view_canvas_get_body_size(wasm_exec_env_t exec_env, uint16_t* w, uint16_t* h)
{
    if (!wbuf_ok(exec_env, w, sizeof(uint16_t)) || !wbuf_ok(exec_env, h, sizeof(uint16_t)))
        return HOST_ERR_INVALID_ARG;
    return host_view_canvas_get_body_size(w, h);
}

static int32_t w_host_view_canvas_set_footer(wasm_exec_env_t, const char* hint)
{ return host_view_canvas_set_footer(hint); }

static int32_t w_host_view_canvas_clear(wasm_exec_env_t)
{ return host_view_canvas_clear(); }

static int32_t w_host_view_canvas_set_text_size(wasm_exec_env_t, uint32_t size)
{ return host_view_canvas_set_text_size(static_cast<uint8_t>(size)); }

static int32_t w_host_view_canvas_set_text_color(wasm_exec_env_t, uint32_t inverted)
{ return host_view_canvas_set_text_color(inverted != 0); }

static int32_t w_host_view_canvas_set_font(wasm_exec_env_t, uint32_t font_id)
{ return host_view_canvas_set_font(static_cast<uint8_t>(font_id)); }

static int32_t w_host_text_pick_font_that_fits(wasm_exec_env_t, const char* text, int32_t max_width_px,
                                               const uint8_t* candidates, uint32_t count,
                                               uint8_t* out_font_id)
{
    return host_text_pick_font_that_fits(text, static_cast<int16_t>(max_width_px),
                                         candidates, count, out_font_id);
}

static int32_t w_host_view_canvas_draw_text(wasm_exec_env_t, int32_t x, int32_t y, const char* text)
{ return host_view_canvas_draw_text(static_cast<int16_t>(x), static_cast<int16_t>(y), text); }

static int32_t w_host_view_canvas_draw_text_aligned(wasm_exec_env_t, int32_t x, int32_t y,
                                                     int32_t w, const char* text, uint32_t align)
{ return host_view_canvas_draw_text_aligned(static_cast<int16_t>(x), static_cast<int16_t>(y),
                                            static_cast<int16_t>(w), text,
                                            static_cast<uint8_t>(align)); }

static int32_t w_host_view_canvas_draw_rect(wasm_exec_env_t, int32_t x, int32_t y,
                                             int32_t w, int32_t h, uint32_t filled)
{ return host_view_canvas_draw_rect(static_cast<int16_t>(x), static_cast<int16_t>(y),
                                    static_cast<int16_t>(w), static_cast<int16_t>(h),
                                    filled != 0); }

static int32_t w_host_view_canvas_invert_rect(wasm_exec_env_t, int32_t x, int32_t y,
                                               int32_t w, int32_t h)
{ return host_view_canvas_invert_rect(static_cast<int16_t>(x), static_cast<int16_t>(y),
                                       static_cast<int16_t>(w), static_cast<int16_t>(h)); }

static int32_t w_host_view_canvas_hline(wasm_exec_env_t, int32_t x, int32_t y, int32_t w)
{ return host_view_canvas_hline(static_cast<int16_t>(x), static_cast<int16_t>(y),
                                 static_cast<int16_t>(w)); }

static int32_t w_host_view_canvas_vline(wasm_exec_env_t, int32_t x, int32_t y, int32_t h)
{ return host_view_canvas_vline(static_cast<int16_t>(x), static_cast<int16_t>(y),
                                 static_cast<int16_t>(h)); }

static int32_t w_host_view_canvas_commit(wasm_exec_env_t, uint32_t full_refresh)
{ return host_view_canvas_commit(full_refresh != 0); }

static int32_t w_host_view_canvas_add_slider(wasm_exec_env_t, uint32_t widget_id,
                                              int32_t min, int32_t max, int32_t initial, int32_t step)
{ return host_view_canvas_add_slider(widget_id, min, max, initial, step); }

static int32_t w_host_view_canvas_add_text(wasm_exec_env_t, uint32_t widget_id,
                                            uint32_t max_len, const char* initial)
{ return host_view_canvas_add_text(widget_id, static_cast<uint16_t>(max_len), initial); }

static int32_t w_host_view_canvas_add_button(wasm_exec_env_t, uint32_t widget_id)
{ return host_view_canvas_add_button(widget_id); }

static int32_t w_host_view_canvas_remove_widget(wasm_exec_env_t, uint32_t widget_id)
{ return host_view_canvas_remove_widget(widget_id); }

static int32_t w_host_view_canvas_set_value(wasm_exec_env_t, uint32_t widget_id, int32_t value)
{ return host_view_canvas_set_value(widget_id, value); }

static int32_t w_host_view_canvas_get_value(wasm_exec_env_t exec_env, uint32_t widget_id, int32_t* out)
{
    if (!wbuf_ok(exec_env, out, sizeof(int32_t))) return HOST_ERR_INVALID_ARG;
    return host_view_canvas_get_value(widget_id, out);
}

static int32_t w_host_view_canvas_set_text(wasm_exec_env_t, uint32_t widget_id, const char* text)
{ return host_view_canvas_set_text(widget_id, text); }

static int32_t w_host_view_canvas_get_text(wasm_exec_env_t, uint32_t widget_id,
                                            char* out, uint32_t cap)
{ return host_view_canvas_get_text(widget_id, out, cap); }

static int32_t w_host_view_canvas_set_focus(wasm_exec_env_t, uint32_t widget_id)
{ return host_view_canvas_set_focus(widget_id); }

static int32_t w_host_view_canvas_get_focus(wasm_exec_env_t exec_env, uint32_t* out)
{
    if (!wbuf_ok(exec_env, out, sizeof(uint32_t))) return HOST_ERR_INVALID_ARG;
    return host_view_canvas_get_focus(out);
}

static int32_t w_host_view_canvas_set_key_repeat(wasm_exec_env_t, uint32_t initial_ms, uint32_t repeat_ms)
{ return host_view_canvas_set_key_repeat(static_cast<uint16_t>(initial_ms),
                                          static_cast<uint16_t>(repeat_ms)); }

static int32_t w_host_view_canvas_set_long_press_action(wasm_exec_env_t, uint32_t action_id)
{ return host_view_canvas_set_long_press_action(action_id); }

// -- I18n -------------------------------------------------------------------

static int32_t w_host_i18n_tr_key (wasm_exec_env_t, const char* k, char* o, uint32_t c) { return host_i18n_tr_key(k, o, c); }
static int32_t w_host_i18n_tr_meta(wasm_exec_env_t, const char* f, char* o, uint32_t c) { return host_i18n_tr_meta(f, o, c); }
static int32_t w_host_i18n_tr_core(wasm_exec_env_t, const char* k, char* o, uint32_t c) { return host_i18n_tr_core(k, o, c); }
static uint32_t w_host_i18n_current_language(wasm_exec_env_t)                           { return host_i18n_current_language(); }

// -- NVS --------------------------------------------------------------------

static int32_t w_host_nvs_get_blob(wasm_exec_env_t, const char* key, uint8_t* buf, uint32_t buf_size)
{
    size_t len = buf_size;
    int rc = host_nvs_get_blob(key, buf, &len);
    return rc == HOST_OK ? static_cast<int32_t>(len) : rc;
}

static int32_t w_host_nvs_set_blob(wasm_exec_env_t, const char* key, const uint8_t* buf, uint32_t len)
{ return host_nvs_set_blob(key, buf, len); }

static int32_t w_host_nvs_get_u32(wasm_exec_env_t exec_env, const char* key, uint32_t* out)
{
    if (!wbuf_ok(exec_env, out, sizeof(uint32_t))) return HOST_ERR_INVALID_ARG;
    return host_nvs_get_u32(key, out);
}

static int32_t w_host_nvs_set_u32(wasm_exec_env_t, const char* key, uint32_t value)
{ return host_nvs_set_u32(key, value); }

static int32_t w_host_nvs_get_str(wasm_exec_env_t, const char* key, char* buf, uint32_t buf_size)
{ return host_nvs_get_str(key, buf, buf_size); }

static int32_t w_host_nvs_set_str(wasm_exec_env_t, const char* key, const char* value)
{ return host_nvs_set_str(key, value); }

static int32_t w_host_nvs_erase(wasm_exec_env_t, const char* key) { return host_nvs_erase(key); }

// -- vFAT (sandboxed plugin file storage) -----------------------------------

static int32_t w_host_fs_write(wasm_exec_env_t, const char* name, const uint8_t* data, uint32_t len)
{ return host_fs_write(name, data, len); }

static int32_t w_host_fs_read(wasm_exec_env_t, const char* name, uint8_t* buf, uint32_t buf_size)
{
    size_t len = buf_size;
    int rc = host_fs_read(name, buf, &len);
    return rc == HOST_OK ? static_cast<int32_t>(len) : rc;
}

static int32_t w_host_fs_remove(wasm_exec_env_t, const char* name) { return host_fs_remove(name); }

static int32_t w_host_fs_size(wasm_exec_env_t, const char* name)
{
    size_t sz = 0;
    int rc = host_fs_size(name, &sz);
    return rc == HOST_OK ? static_cast<int32_t>(sz) : rc;
}

static int32_t w_host_fs_list(wasm_exec_env_t, char* out, uint32_t out_size)
{
    size_t len = out_size;
    int rc = host_fs_list(out, &len);
    return rc == HOST_OK ? static_cast<int32_t>(len) : rc;
}

static int32_t w_host_fs_view(wasm_exec_env_t, const char* name) { return host_fs_view(name); }
static int32_t w_host_fs_view_image(wasm_exec_env_t, const char* name) { return host_fs_view_image(name); }
static int32_t w_host_fs_view_markdown(wasm_exec_env_t, const char* name) { return host_fs_view_markdown(name); }

static int32_t w_host_ui_view_image(wasm_exec_env_t env, const uint8_t* data, uint32_t len)
{ if (!wbuf_ok(env, data, len)) return HOST_ERR_INVALID_ARG; return host_ui_view_image(data, len); }
static int32_t w_host_ui_view_markdown(wasm_exec_env_t env, const uint8_t* data, uint32_t len)
{ if (!wbuf_ok(env, data, len)) return HOST_ERR_INVALID_ARG; return host_ui_view_markdown(data, len); }

// -- Crypto -----------------------------------------------------------------

static int32_t w_host_random(wasm_exec_env_t, uint8_t* buf, uint32_t len)
{ return host_random(buf, len); }

static int32_t w_host_sha256(wasm_exec_env_t exec_env, const uint8_t* data, uint32_t len, uint8_t* out)
{
    if (!wbuf_ok(exec_env, out, 32)) return HOST_ERR_INVALID_ARG;
    return host_sha256(data, len, out);
}

static int32_t w_host_hmac_sha256(wasm_exec_env_t exec_env, const uint8_t* key, uint32_t klen,
                                  const uint8_t* data, uint32_t dlen, uint8_t* out)
{
    if (!wbuf_ok(exec_env, out, 32)) return HOST_ERR_INVALID_ARG;
    return host_hmac_sha256(key, klen, data, dlen, out);
}

static int32_t w_host_base32_encode(wasm_exec_env_t, const uint8_t* in, uint32_t in_len, char* out, uint32_t out_size)
{ return host_base32_encode(in, in_len, out, out_size); }

static int32_t w_host_base32_decode(wasm_exec_env_t, const char* in, uint32_t in_len, uint8_t* out, uint32_t out_size)
{ return host_base32_decode(in, in_len, out, out_size); }

static int32_t w_host_hex_encode(wasm_exec_env_t, const uint8_t* in, uint32_t in_len, char* out, uint32_t out_size)
{ return host_hex_encode(in, in_len, out, out_size); }

// -- HTTP -------------------------------------------------------------------

static int32_t w_host_http_open(wasm_exec_env_t, uint32_t method, const char* url, uint32_t timeout)
{ return host_http_open(static_cast<uint8_t>(method), url, timeout); }

static int32_t w_host_http_set_header(wasm_exec_env_t, int32_t h, const char* k, const char* v)
{ return host_http_set_header(h, k, v); }

static int32_t w_host_http_set_body(wasm_exec_env_t, int32_t h, const uint8_t* body, uint32_t len)
{ return host_http_set_body(h, body, len); }

static int32_t w_host_http_perform(wasm_exec_env_t, int32_t h)  { return host_http_perform(h); }
static int32_t w_host_http_status(wasm_exec_env_t,  int32_t h)  { return host_http_status(h); }
static int32_t w_host_http_close (wasm_exec_env_t, int32_t h)   { return host_http_close(h); }

static int32_t w_host_http_read_chunk(wasm_exec_env_t, int32_t h, uint8_t* buf, uint32_t buf_size)
{
    size_t out_len = 0;
    int rc = host_http_read_chunk(h, buf, buf_size, &out_len);
    return rc == HOST_OK ? static_cast<int32_t>(out_len) : rc;
}

// -- Socket -----------------------------------------------------------------

static int32_t w_host_socket_open(wasm_exec_env_t, uint32_t proto, const char* host,
                                  uint32_t port, uint32_t timeout)
{ return host_socket_open(static_cast<uint8_t>(proto), host,
                          static_cast<uint16_t>(port), timeout); }

static int32_t w_host_socket_write(wasm_exec_env_t, int32_t h, const uint8_t* data,
                                   uint32_t len, uint32_t timeout)
{ return host_socket_write(h, data, len, timeout); }

static int32_t w_host_socket_read(wasm_exec_env_t, int32_t h, uint8_t* out,
                                  uint32_t cap, uint32_t timeout)
{ return host_socket_read(h, out, cap, timeout); }

static int32_t w_host_socket_close(wasm_exec_env_t, int32_t h) { return host_socket_close(h); }

// -- WiFi -------------------------------------------------------------------

static int32_t w_host_wifi_request(wasm_exec_env_t, uint32_t t) { return host_wifi_request(t); }
static int32_t w_host_wifi_release(wasm_exec_env_t)             { return host_wifi_release(); }
static int32_t w_host_wifi_is_connected(wasm_exec_env_t)        { return host_wifi_is_connected(); }
static int32_t w_host_wifi_ssid(wasm_exec_env_t, char* out, uint32_t sz)
{ return host_wifi_ssid(out, sz); }
static int32_t w_host_wifi_ip(wasm_exec_env_t, char* out, uint32_t sz)
{ return host_wifi_ip(out, sz); }

// -- SecureElement ----------------------------------------------------------

static int32_t w_host_rmem_read_named(wasm_exec_env_t, const char* name,
                                      uint8_t* buf, uint32_t buf_size)
{
    size_t len = buf_size;
    int rc = host_rmem_read_named(name, buf, &len);
    return rc == HOST_OK ? static_cast<int32_t>(len) : rc;
}

static int32_t w_host_rmem_write_named(wasm_exec_env_t, const char* name,
                                       const uint8_t* buf, uint32_t len)
{ return host_rmem_write_named(name, buf, len); }

static int32_t w_host_rmem_erase_named(wasm_exec_env_t, const char* name)
{ return host_rmem_erase_named(name); }

static int32_t w_host_rmem_name_used(wasm_exec_env_t, const char* name)
{ return host_rmem_name_used(name) ? 1 : 0; }

static uint32_t w_host_rmem_slot_size(wasm_exec_env_t)
{ return host_rmem_slot_size(); }

static int32_t w_host_ecc_generate(wasm_exec_env_t, const char* name, uint32_t curve)
{ return host_ecc_generate(name, static_cast<uint8_t>(curve)); }
static int32_t w_host_ecc_import(wasm_exec_env_t exec_env, const char* name, const uint8_t* priv, uint32_t curve)
{
    if (!wbuf_ok(exec_env, priv, 32)) return HOST_ERR_INVALID_ARG;
    return host_ecc_import(name, priv, static_cast<uint8_t>(curve));
}
static int32_t w_host_ecc_pubkey(wasm_exec_env_t exec_env, const char* name, uint8_t* pub, uint32_t curve)
{
    if (!wbuf_ok(exec_env, pub, 64)) return HOST_ERR_INVALID_ARG;
    return host_ecc_pubkey(name, pub, static_cast<uint8_t>(curve));
}
static int32_t w_host_ecc_delete(wasm_exec_env_t, const char* name)
{ return host_ecc_delete(name); }
static int32_t w_host_ecc_exists(wasm_exec_env_t, const char* name)
{ return host_ecc_exists(name) ? 1 : 0; }
static int32_t w_host_ecdsa_sign(wasm_exec_env_t exec_env, const char* name, const uint8_t* msg, uint32_t len, uint8_t* sig)
{
    if (!wbuf_ok(exec_env, sig, 64)) return HOST_ERR_INVALID_ARG;
    return host_ecdsa_sign(name, msg, len, sig);
}
static int32_t w_host_eddsa_sign(wasm_exec_env_t exec_env, const char* name, const uint8_t* msg, uint32_t len, uint8_t* sig)
{
    if (!wbuf_ok(exec_env, sig, 64)) return HOST_ERR_INVALID_ARG;
    return host_eddsa_sign(name, msg, len, sig);
}

// -- EventBus ---------------------------------------------------------------

static int32_t w_host_event_subscribe(wasm_exec_env_t, uint32_t mask, uint32_t action_id)
{ return host_event_subscribe(mask, action_id); }
static int32_t w_host_event_unsubscribe(wasm_exec_env_t, uint32_t sub) { return host_event_unsubscribe(sub); }

// -- GPIO -------------------------------------------------------------------

static int32_t w_host_gpio_set_direction(wasm_exec_env_t, uint32_t pin, uint32_t dir)
{ return host_gpio_set_direction(static_cast<uint8_t>(pin), static_cast<uint8_t>(dir)); }
static int32_t w_host_gpio_set_pull(wasm_exec_env_t, uint32_t pin, uint32_t pull)
{ return host_gpio_set_pull(static_cast<uint8_t>(pin), static_cast<uint8_t>(pull)); }
static int32_t w_host_gpio_write(wasm_exec_env_t, uint32_t pin, int32_t level)
{ return host_gpio_write(static_cast<uint8_t>(pin), level != 0); }
static int32_t w_host_gpio_read(wasm_exec_env_t exec_env, uint32_t pin, int32_t* out)
{
    if (!wbuf_ok(exec_env, out, sizeof(int32_t))) return HOST_ERR_INVALID_ARG;
    bool level = false;
    int rc = host_gpio_read(static_cast<uint8_t>(pin), &level);
    if (rc == HOST_OK) *out = level ? 1 : 0;
    return rc;
}
static int32_t w_host_gpio_release(wasm_exec_env_t, uint32_t pin)
{ return host_gpio_release(static_cast<uint8_t>(pin)); }
static int32_t w_host_gpio_pwm_start(wasm_exec_env_t, uint32_t pin, uint32_t freq, uint32_t duty)
{ return host_gpio_pwm_start(static_cast<uint8_t>(pin), freq, static_cast<uint16_t>(duty)); }
static int32_t w_host_gpio_pwm_stop(wasm_exec_env_t, uint32_t pin)
{ return host_gpio_pwm_stop(static_cast<uint8_t>(pin)); }

// -- Date/Time + System info ------------------------------------------------

static int32_t w_host_ui_push_date(wasm_exec_env_t, const char* title, uint32_t d,
                                   uint32_t m, uint32_t y, uint32_t action_id)
{ return host_ui_push_date(title, static_cast<uint8_t>(d), static_cast<uint8_t>(m),
                           static_cast<uint16_t>(y), action_id); }

static int32_t w_host_ui_push_time(wasm_exec_env_t, const char* title, uint32_t h,
                                   uint32_t m, uint32_t action_id)
{ return host_ui_push_time(title, static_cast<uint8_t>(h), static_cast<uint8_t>(m), action_id); }

static int32_t w_host_get_firmware_version(wasm_exec_env_t, char* out, uint32_t out_size)
{ return host_get_firmware_version(out, out_size); }

static int32_t w_host_str_to_display(wasm_exec_env_t, const char* in, char* out,
                                     uint32_t out_size, uint32_t target)
{ return host_str_to_display(in, out, out_size, target); }

static int32_t w_host_str_to_utf8(wasm_exec_env_t, const char* in, char* out, uint32_t out_size)
{ return host_str_to_utf8(in, out, out_size); }

static int32_t w_host_get_build_profile(wasm_exec_env_t, char* out, uint32_t out_size)
{ return host_get_build_profile(out, out_size); }

static int32_t w_host_feature_enabled(wasm_exec_env_t, uint32_t feature_id)
{ return host_feature_enabled(static_cast<uint16_t>(feature_id)) ? 1 : 0; }

static uint32_t w_host_cpu_load(wasm_exec_env_t) { return host_cpu_load(); }

static int32_t w_host_cmd_consume(wasm_exec_env_t, char* out, uint32_t out_size)
{ return host_cmd_consume(out, out_size); }

// -- Message transfer --
static int32_t w_host_msg_register_handler(wasm_exec_env_t, const char* mime, uint32_t aid)
{ return host_msg_register_handler(mime, aid); }
static int32_t w_host_msg_unregister_handler(wasm_exec_env_t, const char* mime)
{ return host_msg_unregister_handler(mime); }
static int32_t w_host_msg_consume(wasm_exec_env_t, uint8_t* buf, uint32_t buf_size,
                                  char* mime_out, uint32_t mime_size)
{ return host_msg_consume(buf, buf_size, mime_out, mime_size); }
static int32_t w_host_msg_send_interactive(wasm_exec_env_t, const char* mime,
                                           const uint8_t* data, uint32_t len, uint32_t flags)
{ return host_msg_send_interactive(mime, data, len, flags); }
static int32_t w_host_msg_send(wasm_exec_env_t exec_env, const uint8_t* addr, uint32_t addr_type,
                               const char* mime, const uint8_t* data, uint32_t len, uint32_t flags)
{
    // addr is a bare '*' (WAMR validates only 1 byte) -> re-validate all 6 bytes.
    if (!wbuf_ok(exec_env, addr, 6)) return HOST_ERR_INVALID_ARG;
    return host_msg_send(addr, static_cast<uint8_t>(addr_type), mime, data, len, flags);
}

static int32_t w_host_ui_acquire_exclusive(wasm_exec_env_t)  { return host_ui_acquire_exclusive(); }
static int32_t w_host_ui_release_exclusive(wasm_exec_env_t)  { return host_ui_release_exclusive(); }
static int32_t w_host_ui_set_inactivity(wasm_exec_env_t, uint32_t timeout_ms, uint32_t action_id)
{ return host_ui_set_inactivity(timeout_ms, action_id); }

// -- Pixel strip ------------------------------------------------------------

static int32_t w_host_pixel_strip_init(wasm_exec_env_t, uint32_t gpio, uint32_t num, uint32_t format)
{ return host_pixel_strip_init(static_cast<uint8_t>(gpio),
                               static_cast<uint16_t>(num),
                               static_cast<uint8_t>(format)); }
static int32_t w_host_pixel_strip_deinit(wasm_exec_env_t)  { return host_pixel_strip_deinit(); }
static int32_t w_host_pixel_strip_set(wasm_exec_env_t, uint32_t idx,
                                      uint32_t r, uint32_t g, uint32_t b)
{ return host_pixel_strip_set(static_cast<uint16_t>(idx),
                              static_cast<uint8_t>(r),
                              static_cast<uint8_t>(g),
                              static_cast<uint8_t>(b)); }
static int32_t w_host_pixel_strip_fill(wasm_exec_env_t, uint32_t r, uint32_t g, uint32_t b)
{ return host_pixel_strip_fill(static_cast<uint8_t>(r),
                               static_cast<uint8_t>(g),
                               static_cast<uint8_t>(b)); }
static int32_t w_host_pixel_strip_clear  (wasm_exec_env_t)  { return host_pixel_strip_clear(); }
static int32_t w_host_pixel_strip_refresh(wasm_exec_env_t)  { return host_pixel_strip_refresh(); }
static uint32_t w_host_pixel_strip_length(wasm_exec_env_t)  { return host_pixel_strip_length(); }
static int32_t w_host_pixel_strip_ready  (wasm_exec_env_t)  { return host_pixel_strip_ready() ? 1 : 0; }

// -- Lockscreen quick-action -----------------------------------------------

static int32_t w_host_lockscreen_register_action(wasm_exec_env_t,
                                                 const char* label_key,
                                                 uint32_t action_id)
{ return host_lockscreen_register_action(label_key, action_id); }

static int32_t w_host_lockscreen_unregister_action(wasm_exec_env_t)
{ return host_lockscreen_unregister_action(); }

static int32_t w_host_lockscreen_alert(wasm_exec_env_t, const char* text,
                                       uint32_t icon, uint32_t action_id)
{ return host_lockscreen_alert(text, static_cast<uint8_t>(icon), action_id); }

// -- Additional crypto ------------------------------------------------------

static int32_t w_host_random_strict(wasm_exec_env_t, uint8_t* buf, uint32_t len)
{ return host_random_strict(buf, len); }
static int32_t w_host_base64_encode(wasm_exec_env_t, const uint8_t* in, uint32_t in_len, char* out, uint32_t out_size)
{ return host_base64_encode(in, in_len, out, out_size); }
static int32_t w_host_base64_decode(wasm_exec_env_t, const char* in, uint32_t in_len, uint8_t* out, uint32_t out_size)
{ return host_base64_decode(in, in_len, out, out_size); }
static int32_t w_host_hex_decode(wasm_exec_env_t, const char* in, uint32_t in_len, uint8_t* out, uint32_t out_size)
{ return host_hex_decode(in, in_len, out, out_size); }
static int32_t w_host_aes_gcm_encrypt(wasm_exec_env_t exec_env, const uint8_t* key, const uint8_t* iv,
                                      const uint8_t* aad, uint32_t aad_len,
                                      const uint8_t* pt, uint32_t pt_len, uint8_t* ct, uint8_t* tag)
{
    if (!wbuf_ok(exec_env, key, 32) || !wbuf_ok(exec_env, iv, 12) ||
        !wbuf_ok(exec_env, ct, pt_len) || !wbuf_ok(exec_env, tag, 16))
        return HOST_ERR_INVALID_ARG;
    return host_aes_gcm_encrypt(key, iv, aad, aad_len, pt, pt_len, ct, tag);
}
static int32_t w_host_aes_gcm_decrypt(wasm_exec_env_t exec_env, const uint8_t* key, const uint8_t* iv,
                                      const uint8_t* aad, uint32_t aad_len,
                                      const uint8_t* ct, uint32_t ct_len, const uint8_t* tag, uint8_t* pt)
{
    if (!wbuf_ok(exec_env, key, 32) || !wbuf_ok(exec_env, iv, 12) ||
        !wbuf_ok(exec_env, tag, 16) || !wbuf_ok(exec_env, pt, ct_len))
        return HOST_ERR_INVALID_ARG;
    return host_aes_gcm_decrypt(key, iv, aad, aad_len, ct, ct_len, tag, pt);
}

// -- Additional time / logging / nvs / sysinfo ------------------------------

static int32_t w_host_local_time(wasm_exec_env_t exec_env, void* out)
{
    if (!wbuf_ok(exec_env, out, sizeof(struct host_tm))) return HOST_ERR_INVALID_ARG;
    return host_local_time(reinterpret_cast<struct host_tm*>(out));
}

static void w_host_log_hex(wasm_exec_env_t, const char* tag, const char* label,
                           const uint8_t* data, uint32_t len)
{ host_log_hex(tag, label, data, len); }

static int32_t w_host_nvs_erase_all(wasm_exec_env_t) { return host_nvs_erase_all(); }
static int32_t w_host_nvs_list_keys(wasm_exec_env_t exec_env, char* out, uint32_t* out_len)
{
    if (!wbuf_ok(exec_env, out_len, sizeof(uint32_t))) return HOST_ERR_INVALID_ARG;
    size_t len = *out_len;
    if (!wbuf_ok(exec_env, out, len)) return HOST_ERR_INVALID_ARG;
    int rc = host_nvs_list_keys(out, &len);
    *out_len = static_cast<uint32_t>(len);
    return rc;
}

// -- Additional wifi --------------------------------------------------------

static int32_t w_host_wifi_mac(wasm_exec_env_t exec_env, uint8_t* out)
{ return wbuf_ok(exec_env, out, 6) ? host_wifi_mac(out) : HOST_ERR_INVALID_ARG; }
static int32_t w_host_wifi_rssi(wasm_exec_env_t)               { return host_wifi_rssi(); }
static int32_t w_host_wifi_start_scan(wasm_exec_env_t)         { return host_wifi_start_scan(); }
static int32_t w_host_wifi_scan_done(wasm_exec_env_t)          { return host_wifi_scan_done() ? 1 : 0; }
static int32_t w_host_wifi_scan_results(wasm_exec_env_t exec_env, void* out, uint32_t* count)
{
    if (!wbuf_ok(exec_env, count, sizeof(uint32_t))) return HOST_ERR_INVALID_ARG;
    size_t c = *count;
    size_t cap = c > 32 ? 32 : c;  // host clamps to IWifiController::MAX_SCAN_RESULTS (32)
    if (!wbuf_ok(exec_env, out, cap * sizeof(wifi_scan_result_t))) return HOST_ERR_INVALID_ARG;
    int rc = host_wifi_scan_results(reinterpret_cast<wifi_scan_result_t*>(out), &c);
    *count = static_cast<uint32_t>(c);
    return rc;
}

// -- Additional gpio / adc / i2c / sao --------------------------------------

static int32_t w_host_gpio_pwm_set_duty(wasm_exec_env_t, uint32_t pin, uint32_t duty)
{ return host_gpio_pwm_set_duty(static_cast<uint8_t>(pin), static_cast<uint16_t>(duty)); }
static int32_t w_host_adc_read(wasm_exec_env_t exec_env, uint32_t pin, uint16_t* raw, uint16_t* mv)
{
    if (!wbuf_ok(exec_env, raw, sizeof(uint16_t)) || !wbuf_ok(exec_env, mv, sizeof(uint16_t)))
        return HOST_ERR_INVALID_ARG;
    return host_adc_read(static_cast<uint8_t>(pin), raw, mv);
}

static int32_t w_host_i2c_write(wasm_exec_env_t, uint32_t bus, uint32_t addr, const uint8_t* data, uint32_t len)
{ return host_i2c_write(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr), data, len); }
static int32_t w_host_i2c_read(wasm_exec_env_t, uint32_t bus, uint32_t addr, uint8_t* data, uint32_t len)
{ return host_i2c_read(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr), data, len); }
static int32_t w_host_i2c_write_read(wasm_exec_env_t, uint32_t bus, uint32_t addr,
                                     const uint8_t* wr, uint32_t wr_len, uint8_t* rd, uint32_t rd_len)
{ return host_i2c_write_read(static_cast<uint8_t>(bus), static_cast<uint8_t>(addr), wr, wr_len, rd, rd_len); }
static int32_t w_host_i2c_scan(wasm_exec_env_t exec_env, uint32_t bus, uint8_t* found, uint32_t* count)
{
    if (!wbuf_ok(exec_env, count, sizeof(uint32_t))) return HOST_ERR_INVALID_ARG;
    size_t c = *count;
    size_t cap = c > 112 ? 112 : c;  // host probes I2C addresses 0x08..0x77 (<=112)
    if (!wbuf_ok(exec_env, found, cap)) return HOST_ERR_INVALID_ARG;
    int rc = host_i2c_scan(static_cast<uint8_t>(bus), found, &c);
    *count = static_cast<uint32_t>(c);
    return rc;
}
static int32_t w_host_sao_eeprom_read(wasm_exec_env_t, uint32_t off, uint8_t* buf, uint32_t len)
{ return host_sao_eeprom_read(static_cast<uint16_t>(off), buf, len); }
static int32_t w_host_sao_eeprom_write(wasm_exec_env_t, uint32_t off, const uint8_t* buf, uint32_t len)
{ return host_sao_eeprom_write(static_cast<uint16_t>(off), buf, len); }

// -- Additional http / event / secure element ------------------------------

static uint32_t w_host_http_content_length(wasm_exec_env_t, int32_t h)
{ return static_cast<uint32_t>(host_http_content_length(h)); }

static int32_t w_host_event_publish(wasm_exec_env_t, uint32_t subtype, uint32_t value)
{ return host_event_publish(subtype, value); }

static int32_t w_host_se_chip_id(wasm_exec_env_t exec_env, uint8_t* serial, uint32_t* len)
{
    if (!wbuf_ok(exec_env, len, sizeof(uint32_t))) return HOST_ERR_INVALID_ARG;
    size_t l = *len;
    size_t cap = l > 16 ? 16 : l;  // getChipId writes at most sizeof(ser_num) = 16 bytes
    if (!wbuf_ok(exec_env, serial, cap)) return HOST_ERR_INVALID_ARG;
    int rc = host_se_chip_id(serial, &l);
    *len = static_cast<uint32_t>(l);
    return rc;
}
static int32_t w_host_se_fw_version(wasm_exec_env_t exec_env, uint8_t* riscv, uint8_t* spect)
{
    if (!wbuf_ok(exec_env, riscv, 4) || !wbuf_ok(exec_env, spect, 4)) return HOST_ERR_INVALID_ARG;
    return host_se_fw_version(riscv, spect);
}

// -- Display (low-level GFX) ------------------------------------------------

static uint32_t w_host_display_width(wasm_exec_env_t)  { return host_display_width(); }
static uint32_t w_host_display_height(wasm_exec_env_t) { return host_display_height(); }
static int32_t  w_host_display_clear(wasm_exec_env_t)  { return host_display_clear(); }
static int32_t  w_host_display_draw_pixel(wasm_exec_env_t, int32_t x, int32_t y, uint32_t color)
{ return host_display_draw_pixel(static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<uint16_t>(color)); }
static int32_t  w_host_display_draw_line(wasm_exec_env_t, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color)
{ return host_display_draw_line(static_cast<int16_t>(x0), static_cast<int16_t>(y0),
                                static_cast<int16_t>(x1), static_cast<int16_t>(y1), static_cast<uint16_t>(color)); }
static int32_t  w_host_display_draw_rect(wasm_exec_env_t, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{ return host_display_draw_rect(static_cast<int16_t>(x), static_cast<int16_t>(y),
                                static_cast<int16_t>(w), static_cast<int16_t>(h), static_cast<uint16_t>(color)); }
static int32_t  w_host_display_fill_rect(wasm_exec_env_t, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{ return host_display_fill_rect(static_cast<int16_t>(x), static_cast<int16_t>(y),
                                static_cast<int16_t>(w), static_cast<int16_t>(h), static_cast<uint16_t>(color)); }
static int32_t  w_host_display_draw_text(wasm_exec_env_t, int32_t x, int32_t y, const char* text, uint32_t size, uint32_t color)
{ return host_display_draw_text(static_cast<int16_t>(x), static_cast<int16_t>(y), text,
                                static_cast<uint8_t>(size), static_cast<uint16_t>(color)); }
static int32_t  w_host_display_flush(wasm_exec_env_t, uint32_t mode) { return host_display_flush(static_cast<uint8_t>(mode)); }
static int32_t  w_host_display_is_busy(wasm_exec_env_t) { return host_display_is_busy() ? 1 : 0; }

// -- Keypad / USB -----------------------------------------------------------

static int32_t w_host_key_pressed(wasm_exec_env_t, uint32_t key) { return host_key_pressed(static_cast<uint8_t>(key)) ? 1 : 0; }
static int32_t w_host_key_consume_next(wasm_exec_env_t, uint8_t* out) { return host_key_consume_next(out); }
static int32_t w_host_usb_cdc_write(wasm_exec_env_t, const uint8_t* data, uint32_t len) { return host_usb_cdc_write(data, len); }

// -- BLE --------------------------------------------------------------------

static int32_t  w_host_ble_is_enabled(wasm_exec_env_t) { return host_ble_is_enabled() ? 1 : 0; }
static int32_t  w_host_ble_mac(wasm_exec_env_t exec_env, uint8_t* out)
{ return wbuf_ok(exec_env, out, 6) ? host_ble_mac(out) : HOST_ERR_INVALID_ARG; }
static int32_t  w_host_ble_device_name(wasm_exec_env_t, char* out, uint32_t size) { return host_ble_device_name(out, size); }
static int32_t  w_host_ble_rssi(wasm_exec_env_t) { return host_ble_rssi(); }

static int32_t  w_host_ble_register_service(wasm_exec_env_t exec_env, void* def, void* chars, uint32_t num)
{
    size_t nchars = num > 6 ? 6 : num;  // host rejects num > MAX_PLUGIN_CHARS (6)
    if (!wbuf_ok(exec_env, def, sizeof(ble_service_def_t)) ||
        !wbuf_ok(exec_env, chars, nchars * sizeof(ble_char_def_t)))
        return HOST_ERR_INVALID_ARG;
    return host_ble_register_service(reinterpret_cast<ble_service_def_t*>(def),
                                     reinterpret_cast<ble_char_def_t*>(chars), num);
}
static int32_t  w_host_ble_unregister_service(wasm_exec_env_t, uint32_t h) { return host_ble_unregister_service(h); }
static int32_t  w_host_ble_send_notification(wasm_exec_env_t, uint32_t ch, const uint8_t* data, uint32_t len)
{ return host_ble_send_notification(ch, data, len); }
static int32_t  w_host_ble_send_indication(wasm_exec_env_t, uint32_t ch, const uint8_t* data, uint32_t len)
{ return host_ble_send_indication(ch, data, len); }
static int32_t  w_host_ble_consume_write(wasm_exec_env_t, uint32_t ch, uint8_t* buf, uint32_t size)
{ return host_ble_consume_write(ch, buf, size); }

static int32_t  w_host_ble_scan_start(wasm_exec_env_t, uint32_t dur) { return host_ble_scan_start(dur); }
static int32_t  w_host_ble_scan_done(wasm_exec_env_t) { return host_ble_scan_done() ? 1 : 0; }
static int32_t  w_host_ble_scan_results(wasm_exec_env_t exec_env, void* out, uint32_t* count)
{
    if (!wbuf_ok(exec_env, count, sizeof(uint32_t))) return HOST_ERR_INVALID_ARG;
    size_t c = *count;
    size_t cap = c > 32 ? 32 : c;  // host clamps to 32 scan results
    if (!wbuf_ok(exec_env, out, cap * sizeof(ble_scan_result_t))) return HOST_ERR_INVALID_ARG;
    int rc = host_ble_scan_results(reinterpret_cast<ble_scan_result_t*>(out), &c);
    *count = static_cast<uint32_t>(c);
    return rc;
}
static int32_t  w_host_ble_connect(wasm_exec_env_t, const uint8_t* addr, uint32_t type)
{ return host_ble_connect(addr, static_cast<uint8_t>(type)); }
static uint32_t w_host_ble_conn_handle(wasm_exec_env_t) { return host_ble_conn_handle(); }
static int32_t  w_host_ble_disconnect(wasm_exec_env_t, uint32_t conn) { return host_ble_disconnect(conn); }
static int32_t  w_host_ble_discover(wasm_exec_env_t, uint32_t conn, const uint8_t* uuid, uint32_t aid)
{ return host_ble_discover(conn, uuid, aid); }
static int32_t  w_host_ble_consume_discovery(wasm_exec_env_t exec_env, void* out, uint32_t* count)
{
    if (!wbuf_ok(exec_env, count, sizeof(uint32_t))) return HOST_ERR_INVALID_ARG;
    size_t c = *count;
    size_t cap = c > 8 ? 8 : c;  // host clamps to MAX_DISC_CHARS (8)
    if (!wbuf_ok(exec_env, out, cap * sizeof(ble_remote_char_t))) return HOST_ERR_INVALID_ARG;
    int rc = host_ble_consume_discovery(reinterpret_cast<ble_remote_char_t*>(out), &c);
    *count = static_cast<uint32_t>(c);
    return rc;
}
static int32_t  w_host_ble_read_char(wasm_exec_env_t, uint32_t conn, uint32_t vh, uint32_t aid)
{ return host_ble_read_char(conn, static_cast<uint16_t>(vh), aid); }
static int32_t  w_host_ble_consume_read(wasm_exec_env_t, uint8_t* buf, uint32_t size)
{ return host_ble_consume_read(buf, size); }
static int32_t  w_host_ble_write_char(wasm_exec_env_t, uint32_t conn, uint32_t vh,
                                      const uint8_t* data, uint32_t len, uint32_t wr)
{ return host_ble_write_char(conn, static_cast<uint16_t>(vh), data, len, static_cast<uint8_t>(wr)); }
static int32_t  w_host_ble_subscribe(wasm_exec_env_t, uint32_t conn, uint32_t cccd, uint32_t aid)
{ return host_ble_subscribe(conn, static_cast<uint16_t>(cccd), aid); }
static int32_t  w_host_ble_consume_notification(wasm_exec_env_t exec_env, uint16_t* vh_out, uint8_t* buf, uint32_t size)
{
    if (!wbuf_ok(exec_env, vh_out, sizeof(uint16_t))) return HOST_ERR_INVALID_ARG;
    return host_ble_consume_notification(vh_out, buf, size);
}

// -- Symbol table -----------------------------------------------------------

static const NativeSymbol s_symbols[] = {
    W("host_log",                w_host_log,                "(i$$)"),
    W("host_uptime_ms",          w_host_uptime_ms,          "()I"),
    W("host_unix_time",          w_host_unix_time,          "()I"),
    W("host_is_time_set",        w_host_is_time_set,        "()i"),
    W("host_timezone_offset",    w_host_timezone_offset,    "()i"),

    W("host_battery_mv",         w_host_battery_mv,         "()i"),
    W("host_battery_pct",        w_host_battery_pct,        "()i"),
    W("host_is_usb_connected",   w_host_is_usb_connected,   "()i"),
    W("host_power_source",       w_host_power_source,       "()i"),
    W("host_charge_status",      w_host_charge_status,      "()i"),
    W("host_is_battery_low",     w_host_is_battery_low,     "()i"),
    W("host_is_battery_critical",w_host_is_battery_critical,"()i"),
    W("host_set_sleep_inhibit",  w_host_set_sleep_inhibit,  "(i)"),

    W("host_ui_push_toast",      w_host_ui_push_toast,      "($ii)i"),
    W("host_ui_push_message",    w_host_ui_push_message,    "($ii)i"),
    W("host_ui_push_info",       w_host_ui_push_info,       "($$)i"),
    W("host_ui_push_confirm",    w_host_ui_push_confirm,    "($ii)i"),
    W("host_browser_open",       w_host_browser_open,       "($)i"),
    W("host_ui_push_list",       w_host_ui_push_list,       "($*~ii)i"),
    W("host_ui_replace_list",    w_host_ui_replace_list,    "($*~ii)i"),
    W("host_ui_set_view_footer", w_host_ui_set_view_footer, "($)i"),
    W("host_ui_update_list_item", w_host_ui_update_list_item, "(i*)i"),
    W("host_ui_insert_list_item", w_host_ui_insert_list_item, "(i*)i"),
    W("host_ui_remove_list_item", w_host_ui_remove_list_item, "(i)i"),
    W("host_ui_set_view_empty",  w_host_ui_set_view_empty,  "($)i"),
    W("host_ui_set_view_lifecycle", w_host_ui_set_view_lifecycle, "(ii)i"),
    W("host_ui_push_context_menu", w_host_ui_push_context_menu, "($*~i)i"),
    W("host_ui_push_t9_input",   w_host_ui_push_t9_input,   "($$ii)i"),
    W("host_ui_push_password",   w_host_ui_push_password,   "($$ii)i"),
    W("host_ui_push_pin_entry",  w_host_ui_push_pin_entry,  "($iii)i"),
    W("host_ui_push_slider",     w_host_ui_push_slider,     "($iiii$i)i"),
    W("host_ui_push_color_picker", w_host_ui_push_color_picker, "(iiii)i"),
    W("host_ui_consume_input_text", w_host_ui_consume_input_text, "(*~)i"),
    W("host_ui_consume_input_int",  w_host_ui_consume_input_int,  "(*)i"),
    W("host_ui_pop",             w_host_ui_pop,             "()i"),
    W("host_ui_pop_to_plugin",   w_host_ui_pop_to_plugin,   "()i"),
    W("host_ui_repaint",         w_host_ui_repaint,         "()i"),
    W("host_ui_wink",            w_host_ui_wink,            "(ii)i"),

    W("host_view_canvas_push",          w_host_view_canvas_push,          "($ii)i"),
    W("host_view_canvas_get_body_size", w_host_view_canvas_get_body_size, "(**)i"),
    W("host_view_canvas_set_footer",    w_host_view_canvas_set_footer,    "($)i"),
    W("host_view_canvas_clear",         w_host_view_canvas_clear,         "()i"),
    W("host_view_canvas_set_text_size", w_host_view_canvas_set_text_size, "(i)i"),
    W("host_view_canvas_set_text_color",w_host_view_canvas_set_text_color,"(i)i"),
    W("host_view_canvas_set_font",      w_host_view_canvas_set_font,      "(i)i"),
    W("host_text_pick_font_that_fits",  w_host_text_pick_font_that_fits,  "($i*~*)i"),
    W("host_view_canvas_draw_text",     w_host_view_canvas_draw_text,     "(ii$)i"),
    W("host_view_canvas_draw_text_aligned", w_host_view_canvas_draw_text_aligned, "(iii$i)i"),
    W("host_view_canvas_draw_rect",     w_host_view_canvas_draw_rect,     "(iiiii)i"),
    W("host_view_canvas_invert_rect",   w_host_view_canvas_invert_rect,   "(iiii)i"),
    W("host_view_canvas_hline",         w_host_view_canvas_hline,         "(iii)i"),
    W("host_view_canvas_vline",         w_host_view_canvas_vline,         "(iii)i"),
    W("host_view_canvas_commit",        w_host_view_canvas_commit,        "(i)i"),
    W("host_view_canvas_add_slider",    w_host_view_canvas_add_slider,    "(iiiii)i"),
    W("host_view_canvas_add_text",      w_host_view_canvas_add_text,      "(ii$)i"),
    W("host_view_canvas_add_button",    w_host_view_canvas_add_button,    "(i)i"),
    W("host_view_canvas_remove_widget", w_host_view_canvas_remove_widget, "(i)i"),
    W("host_view_canvas_set_value",     w_host_view_canvas_set_value,     "(ii)i"),
    W("host_view_canvas_get_value",     w_host_view_canvas_get_value,     "(i*)i"),
    W("host_view_canvas_set_text",      w_host_view_canvas_set_text,      "(i$)i"),
    W("host_view_canvas_get_text",      w_host_view_canvas_get_text,      "(i*~)i"),
    W("host_view_canvas_set_focus",     w_host_view_canvas_set_focus,     "(i)i"),
    W("host_view_canvas_get_focus",     w_host_view_canvas_get_focus,     "(*)i"),
    W("host_view_canvas_set_key_repeat",w_host_view_canvas_set_key_repeat,"(ii)i"),
    W("host_view_canvas_set_long_press_action",w_host_view_canvas_set_long_press_action,"(i)i"),

    W("host_i18n_tr_key",        w_host_i18n_tr_key,        "($*~)i"),
    W("host_i18n_tr_meta",       w_host_i18n_tr_meta,       "($*~)i"),
    W("host_i18n_tr_core",       w_host_i18n_tr_core,       "($*~)i"),
    W("host_i18n_current_language", w_host_i18n_current_language, "()i"),

    W("host_nvs_get_blob",       w_host_nvs_get_blob,       "($*~)i"),
    W("host_nvs_set_blob",       w_host_nvs_set_blob,       "($*~)i"),
    W("host_nvs_get_u32",        w_host_nvs_get_u32,        "($*)i"),
    W("host_nvs_set_u32",        w_host_nvs_set_u32,        "($i)i"),
    W("host_nvs_get_str",        w_host_nvs_get_str,        "($*~)i"),
    W("host_nvs_set_str",        w_host_nvs_set_str,        "($$)i"),
    W("host_nvs_erase",          w_host_nvs_erase,          "($)i"),

    W("host_fs_write",           w_host_fs_write,           "($*~)i"),
    W("host_fs_read",            w_host_fs_read,            "($*~)i"),
    W("host_fs_remove",          w_host_fs_remove,          "($)i"),
    W("host_fs_size",            w_host_fs_size,            "($)i"),
    W("host_fs_list",            w_host_fs_list,            "(*~)i"),
    W("host_fs_view",            w_host_fs_view,            "($)i"),
    W("host_fs_view_image",      w_host_fs_view_image,      "($)i"),
    W("host_fs_view_markdown",   w_host_fs_view_markdown,   "($)i"),
    W("host_ui_view_image",      w_host_ui_view_image,      "(*~)i"),
    W("host_ui_view_markdown",   w_host_ui_view_markdown,   "(*~)i"),

    W("host_random",             w_host_random,             "(*~)i"),
    W("host_sha256",             w_host_sha256,             "(*~*)i"),
    W("host_hmac_sha256",        w_host_hmac_sha256,        "(*~*~*)i"),
    W("host_base32_encode",      w_host_base32_encode,      "(*~*~)i"),
    W("host_base32_decode",      w_host_base32_decode,      "(*~*~)i"),
    W("host_hex_encode",         w_host_hex_encode,         "(*~*~)i"),

    W("host_http_open",          w_host_http_open,          "(i$i)i"),
    W("host_http_set_header",    w_host_http_set_header,    "(i$$)i"),
    W("host_http_set_body",      w_host_http_set_body,      "(i*~)i"),
    W("host_http_perform",       w_host_http_perform,       "(i)i"),
    W("host_http_status",        w_host_http_status,        "(i)i"),
    W("host_http_read_chunk",    w_host_http_read_chunk,    "(i*~)i"),
    W("host_http_close",         w_host_http_close,         "(i)i"),

    W("host_socket_open",        w_host_socket_open,        "(i$ii)i"),
    W("host_socket_write",       w_host_socket_write,       "(i*~i)i"),
    W("host_socket_read",        w_host_socket_read,        "(i*~i)i"),
    W("host_socket_close",       w_host_socket_close,       "(i)i"),

    W("host_wifi_request",       w_host_wifi_request,       "(i)i"),
    W("host_wifi_release",       w_host_wifi_release,       "()i"),
    W("host_wifi_is_connected",  w_host_wifi_is_connected,  "()i"),
    W("host_wifi_ssid",          w_host_wifi_ssid,          "(*~)i"),
    W("host_wifi_ip",            w_host_wifi_ip,            "(*~)i"),

    W("host_rmem_read_named",    w_host_rmem_read_named,    "($*~)i"),
    W("host_rmem_write_named",   w_host_rmem_write_named,   "($*~)i"),
    W("host_rmem_erase_named",   w_host_rmem_erase_named,   "($)i"),
    W("host_rmem_name_used",     w_host_rmem_name_used,     "($)i"),
    W("host_rmem_slot_size",     w_host_rmem_slot_size,     "()i"),
    W("host_ecc_generate",       w_host_ecc_generate,       "($i)i"),
    W("host_ecc_import",         w_host_ecc_import,         "($*i)i"),
    W("host_ecc_pubkey",         w_host_ecc_pubkey,         "($*i)i"),
    W("host_ecc_delete",         w_host_ecc_delete,         "($)i"),
    W("host_ecc_exists",         w_host_ecc_exists,         "($)i"),
    W("host_ecdsa_sign",         w_host_ecdsa_sign,         "($*~*)i"),
    W("host_eddsa_sign",         w_host_eddsa_sign,         "($*~*)i"),

    W("host_event_subscribe",    w_host_event_subscribe,    "(ii)i"),
    W("host_event_unsubscribe",  w_host_event_unsubscribe,  "(i)i"),

    W("host_gpio_set_direction", w_host_gpio_set_direction, "(ii)i"),
    W("host_gpio_set_pull",      w_host_gpio_set_pull,      "(ii)i"),
    W("host_gpio_write",         w_host_gpio_write,         "(ii)i"),
    W("host_gpio_read",          w_host_gpio_read,          "(i*)i"),
    W("host_gpio_release",       w_host_gpio_release,       "(i)i"),
    W("host_gpio_pwm_start",     w_host_gpio_pwm_start,     "(iii)i"),
    W("host_gpio_pwm_stop",      w_host_gpio_pwm_stop,      "(i)i"),

    W("host_ui_push_date",       w_host_ui_push_date,       "($iiii)i"),
    W("host_ui_push_time",       w_host_ui_push_time,       "($iii)i"),

    W("host_get_firmware_version", w_host_get_firmware_version, "(*~)i"),
    W("host_str_to_display",        w_host_str_to_display,        "($*~i)i"),
    W("host_str_to_utf8",           w_host_str_to_utf8,           "($*~)i"),
    W("host_get_build_profile",    w_host_get_build_profile,    "(*~)i"),
    W("host_feature_enabled",      w_host_feature_enabled,      "(i)i"),
    W("host_cpu_load",             w_host_cpu_load,             "()i"),
    W("host_cmd_consume",          w_host_cmd_consume,          "(*~)i"),

    W("host_msg_register_handler",   w_host_msg_register_handler,   "($i)i"),
    W("host_msg_unregister_handler", w_host_msg_unregister_handler, "($)i"),
    W("host_msg_consume",            w_host_msg_consume,            "(*~*~)i"),
    W("host_msg_send_interactive",   w_host_msg_send_interactive,   "($*~i)i"),
    W("host_msg_send",               w_host_msg_send,               "(*i$*~i)i"),

    W("host_ui_acquire_exclusive", w_host_ui_acquire_exclusive, "()i"),
    W("host_ui_release_exclusive", w_host_ui_release_exclusive, "()i"),
    W("host_ui_set_inactivity",    w_host_ui_set_inactivity,    "(ii)i"),

    W("host_pixel_strip_init",    w_host_pixel_strip_init,    "(iii)i"),
    W("host_pixel_strip_deinit",  w_host_pixel_strip_deinit,  "()i"),
    W("host_pixel_strip_set",     w_host_pixel_strip_set,     "(iiii)i"),
    W("host_pixel_strip_fill",    w_host_pixel_strip_fill,    "(iii)i"),
    W("host_pixel_strip_clear",   w_host_pixel_strip_clear,   "()i"),
    W("host_pixel_strip_refresh", w_host_pixel_strip_refresh, "()i"),
    W("host_pixel_strip_length",  w_host_pixel_strip_length,  "()i"),
    W("host_pixel_strip_ready",   w_host_pixel_strip_ready,   "()i"),

    W("host_lockscreen_register_action",   w_host_lockscreen_register_action,   "($i)i"),
    W("host_lockscreen_unregister_action", w_host_lockscreen_unregister_action, "()i"),
    W("host_lockscreen_alert",             w_host_lockscreen_alert,             "($ii)i"),

    W("host_random_strict",      w_host_random_strict,      "(*~)i"),
    W("host_base64_encode",      w_host_base64_encode,      "(*~*~)i"),
    W("host_base64_decode",      w_host_base64_decode,      "(*~*~)i"),
    W("host_hex_decode",         w_host_hex_decode,         "(*~*~)i"),
    W("host_aes_gcm_encrypt",    w_host_aes_gcm_encrypt,    "(***~*~**)i"),
    W("host_aes_gcm_decrypt",    w_host_aes_gcm_decrypt,    "(***~*~**)i"),

    W("host_local_time",         w_host_local_time,         "(*)i"),
    W("host_log_hex",            w_host_log_hex,            "($$*~)"),
    W("host_nvs_erase_all",      w_host_nvs_erase_all,      "()i"),
    W("host_nvs_list_keys",      w_host_nvs_list_keys,      "(**)i"),

    W("host_wifi_mac",           w_host_wifi_mac,           "(*)i"),
    W("host_wifi_rssi",          w_host_wifi_rssi,          "()i"),
    W("host_wifi_start_scan",    w_host_wifi_start_scan,    "()i"),
    W("host_wifi_scan_done",     w_host_wifi_scan_done,     "()i"),
    W("host_wifi_scan_results",  w_host_wifi_scan_results,  "(**)i"),

    W("host_gpio_pwm_set_duty",  w_host_gpio_pwm_set_duty,  "(ii)i"),
    W("host_adc_read",           w_host_adc_read,           "(i**)i"),
    W("host_i2c_write",          w_host_i2c_write,          "(ii*~)i"),
    W("host_i2c_read",           w_host_i2c_read,           "(ii*~)i"),
    W("host_i2c_write_read",     w_host_i2c_write_read,     "(ii*~*~)i"),
    W("host_i2c_scan",           w_host_i2c_scan,           "(i**)i"),
    W("host_sao_eeprom_read",    w_host_sao_eeprom_read,    "(i*~)i"),
    W("host_sao_eeprom_write",   w_host_sao_eeprom_write,   "(i*~)i"),

    W("host_http_content_length",w_host_http_content_length,"(i)i"),
    W("host_event_publish",      w_host_event_publish,      "(ii)i"),
    W("host_se_chip_id",         w_host_se_chip_id,         "(**)i"),
    W("host_se_fw_version",      w_host_se_fw_version,      "(**)i"),

    W("host_display_width",      w_host_display_width,      "()i"),
    W("host_display_height",     w_host_display_height,     "()i"),
    W("host_display_clear",      w_host_display_clear,      "()i"),
    W("host_display_draw_pixel", w_host_display_draw_pixel, "(iii)i"),
    W("host_display_draw_line",  w_host_display_draw_line,  "(iiiii)i"),
    W("host_display_draw_rect",  w_host_display_draw_rect,  "(iiiii)i"),
    W("host_display_fill_rect",  w_host_display_fill_rect,  "(iiiii)i"),
    W("host_display_draw_text",  w_host_display_draw_text,  "(ii$ii)i"),
    W("host_display_flush",      w_host_display_flush,      "(i)i"),
    W("host_display_is_busy",    w_host_display_is_busy,    "()i"),

    W("host_key_pressed",        w_host_key_pressed,        "(i)i"),
    W("host_key_consume_next",   w_host_key_consume_next,   "(*)i"),
    W("host_usb_cdc_write",      w_host_usb_cdc_write,      "(*~)i"),

    W("host_ble_is_enabled",        w_host_ble_is_enabled,        "()i"),
    W("host_ble_mac",               w_host_ble_mac,               "(*)i"),
    W("host_ble_device_name",       w_host_ble_device_name,       "(*~)i"),
    W("host_ble_rssi",              w_host_ble_rssi,              "()i"),
    W("host_ble_register_service",  w_host_ble_register_service,  "(**i)i"),
    W("host_ble_unregister_service",w_host_ble_unregister_service,"(i)i"),
    W("host_ble_send_notification", w_host_ble_send_notification, "(i*~)i"),
    W("host_ble_send_indication",   w_host_ble_send_indication,   "(i*~)i"),
    W("host_ble_consume_write",     w_host_ble_consume_write,     "(i*~)i"),
    W("host_ble_scan_start",        w_host_ble_scan_start,        "(i)i"),
    W("host_ble_scan_done",         w_host_ble_scan_done,         "()i"),
    W("host_ble_scan_results",      w_host_ble_scan_results,      "(**)i"),
    W("host_ble_connect",           w_host_ble_connect,           "(*i)i"),
    W("host_ble_conn_handle",       w_host_ble_conn_handle,       "()i"),
    W("host_ble_disconnect",        w_host_ble_disconnect,        "(i)i"),
    W("host_ble_discover",          w_host_ble_discover,          "(i*i)i"),
    W("host_ble_consume_discovery", w_host_ble_consume_discovery, "(**)i"),
    W("host_ble_read_char",         w_host_ble_read_char,         "(iii)i"),
    W("host_ble_consume_read",      w_host_ble_consume_read,      "(*~)i"),
    W("host_ble_write_char",        w_host_ble_write_char,        "(ii*~i)i"),
    W("host_ble_subscribe",         w_host_ble_subscribe,         "(iii)i"),
    W("host_ble_consume_notification", w_host_ble_consume_notification, "(**~)i"),
};

static cdc::core::PsramUniquePtr<NativeSymbol> s_symbols_ram;

bool register_host_imports()
{
    const uint32_t n = sizeof(s_symbols) / sizeof(s_symbols[0]);
    if (!s_symbols_ram) {
        s_symbols_ram = cdc::core::psramAlloc<NativeSymbol>(n);
        if (!s_symbols_ram) {
            plg_log_error("WAMR: PSRAM alloc for host symbol table failed");
            return false;
        }
        std::memcpy(s_symbols_ram.get(), s_symbols, sizeof(s_symbols));
    }
    if (!wasm_runtime_register_natives("cdc", s_symbols_ram.get(), n)) {
        plg_log_error("WAMR: register_natives(\"cdc\") failed");
        return false;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "WAMR: %u host imports registered", static_cast<unsigned>(n));
    plg_log_info(buf);
    return true;
}

void unregister_host_imports()
{
    if (s_symbols_ram) wasm_runtime_unregister_natives("cdc", s_symbols_ram.get());
}

}  // namespace cdc::plugin_manager
