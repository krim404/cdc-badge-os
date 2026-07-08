/**
 * \file MonoSurface.cpp
 * \brief Offscreen 1-bpp Adafruit_GFX render target in PSRAM.
 */

#include "cdc_views/MonoSurface.h"

#include "esp_heap_caps.h"

#include <cstring>

namespace cdc::ui {

MonoSurface::MonoSurface(uint16_t w, uint16_t h)
    : Adafruit_GFX(static_cast<int16_t>(w), static_cast<int16_t>(h)),
      stride_(static_cast<uint16_t>((w + 7) / 8)) {
    const size_t bytes = static_cast<size_t>(stride_) * h;
    if (bytes == 0) return;
    bits_ = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (bits_) std::memset(bits_, 0, bytes);
}

MonoSurface::~MonoSurface() {
    if (bits_) heap_caps_free(bits_);
}

void MonoSurface::drawPixel(int16_t x, int16_t y, uint16_t color) {
    if (!bits_ || x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return;
    uint8_t& cell = bits_[static_cast<size_t>(y) * stride_ + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
    if (color) cell |= mask;
    else cell &= static_cast<uint8_t>(~mask);
}

void MonoSurface::clear() {
    if (bits_) std::memset(bits_, 0, byteSize());
}

}  // namespace cdc::ui
