/**
 * \file MonoSurface.h
 * \brief Offscreen 1-bpp Adafruit_GFX render target in PSRAM.
 *
 * Packed rows, MSB-first, a set bit is black (color != 0 draws black) - the
 * shared bitmap layout of MonoBitmap, the QR raster and the thermo_print job
 * format. Unlike GFXcanvas1 the buffer lives in PSRAM, where surfaces like a
 * 384-px-wide print band belong.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <Adafruit_GFX.h>

namespace cdc::ui {

class MonoSurface : public Adafruit_GFX {
public:
    /**
     * \brief Allocates a `w x h` 1-bpp surface in PSRAM, cleared to white.
     * \param w Width in pixels.
     * \param h Height in pixels.
     */
    MonoSurface(uint16_t w, uint16_t h);
    ~MonoSurface() override;

    MonoSurface(const MonoSurface&) = delete;
    MonoSurface& operator=(const MonoSurface&) = delete;

    /// \brief True when the buffer allocation succeeded.
    [[nodiscard]] bool ok() const { return bits_ != nullptr; }

    void drawPixel(int16_t x, int16_t y, uint16_t color) override;

    /// \brief Clears the surface to white.
    void clear();

    /// \brief Packed pixel rows, `strideBytes() * height()` bytes.
    [[nodiscard]] const uint8_t* buffer() const { return bits_; }

    /// \brief Bytes per packed row, `(width + 7) / 8`.
    [[nodiscard]] uint16_t strideBytes() const { return stride_; }

    /// \brief Total packed byte size.
    [[nodiscard]] size_t byteSize() const { return static_cast<size_t>(stride_) * HEIGHT; }

private:
    uint8_t* bits_ = nullptr;
    uint16_t stride_ = 0;
};

}  // namespace cdc::ui
