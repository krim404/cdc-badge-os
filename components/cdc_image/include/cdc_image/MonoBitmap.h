#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::image {

/**
 * \brief Non-owning view over a 1-bpp packed bitmap (MSB-first within each row).
 *
 * A set bit means a black pixel. The underlying buffer is owned elsewhere
 * (PSRAM in firmware); this struct only describes its geometry and offers
 * bounds-checked pixel access.
 */
struct MonoBitmap {
    uint8_t* bits = nullptr;  ///< Packed buffer of `stride * height` bytes.
    uint16_t width = 0;       ///< Width in pixels.
    uint16_t height = 0;      ///< Height in pixels.
    uint16_t stride = 0;      ///< Bytes per row, `(width + 7) / 8`.

    /// \brief Row stride in bytes for a given pixel width.
    static constexpr uint16_t strideFor(uint16_t w) {
        return static_cast<uint16_t>((w + 7) / 8);
    }

    /// \brief Total packed byte size for a `w x h` bitmap.
    static constexpr size_t byteSize(uint16_t w, uint16_t h) {
        return static_cast<size_t>(strideFor(w)) * h;
    }

    /// \brief Sets a pixel; out-of-range coordinates are ignored.
    /// \param x Column.
    /// \param y Row.
    /// \param black True for black, false for white.
    void setPixel(uint16_t x, uint16_t y, bool black) {
        if (!bits || x >= width || y >= height) return;
        uint8_t& cell = bits[static_cast<size_t>(y) * stride + (x >> 3)];
        const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
        if (black) cell |= mask;
        else cell &= static_cast<uint8_t>(~mask);
    }

    /// \brief Reads a pixel.
    /// \param x Column.
    /// \param y Row.
    /// \return True if the pixel is black; false if white or out of range.
    bool getPixel(uint16_t x, uint16_t y) const {
        if (!bits || x >= width || y >= height) return false;
        const uint8_t cell = bits[static_cast<size_t>(y) * stride + (x >> 3)];
        return (cell & (0x80u >> (x & 7))) != 0;
    }
};

} // namespace cdc::image
