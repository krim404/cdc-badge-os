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
    auto* c = canvas();
    if (!c) return HOST_ERR_NOT_FOUND;
    c->clearBody();
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
