/**
 * \file host_api_image.cpp
 * \brief Image host API: decode PNG/JPEG, scale to a target width and dither
 *        into a packed 1-bpp raster via the cdc_image engine.
 *
 * No capability gate - pure compute on caller-supplied memory. Temporaries
 * live in PSRAM and are bounded by the decoder's 1-megapixel input cap.
 */

#include "plugin_manager/host_api.h"
#include "cdc_core/Raii.h"
#include "cdc_image/Dither.h"
#include "cdc_image/ImageDecoder.h"
#include "cdc_log.h"

#include <cstring>

namespace {

const char* TAG = "PLG_IMG";

constexpr uint32_t MAX_PIXELS = 1048576;  // decoder input cap (~1 MP)
constexpr uint16_t MAX_TARGET_W = 1024;

// Nearest-neighbour upscale for targets wider than the source (the cdc_image
// box filter only downscales).
void upscaleGrayNearest(const uint8_t* src, uint16_t sw, uint16_t sh,
                        uint8_t* dst, uint16_t dw, uint16_t dh) {
    for (uint16_t y = 0; y < dh; y++) {
        const uint8_t* srow = src + static_cast<size_t>(y) * sh / dh * sw;
        uint8_t* drow = dst + static_cast<size_t>(y) * dw;
        for (uint16_t x = 0; x < dw; x++) {
            drow[x] = srow[static_cast<size_t>(x) * sw / dw];
        }
    }
}

}  // namespace

extern "C" {

int host_image_info(const uint8_t* data, size_t len, uint16_t* out_w, uint16_t* out_h) {
    if (!data || len == 0 || !out_w || !out_h) return HOST_ERR_INVALID_ARG;
    uint16_t w = 0, h = 0;
    const char* errorKey = nullptr;
    auto gray = cdc::image::decodeToGray(data, len, MAX_PIXELS, w, h, errorKey);
    if (!gray) {
        LOG_W(TAG, "info decode failed: %s", errorKey ? errorKey : "?");
        return HOST_ERR_GENERIC;
    }
    *out_w = w;
    *out_h = h;
    return HOST_OK;
}

int host_image_render(const uint8_t* data, size_t len, uint16_t target_w,
                      uint8_t* out, size_t out_size,
                      uint16_t* out_stride_bytes, uint16_t* out_height_px) {
    if (!data || len == 0 || !out || !out_stride_bytes || !out_height_px) {
        return HOST_ERR_INVALID_ARG;
    }
    if (target_w == 0 || target_w > MAX_TARGET_W) return HOST_ERR_INVALID_ARG;

    uint16_t w = 0, h = 0;
    const char* errorKey = nullptr;
    auto gray = cdc::image::decodeToGray(data, len, MAX_PIXELS, w, h, errorKey);
    if (!gray) {
        LOG_W(TAG, "render decode failed: %s", errorKey ? errorKey : "?");
        return HOST_ERR_GENERIC;
    }

    uint32_t target_h32 = (static_cast<uint32_t>(h) * target_w + w / 2) / w;
    if (target_h32 == 0) target_h32 = 1;
    if (target_h32 > 4096) return HOST_ERR_INVALID_ARG;  // absurd aspect ratio
    const uint16_t target_h = static_cast<uint16_t>(target_h32);

    const uint16_t stride = static_cast<uint16_t>((target_w + 7) / 8);
    const size_t needed = static_cast<size_t>(stride) * target_h;
    if (out_size < needed) return HOST_ERR_NO_MEMORY;

    const uint8_t* grayForDither = gray.get();
    cdc::core::PsramUniquePtr<uint8_t> scaled;
    if (target_w != w || target_h != h) {
        scaled = cdc::core::psramAlloc<uint8_t>(static_cast<size_t>(target_w) * target_h);
        if (!scaled) return HOST_ERR_NO_MEMORY;
        if (target_w <= w && target_h <= h) {
            cdc::image::downscaleGrayBox(gray.get(), w, h, scaled.get(), target_w, target_h);
        } else {
            upscaleGrayNearest(gray.get(), w, h, scaled.get(), target_w, target_h);
        }
        grayForDither = scaled.get();
    }

    auto scratch = cdc::core::psramAlloc<int16_t>(cdc::image::Ditherer::scratchCells(target_w));
    if (!scratch) return HOST_ERR_NO_MEMORY;
    std::memset(out, 0, needed);
    cdc::image::ditherImage(grayForDither, target_w, target_h, out, scratch.get());

    *out_stride_bytes = stride;
    *out_height_px = target_h;
    return HOST_OK;
}

}  // extern "C"
