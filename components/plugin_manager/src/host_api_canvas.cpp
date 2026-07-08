/**
 * \file host_api_canvas.cpp
 * \brief extern-C adapter for the plugin canvas view.
 *
 * Push a CanvasView, then forward draw/widget/focus calls to it. All state
 * lives in PluginUiState / CanvasView; this TU only marshals C arguments.
 */

#include "plugin_manager/PluginUiState.h"
#include "plugin_manager/host_api.h"
#include "cdc_views/CanvasView.h"
#include "cdc_views/Fonts.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_hal/IDisplay.h"
#include "host_str_conv.h"
#include <goodisplay/gdey029T94.h>
#include <cstring>
#include <string>

namespace {

cdc::ui::CanvasView* canvas()
{
    return cdc::plugin_manager::PluginUiState::instance().canvasView();
}

}  // namespace

extern "C" {

int host_view_canvas_push(const char* title, uint32_t key_action_id,
                          uint32_t widget_action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .pushCanvas(title, key_action_id, widget_action_id);
}

int host_view_canvas_get_body_size(uint16_t* w, uint16_t* h)
{
    auto* c = canvas();
    if (!c || !w || !h) return HOST_ERR_NOT_FOUND;
    c->getBodySize(w, h);
    return HOST_OK;
}

int host_view_canvas_set_footer(const char* hint)
{
    return cdc::plugin_manager::PluginUiState::instance().setViewFooter(hint);
}

int host_view_canvas_clear(void)
{
    return host_view_canvas_clear_ex(0);
}

int host_view_canvas_clear_ex(uint32_t flags)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    if (flags & ~static_cast<uint32_t>(HOST_CANVAS_CLEAR_KEEP_SPRITES)) {
        return HOST_ERR_INVALID_ARG;
    }
    c->clearBody((flags & HOST_CANVAS_CLEAR_KEEP_SPRITES) != 0);
    return HOST_OK;
}

int host_view_canvas_set_text_size(uint8_t size)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->setTextSize(size);
    return HOST_OK;
}

int host_view_canvas_set_text_color(bool inverted)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->setTextInverted(inverted);
    return HOST_OK;
}

int host_view_canvas_set_shade(uint8_t shade)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->setShade(shade);
    return HOST_OK;
}

int host_view_canvas_set_font(uint8_t font_id)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    if (font_id >= HOST_FONT_COUNT) return HOST_ERR_INVALID_ARG;
    c->setFontId(font_id);
    return HOST_OK;
}

int host_text_pick_font_that_fits(const char* text, int16_t max_width_px,
                                  const uint8_t* candidates, uint32_t count,
                                  uint8_t* out_font_id)
{
    if (!text || !candidates || !out_font_id || count == 0) {
        return HOST_ERR_INVALID_ARG;
    }
    auto* display = cdc::hal::getDisplayInstance();
    if (!display) return HOST_ERR_NOT_FOUND;
    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return HOST_ERR_NOT_FOUND;

    std::string cp = cdc::plugin_manager::toDisplay(text);
    const GFXfont* candFonts[HOST_FONT_COUNT];
    if (count > HOST_FONT_COUNT) count = HOST_FONT_COUNT;
    for (uint32_t i = 0; i < count; ++i) {
        candFonts[i] = cdc::ui::getGfxFont(candidates[i]);
    }
    const GFXfont* picked = cdc::ui::render::pickFontThatFits(
        gfx, cp.c_str(), max_width_px, candFonts, count, false);

    uint8_t pickedId = candidates[count - 1];
    for (uint32_t i = 0; i < count; ++i) {
        if (candFonts[i] == picked) {
            pickedId = candidates[i];
            break;
        }
    }
    *out_font_id = pickedId;
    return HOST_OK;
}

int host_view_canvas_draw_text(int16_t x, int16_t y, const char* text)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->drawText(x, y, cdc::plugin_manager::toDisplay(text).c_str());
    return HOST_OK;
}

int host_view_canvas_draw_text_aligned(int16_t x, int16_t y, int16_t w,
                                       const char* text, uint8_t align)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->drawTextAligned(x, y, w, cdc::plugin_manager::toDisplay(text).c_str(), align);
    return HOST_OK;
}

int host_view_canvas_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, bool filled)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->drawRect(x, y, w, h, filled);
    return HOST_OK;
}

int host_view_canvas_draw_pixel(int16_t x, int16_t y)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->drawPixel(x, y);
    return HOST_OK;
}

int host_view_canvas_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->drawLine(x0, y0, x1, y1);
    return HOST_OK;
}

int host_view_canvas_draw_circle(int16_t x, int16_t y, int16_t r, bool filled)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->drawCircle(x, y, r, filled);
    return HOST_OK;
}

int host_view_canvas_draw_triangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                   int16_t x2, int16_t y2, bool filled)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->drawTriangle(x0, y0, x1, y1, x2, y2, filled);
    return HOST_OK;
}

int host_view_canvas_draw_round_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                                     int16_t r, bool filled)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->drawRoundRect(x, y, w, h, r, filled);
    return HOST_OK;
}

int host_view_canvas_draw_bitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                                 const uint8_t* data, uint32_t len)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    if (!data) return HOST_ERR_INVALID_ARG;
    c->drawBitmap(x, y, w, h, data, len);
    return HOST_OK;
}

int host_view_canvas_hline(int16_t x, int16_t y, int16_t w)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->drawHLine(x, y, w);
    return HOST_OK;
}

int host_view_canvas_vline(int16_t x, int16_t y, int16_t h)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->drawVLine(x, y, h);
    return HOST_OK;
}

int host_view_canvas_commit(bool full_refresh)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->commit(full_refresh);
    return HOST_OK;
}

int host_view_canvas_elem_begin(uint32_t elem_id)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    if (elem_id == 0) return HOST_ERR_INVALID_ARG;
    return c->beginElem(elem_id) ? HOST_OK : HOST_ERR_NO_MEMORY;
}

int host_view_canvas_elem_end(void)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->endElem();
    return HOST_OK;
}

int host_view_canvas_elem_set_offset(uint32_t elem_id, int16_t ox, int16_t oy)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->elemSetOffset(elem_id, ox, oy) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_elem_move(uint32_t elem_id, int16_t dx, int16_t dy)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->elemMove(elem_id, dx, dy) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_elem_show(uint32_t elem_id, bool visible)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->elemShow(elem_id, visible) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_elem_remove(uint32_t elem_id)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->elemRemove(elem_id) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_elem_clear(uint32_t elem_id)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->elemClear(elem_id) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_elem_set_z(uint32_t elem_id, int8_t z)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->elemSetZ(elem_id, z) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_elem_get_offset(uint32_t elem_id, int16_t* ox, int16_t* oy)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->elemGetOffset(elem_id, ox, oy) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_elem_get_bounds(uint32_t elem_id, int16_t* x, int16_t* y,
                                     uint16_t* w, uint16_t* h)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->elemGetBounds(elem_id, x, y, w, h) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_set_anim_policy(uint8_t refresh_policy, uint8_t max_fps)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->setAnimPolicy(refresh_policy, max_fps) ? HOST_OK : HOST_ERR_INVALID_ARG;
}

int host_view_canvas_draw_sprite(int16_t x, int16_t y, uint32_t sprite)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    cdc::ui::anim::SpriteStore::FrameView fv;
    if (!c->spriteFrameView(sprite, &fv)) return HOST_ERR_NOT_FOUND;
    c->drawSprite(x, y, sprite);
    return HOST_OK;
}

int host_view_canvas_set_ink(bool white)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->setInkWhite(white);
    return HOST_OK;
}

int host_view_canvas_marquee(int16_t x, int16_t y, int16_t window_w,
                             const char* text, uint16_t step_px, uint16_t frame_ms)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    if (!text) return HOST_ERR_INVALID_ARG;
    int32_t handle = c->marquee(x, y, window_w,
                                cdc::plugin_manager::toDisplay(text).c_str(),
                                step_px, frame_ms);
    if (handle > 0) return static_cast<int>(handle);
    return handle == 0 ? HOST_ERR_INVALID_ARG : HOST_ERR_NO_MEMORY;
}

// --- Sprites -----------------------------------------------------------------

int host_sprite_create(uint16_t frame_w, uint16_t frame_h, uint16_t frame_count,
                       const uint8_t* frames, uint32_t len)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    int32_t handle = c->spriteCreate(frame_w, frame_h, frame_count, frames, len);
    if (handle > 0) return static_cast<int>(handle);
    return handle == 0 ? HOST_ERR_INVALID_ARG : HOST_ERR_NO_MEMORY;
}

int host_sprite_set_mask(uint32_t sprite, const uint8_t* mask, uint32_t len)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    if (!mask) return HOST_ERR_INVALID_ARG;
    return c->spriteSetMask(sprite, mask, len) ? HOST_OK : HOST_ERR_INVALID_ARG;
}

int host_sprite_set_flags(uint32_t sprite, uint8_t flags)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->spriteSetFlags(sprite, flags) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_sprite_set_scale(uint32_t sprite, uint8_t scale)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    if (scale == 0 || scale > 4) return HOST_ERR_INVALID_ARG;
    return c->spriteSetScale(sprite, scale) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_sprite_create_from_image(const uint8_t* data, uint32_t len,
                                  uint16_t target_w, uint16_t frame_h)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    if (!data || len == 0 || target_w == 0 || frame_h == 0) {
        return HOST_ERR_INVALID_ARG;
    }

    // Decode + dither through the image pipeline into a temporary strip,
    // then hand the vertically stacked frames to the sprite store.
    const uint16_t stride = static_cast<uint16_t>((target_w + 7) / 8);
    const size_t   cap    = 65536;  // sprite arena upper bound
    auto strip = cdc::core::psramAlloc<uint8_t>(cap);
    if (!strip) return HOST_ERR_NO_MEMORY;
    uint16_t out_stride = 0, out_h = 0;
    int rc = host_image_render(data, len, target_w, strip.get(), cap,
                               &out_stride, &out_h);
    if (rc != HOST_OK) return rc;

    uint16_t frames = out_h / frame_h;  // partial last frame is dropped
    if (frames == 0) return HOST_ERR_INVALID_ARG;
    int32_t handle = c->spriteCreate(target_w, frame_h, frames, strip.get(),
                                     static_cast<uint32_t>(stride) * frame_h * frames);
    if (handle > 0) return static_cast<int>(handle);
    return handle == 0 ? HOST_ERR_INVALID_ARG : HOST_ERR_NO_MEMORY;
}

int host_sprite_set_frame(uint32_t sprite, uint16_t frame)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->spriteSetFrame(sprite, frame) ? HOST_OK : HOST_ERR_INVALID_ARG;
}

int host_sprite_get_frame(uint32_t sprite, uint16_t* out)
{
    auto* c = canvas();
    if (!c || !out) return HOST_ERR_NOT_FOUND;
    int32_t f = c->spriteGetFrame(sprite);
    if (f < 0) return HOST_ERR_NOT_FOUND;
    *out = static_cast<uint16_t>(f);
    return HOST_OK;
}

int host_sprite_set_frame_durations(uint32_t sprite, const uint16_t* ms,
                                    uint16_t count)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->spriteSetFrameDurations(sprite, ms, count) ? HOST_OK
                                                         : HOST_ERR_INVALID_ARG;
}

int host_sprite_play(uint32_t sprite, uint8_t mode, uint16_t frame_ms,
                     uint16_t repeat, uint32_t done_action_id)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->spritePlay(sprite, mode, frame_ms, repeat, done_action_id)
               ? HOST_OK : HOST_ERR_INVALID_ARG;
}

int host_sprite_stop(uint32_t sprite)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->spriteStop(sprite) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_sprite_destroy(uint32_t sprite)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->spriteDestroy(sprite) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

// --- Tweens ------------------------------------------------------------------

int host_anim_start(const host_anim_t* cfg)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    if (!cfg) return HOST_ERR_INVALID_ARG;

    cdc::ui::anim::TweenConfig tc;
    tc.elemId       = cfg->elem_id;
    tc.fromX        = cfg->from_x;
    tc.fromY        = cfg->from_y;
    tc.toX          = cfg->to_x;
    tc.toY          = cfg->to_y;
    tc.durationMs   = cfg->duration_ms;
    tc.delayMs      = cfg->delay_ms;
    tc.repeat       = cfg->repeat;
    tc.easing       = cfg->easing;
    tc.flags        = cfg->flags;
    tc.doneActionId = cfg->done_action_id;
    tc.startAfter   = cfg->start_after;

    uint32_t handle = c->animStart(tc);
    if (handle != 0) return static_cast<int>(handle);
    // Disambiguate: unknown element vs bad parameters vs full slots.
    if (!c->elemGetOffset(cfg->elem_id, nullptr, nullptr)) return HOST_ERR_NOT_FOUND;
    if (cfg->duration_ms == 0 || cfg->duration_ms > 60000
        || cfg->delay_ms > 60000 || cfg->easing > HOST_EASE_STEP) {
        return HOST_ERR_INVALID_ARG;
    }
    return HOST_ERR_NO_MEMORY;
}

int host_anim_cancel(uint32_t handle)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->animCancel(handle) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_anim_pause(uint32_t handle, bool paused)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->animPause(handle, paused) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_anim_state(uint32_t handle)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    int8_t s = c->animState(handle);
    return s < 0 ? HOST_ERR_NOT_FOUND : s;
}

int host_anim_active_count(void)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->animActiveCount();
}

int host_anim_blink(uint32_t elem_id, uint16_t period_ms, uint16_t count,
                    uint32_t done_action_id)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    uint32_t handle = c->animBlink(elem_id, period_ms, count, done_action_id);
    if (handle != 0) return static_cast<int>(handle);
    if (!c->elemGetOffset(elem_id, nullptr, nullptr)) return HOST_ERR_NOT_FOUND;
    if (period_ms == 0 || count == 0) return HOST_ERR_INVALID_ARG;
    return HOST_ERR_NO_MEMORY;
}

int host_view_canvas_add_slider(uint32_t widget_id, int32_t min, int32_t max,
                                int32_t initial, int32_t step)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->addSlider(widget_id, min, max, initial, step) ? HOST_OK : HOST_ERR_INVALID_ARG;
}

int host_view_canvas_add_text(uint32_t widget_id, uint16_t max_len, const char* initial)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    std::string cp = cdc::plugin_manager::toDisplay(initial);
    return c->addText(widget_id, max_len, initial ? cp.c_str() : nullptr)
               ? HOST_OK : HOST_ERR_INVALID_ARG;
}

int host_view_canvas_add_button(uint32_t widget_id)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->addButton(widget_id) ? HOST_OK : HOST_ERR_INVALID_ARG;
}

int host_view_canvas_remove_widget(uint32_t widget_id)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->removeWidget(widget_id) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_set_value(uint32_t widget_id, int32_t value)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->setValue(widget_id, value) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_get_value(uint32_t widget_id, int32_t* out)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->getValue(widget_id, out) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_set_text(uint32_t widget_id, const char* text)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->setText(widget_id, cdc::plugin_manager::toDisplay(text).c_str())
               ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_get_text(uint32_t widget_id, char* out, size_t cap)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    if (!out || cap == 0) return HOST_ERR_INVALID_ARG;
    std::string tmp;
    tmp.resize(cap);
    int n = c->getText(widget_id, &tmp[0], cap);
    if (n < 0) return HOST_ERR_NOT_FOUND;
    tmp.resize(std::strlen(tmp.c_str()));
    return cdc::plugin_manager::copyUtf8(tmp.c_str(), out, cap);
}

int host_view_canvas_set_focus(uint32_t widget_id)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    return c->setFocus(widget_id) ? HOST_OK : HOST_ERR_NOT_FOUND;
}

int host_view_canvas_get_focus(uint32_t* out)
{
    auto* c = canvas();
    if (!c || !out) return HOST_ERR_NOT_FOUND;
    *out = c->getFocus();
    return HOST_OK;
}

int host_view_canvas_set_key_repeat(uint16_t initial_ms, uint16_t repeat_ms)
{
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->setKeyRepeat(initial_ms, repeat_ms);
    return HOST_OK;
}

int host_view_canvas_set_long_press_action(uint32_t action_id)
{
    return cdc::plugin_manager::PluginUiState::instance()
        .setCanvasLongPressAction(action_id);
}

}  // extern "C"
