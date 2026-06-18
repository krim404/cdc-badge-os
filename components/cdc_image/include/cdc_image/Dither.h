#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::image {

/**
 * \brief Streaming Floyd-Steinberg error-diffusion ditherer (8-bit gray to 1-bit).
 *
 * Processes one row at a time so the caller never holds a full image. The two
 * error rows are caller-owned scratch (\ref scratchCells cells of `int16_t`),
 * keeping this class allocation-free and host-testable.
 */
class Ditherer {
public:
    /// \brief Number of `int16_t` cells the scratch buffer must hold for `width`.
    static constexpr size_t scratchCells(uint16_t width) {
        return (static_cast<size_t>(width) + 2) * 2;
    }

    /**
     * \brief Binds the ditherer to an output width and caller-provided scratch.
     * \param width Pixels per row.
     * \param scratch Buffer of at least `scratchCells(width)` `int16_t` cells.
     */
    Ditherer(uint16_t width, int16_t* scratch);

    /// \brief Clears accumulated error. Call once before each image.
    void reset();

    /**
     * \brief Dithers one row of `width` 8-bit gray pixels into a packed 1-bpp row.
     * \param grayRow `width` bytes, 0 = black .. 255 = white.
     * \param outPacked `(width + 7) / 8` bytes, MSB-first; a set bit is black.
     */
    void ditherRow(const uint8_t* grayRow, uint8_t* outPacked);

private:
    uint16_t width_;
    int16_t* cur_;   ///< Current-row error (carries +/-1 border cells).
    int16_t* next_;  ///< Next-row error.
};

/**
 * \brief Dithers a full 8-bit grayscale image into a packed 1-bpp buffer.
 * \param gray `w * h` bytes, row-major, 0 = black .. 255 = white.
 * \param w Width in pixels.
 * \param h Height in pixels.
 * \param outBits Packed output, stride `(w + 7) / 8` bytes per row, MSB-first.
 * \param scratch Error scratch of at least `Ditherer::scratchCells(w)` cells.
 */
void ditherImage(const uint8_t* gray, uint16_t w, uint16_t h,
                 uint8_t* outBits, int16_t* scratch);

} // namespace cdc::image
