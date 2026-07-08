/**
 * \file QrRaster.cpp
 * \brief QR encoding into a packed 1-bpp raster via espressif/qrcode.
 *
 * The qrcode display callback carries no usable user_data round-trip, so a
 * file-static context struct holds the target buffer (the QRCodeView pattern).
 * A static mutex serialises concurrent callers - plugin WASM frames can run
 * on more than one task.
 */

#include "cdc_views/QrRaster.h"
#include "qrcode.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstring>

namespace cdc::ui::qr {

namespace {

constexpr uint8_t QR_VERSION_MAX = 20;

struct RasterCtx {
    bool     sizing = true;
    int      modules = 0;
    uint8_t  scale = 1;
    uint8_t  quiet = 0;
    uint8_t* out = nullptr;
    uint16_t stride = 0;
    uint16_t side_px = 0;
};
RasterCtx s_ctx;

SemaphoreHandle_t s_lock = nullptr;
void lock_init() { if (!s_lock) s_lock = xSemaphoreCreateMutex(); }
struct Guard {
    bool held = false;
    Guard()  { lock_init(); if (s_lock) held = (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE); }
    ~Guard() { if (held)  xSemaphoreGive(s_lock); }
};

void raster_callback(esp_qrcode_handle_t qrcode) {
    int size = esp_qrcode_get_size(qrcode);
    s_ctx.modules = size;
    if (s_ctx.sizing || !s_ctx.out) return;

    for (int my = 0; my < size; my++) {
        for (int mx = 0; mx < size; mx++) {
            if (!esp_qrcode_get_module(qrcode, mx, my)) continue;
            packModule(s_ctx.out, s_ctx.stride, s_ctx.scale, s_ctx.quiet, mx, my);
        }
    }
}

esp_qrcode_config_t make_cfg(uint8_t max_version, uint8_t ecc) {
    esp_qrcode_config_t cfg = {};
    cfg.display_func = raster_callback;
    cfg.max_qrcode_version = (max_version >= 1 && max_version <= QR_VERSION_MAX)
                                 ? max_version : QR_VERSION_MAX;
    switch (ecc) {
        case 1:  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;  break;
        case 2:  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_QUART; break;
        case 3:  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_HIGH; break;
        default: cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;  break;
    }
    return cfg;
}

}  // namespace

int measure(const char* data, uint8_t max_version, uint8_t ecc, uint16_t* out_modules) {
    if (!data || !data[0] || !out_modules) return -2;
    Guard g;
    s_ctx = RasterCtx{};
    s_ctx.sizing = true;
    esp_qrcode_config_t cfg = make_cfg(max_version, ecc);
    if (esp_qrcode_generate(&cfg, data) != ESP_OK || s_ctx.modules <= 0) return -1;
    *out_modules = static_cast<uint16_t>(s_ctx.modules);
    return 0;
}

int render(const char* data, uint8_t max_version, uint8_t ecc,
           uint8_t scale, uint8_t quiet_modules,
           uint8_t* out, size_t out_size,
           uint16_t* out_stride_bytes, uint16_t* out_height_px) {
    if (!data || !data[0] || !out || !out_stride_bytes || !out_height_px) return -2;
    if (scale == 0) scale = 1;

    uint16_t modules = 0;
    int rc = measure(data, max_version, ecc, &modules);
    if (rc != 0) return rc;

    const uint32_t side_px = (static_cast<uint32_t>(modules) + 2u * quiet_modules) * scale;
    if (side_px == 0 || side_px > QR_MAX_SIDE_PX) return -2;
    const uint16_t stride = static_cast<uint16_t>((side_px + 7) / 8);
    const size_t needed = static_cast<size_t>(stride) * side_px;
    if (out_size < needed) return -3;

    Guard g;
    std::memset(out, 0, needed);
    s_ctx = RasterCtx{};
    s_ctx.sizing = false;
    s_ctx.scale = scale;
    s_ctx.quiet = quiet_modules;
    s_ctx.out = out;
    s_ctx.stride = stride;
    s_ctx.side_px = static_cast<uint16_t>(side_px);

    esp_qrcode_config_t cfg = make_cfg(max_version, ecc);
    esp_err_t err = esp_qrcode_generate(&cfg, data);
    s_ctx.out = nullptr;
    if (err != ESP_OK) return -1;

    *out_stride_bytes = stride;
    *out_height_px = static_cast<uint16_t>(side_px);
    return 0;
}

}  // namespace cdc::ui::qr
