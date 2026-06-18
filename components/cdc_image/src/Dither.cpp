#include "cdc_image/Dither.h"

#include <cstring>

namespace cdc::image {

Ditherer::Ditherer(uint16_t width, int16_t* scratch)
    : width_(width),
      cur_(scratch + 1),
      next_(scratch + (static_cast<size_t>(width) + 2) + 1) {
    reset();
}

void Ditherer::reset() {
    const size_t row = (static_cast<size_t>(width_) + 2) * sizeof(int16_t);
    std::memset(cur_ - 1, 0, row);
    std::memset(next_ - 1, 0, row);
}

void Ditherer::ditherRow(const uint8_t* grayRow, uint8_t* outPacked) {
    const size_t bytes = (static_cast<size_t>(width_) + 7) / 8;
    std::memset(outPacked, 0, bytes);
    std::memset(next_ - 1, 0, (static_cast<size_t>(width_) + 2) * sizeof(int16_t));

    for (uint16_t x = 0; x < width_; ++x) {
        int v = static_cast<int>(grayRow[x]) + cur_[x];
        if (v < 0) v = 0;
        else if (v > 255) v = 255;

        const bool black = v < 128;
        const int err = v - (black ? 0 : 255);
        if (black) outPacked[x >> 3] |= static_cast<uint8_t>(0x80u >> (x & 7));

        cur_[x + 1]  += static_cast<int16_t>(err * 7 / 16);
        next_[x - 1] += static_cast<int16_t>(err * 3 / 16);
        next_[x]     += static_cast<int16_t>(err * 5 / 16);
        next_[x + 1] += static_cast<int16_t>(err * 1 / 16);
    }

    int16_t* tmp = cur_;
    cur_ = next_;
    next_ = tmp;
}

void ditherImage(const uint8_t* gray, uint16_t w, uint16_t h,
                 uint8_t* outBits, int16_t* scratch) {
    if (!gray || !outBits || !scratch || w == 0) return;
    const size_t stride = (static_cast<size_t>(w) + 7) / 8;
    Ditherer d(w, scratch);
    for (uint16_t y = 0; y < h; ++y) {
        d.ditherRow(gray + static_cast<size_t>(y) * w, outBits + static_cast<size_t>(y) * stride);
    }
}

} // namespace cdc::image
