/**
 * \file host_api_pixel_strip.cpp
 * \brief Addressable pixel strip host API (WS2811/WS2812/WS2813/SK6812).
 *
 * Thin wrapper on top of ESP-IDF `espressif/led_strip` v2.5.x. One global
 * strip handle is shared between plugins; (gpio_pin, num_pixels, format)
 * identifies the configured strip. Manifest must declare
 * `capabilities.pixel_strip = true`.
 */

#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginGpioPolicy.h"

#include "led_strip.h"
#include "led_strip_rmt.h"
#include "led_strip_types.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstring>

extern "C" void* plg_get_active_plugin(void);

namespace {

constexpr uint16_t MAX_PIXELS = 1024;

led_strip_handle_t s_handle = nullptr;
uint8_t            s_gpio   = 0xFF;
uint16_t           s_count  = 0;
uint8_t            s_format = 0xFF;

SemaphoreHandle_t mutex()
{
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

bool manifest_allows()
{
    auto* p = static_cast<cdc::plugin_manager::Plugin*>(plg_get_active_plugin());
    if (!p) return false;
    return p->manifest().capabilities.pixel_strip;
}

led_pixel_format_t map_format(uint8_t fmt, bool& has_white)
{
    has_white = false;
    switch (fmt) {
        case PIXEL_FORMAT_GRB:  return LED_PIXEL_FORMAT_GRB;
        case PIXEL_FORMAT_RGB:  return LED_PIXEL_FORMAT_GRB;  // driver handles ordering at refresh
        case PIXEL_FORMAT_GRBW:
        case PIXEL_FORMAT_RGBW: has_white = true; return LED_PIXEL_FORMAT_GRBW;
        default:                return LED_PIXEL_FORMAT_INVALID;
    }
}

int create_strip(uint8_t gpio, uint16_t count, uint8_t format)
{
    bool has_white = false;
    led_pixel_format_t pf = map_format(format, has_white);
    if (pf == LED_PIXEL_FORMAT_INVALID) return HOST_ERR_INVALID_ARG;

    led_strip_config_t cfg{};
    cfg.strip_gpio_num   = gpio;
    cfg.max_leds         = count;
    cfg.led_pixel_format = pf;
    cfg.led_model        = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt{};
    rmt.clk_src        = RMT_CLK_SRC_DEFAULT;
    rmt.resolution_hz  = 10 * 1000 * 1000;
    rmt.mem_block_symbols = 64;
    rmt.flags.with_dma = 0;

    led_strip_handle_t h = nullptr;
    if (led_strip_new_rmt_device(&cfg, &rmt, &h) != ESP_OK) return HOST_ERR_GENERIC;
    s_handle = h;
    s_gpio   = gpio;
    s_count  = count;
    s_format = format;
    return HOST_OK;
}

void destroy_strip()
{
    if (s_handle) {
        led_strip_clear(s_handle);
        led_strip_del(s_handle);
        s_handle = nullptr;
    }
    s_count  = 0;
    s_gpio   = 0xFF;
    s_format = 0xFF;
}

struct Lock {
    SemaphoreHandle_t m;
    bool ok;
    explicit Lock(SemaphoreHandle_t s) : m(s), ok(xSemaphoreTake(s, pdMS_TO_TICKS(100)) == pdTRUE) {}
    ~Lock() { if (ok) xSemaphoreGive(m); }
};

}  // namespace

extern "C" {

int host_pixel_strip_init(uint8_t gpio_pin, uint16_t num_pixels, uint8_t format)
{
    if (!manifest_allows())            return HOST_ERR_NO_CAPABILITY;
    if (!cdc::plugin_manager::gpio_policy::isAllowed(gpio_pin))
                                       return HOST_ERR_NO_CAPABILITY;
    if (num_pixels == 0)               return HOST_ERR_INVALID_ARG;
    if (num_pixels > MAX_PIXELS)       return HOST_ERR_INVALID_ARG;

    Lock l(mutex());
    if (!l.ok) return HOST_ERR_BUSY;

    if (s_handle && s_gpio == gpio_pin && s_count == num_pixels && s_format == format) {
        return HOST_OK;  // idempotent re-init with identical params
    }
    destroy_strip();
    return create_strip(gpio_pin, num_pixels, format);
}

int host_pixel_strip_deinit(void)
{
    if (!manifest_allows()) return HOST_ERR_NO_CAPABILITY;
    Lock l(mutex());
    if (!l.ok) return HOST_ERR_BUSY;
    destroy_strip();
    return HOST_OK;
}

int host_pixel_strip_set(uint16_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!manifest_allows()) return HOST_ERR_NO_CAPABILITY;
    Lock l(mutex());
    if (!l.ok)               return HOST_ERR_BUSY;
    if (!s_handle)           return HOST_ERR_GENERIC;
    if (index >= s_count)    return HOST_ERR_INVALID_ARG;
    return led_strip_set_pixel(s_handle, index, r, g, b) == ESP_OK ? HOST_OK : HOST_ERR_GENERIC;
}

int host_pixel_strip_fill(uint8_t r, uint8_t g, uint8_t b)
{
    if (!manifest_allows()) return HOST_ERR_NO_CAPABILITY;
    Lock l(mutex());
    if (!l.ok)     return HOST_ERR_BUSY;
    if (!s_handle) return HOST_ERR_GENERIC;
    for (uint16_t i = 0; i < s_count; ++i) {
        if (led_strip_set_pixel(s_handle, i, r, g, b) != ESP_OK) return HOST_ERR_GENERIC;
    }
    return HOST_OK;
}

int host_pixel_strip_clear(void)
{
    if (!manifest_allows()) return HOST_ERR_NO_CAPABILITY;
    Lock l(mutex());
    if (!l.ok)     return HOST_ERR_BUSY;
    if (!s_handle) return HOST_ERR_GENERIC;
    return led_strip_clear(s_handle) == ESP_OK ? HOST_OK : HOST_ERR_GENERIC;
}

int host_pixel_strip_refresh(void)
{
    if (!manifest_allows()) return HOST_ERR_NO_CAPABILITY;
    Lock l(mutex());
    if (!l.ok)     return HOST_ERR_BUSY;
    if (!s_handle) return HOST_ERR_GENERIC;
    return led_strip_refresh(s_handle) == ESP_OK ? HOST_OK : HOST_ERR_GENERIC;
}

uint16_t host_pixel_strip_length(void)
{
    Lock l(mutex());
    if (!l.ok) return 0;
    return s_count;
}

bool host_pixel_strip_ready(void)
{
    Lock l(mutex());
    if (!l.ok) return false;
    return s_handle != nullptr;
}

}  // extern "C"
