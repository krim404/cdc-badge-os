/**
 * \file QrRaster.h
 * \brief QR encoding into a caller-provided 1-bpp packed raster (no display).
 *
 * Shares the espressif/qrcode dependency with QRCodeView but renders into
 * memory: packed rows, MSB-first, a set bit is black - the common bitmap
 * layout of MonoBitmap, the surface API and the thermo_print job format.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::ui::qr {

/// Hard cap on the rendered side length in pixels (incl. quiet zone, scaled).
inline constexpr uint16_t QR_MAX_SIDE_PX = 1024;

/**
 * \brief Sets the packed pixels of one black QR module.
 *
 * Pure bit-packing shared by the firmware renderer and the host unit tests:
 * module (mx, my) inks a `scale x scale` pixel block offset by the quiet
 * zone. Rows are MSB-first, a set bit is black.
 * \param out Packed raster, `stride` bytes per row, pre-cleared to white.
 * \param stride Bytes per row.
 * \param scale Pixels per module, >= 1.
 * \param quiet Quiet-zone width in modules per edge.
 * \param mx Module column.
 * \param my Module row.
 */
inline void packModule(uint8_t* out, uint16_t stride, uint8_t scale, uint8_t quiet,
                       int mx, int my)
{
    const int px0 = (quiet + mx) * scale;
    const int py0 = (quiet + my) * scale;
    for (int dy = 0; dy < scale; dy++) {
        uint8_t* row = out + static_cast<size_t>(py0 + dy) * stride;
        for (int dx = 0; dx < scale; dx++) {
            const int px = px0 + dx;
            row[px >> 3] |= static_cast<uint8_t>(0x80u >> (px & 7));
        }
    }
}

/**
 * \brief Measures the module count for `data` without rendering.
 * \param data NUL-terminated payload text.
 * \param max_version Maximum QR version 1..20 (0 defaults to 20).
 * \param ecc ECC level 0..3 (LOW..HIGH).
 * \param out_modules Receives the side length in modules.
 * \return 0 on success, negative on failure (bad args or data too long).
 */
int measure(const char* data, uint8_t max_version, uint8_t ecc, uint16_t* out_modules);

/**
 * \brief Encodes `data` and packs it into a 1-bpp raster.
 *
 * The rendered side is `(modules + 2 * quiet_modules) * scale` pixels; the
 * quiet zone is white. Rows are packed MSB-first with a set bit meaning black.
 * \param data NUL-terminated payload text.
 * \param max_version Maximum QR version 1..20 (0 defaults to 20).
 * \param ecc ECC level 0..3 (LOW..HIGH).
 * \param scale Pixels per module, >= 1.
 * \param quiet_modules Quiet-zone width in modules per edge.
 * \param out Caller buffer for the packed rows.
 * \param out_size Capacity of `out` in bytes.
 * \param out_stride_bytes Receives the row stride `(side_px + 7) / 8`.
 * \param out_height_px Receives the side length in pixels.
 * \return 0 on success, -1 on encode failure, -2 on bad args / side too
 *         large, -3 when `out` is too small.
 */
int render(const char* data, uint8_t max_version, uint8_t ecc,
           uint8_t scale, uint8_t quiet_modules,
           uint8_t* out, size_t out_size,
           uint16_t* out_stride_bytes, uint16_t* out_height_px);

}  // namespace cdc::ui::qr
