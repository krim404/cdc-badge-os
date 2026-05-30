/**
 * \file host_api_display.cpp
 * \brief Low-level framebuffer drawing for plugins.
 *
 * Opt-in via the manifest capability "display_lowlevel". Forwards to the
 * shared IDisplay GFX surface; call host_display_flush to push pixels to the
 * panel. Without the capability every call returns HOST_ERR_NO_CAPABILITY.
 */

#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "cdc_hal/IDisplay.h"

extern "C" void* plg_get_active_plugin(void);

namespace {

cdc::plugin_manager::Plugin* active() {
    return static_cast<cdc::plugin_manager::Plugin*>(plg_get_active_plugin());
}

bool allowed() {
    auto* p = active();
    return p && p->manifest().capabilities.display_lowlevel;
}

cdc::hal::IDisplay* disp() { return cdc::hal::getDisplayInstance(); }

}  // namespace

extern "C" {

uint16_t host_display_width(void)
{
    auto* d = disp();
    return (allowed() && d) ? d->getWidth() : 0;
}

uint16_t host_display_height(void)
{
    auto* d = disp();
    return (allowed() && d) ? d->getHeight() : 0;
}

int host_display_clear(void)
{
    if (!allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* d = disp();
    if (!d) return HOST_ERR_GENERIC;
    d->clear();
    return HOST_OK;
}

int host_display_draw_pixel(int16_t x, int16_t y, uint16_t color)
{
    if (!allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* d = disp();
    if (!d) return HOST_ERR_GENERIC;
    d->drawPixel(x, y, color);
    return HOST_OK;
}

int host_display_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    if (!allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* d = disp();
    if (!d) return HOST_ERR_GENERIC;
    d->drawLine(x0, y0, x1, y1, color);
    return HOST_OK;
}

int host_display_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (!allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* d = disp();
    if (!d) return HOST_ERR_GENERIC;
    d->drawRect(x, y, w, h, color);
    return HOST_OK;
}

int host_display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (!allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* d = disp();
    if (!d) return HOST_ERR_GENERIC;
    d->fillRect(x, y, w, h, color);
    return HOST_OK;
}

int host_display_draw_text(int16_t x, int16_t y, const char* text, uint8_t size, uint16_t color)
{
    if (!allowed()) return HOST_ERR_NO_CAPABILITY;
    if (!text) return HOST_ERR_INVALID_ARG;
    auto* d = disp();
    if (!d) return HOST_ERR_GENERIC;
    d->setFont(nullptr);
    d->setTextSize(size ? size : 1);
    d->setTextColor(color);
    d->setCursor(x, y);
    d->print(text);
    return HOST_OK;
}

int host_display_flush(uint8_t refresh_mode)
{
    if (!allowed()) return HOST_ERR_NO_CAPABILITY;
    auto* d = disp();
    if (!d) return HOST_ERR_GENERIC;
    d->flush(refresh_mode == 1 ? cdc::hal::RefreshMode::PARTIAL : cdc::hal::RefreshMode::FULL);
    return HOST_OK;
}

bool host_display_is_busy(void)
{
    auto* d = disp();
    return (allowed() && d) ? d->isBusy() : false;
}

}  // extern "C"
