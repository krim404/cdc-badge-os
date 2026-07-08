/**
 * \file JpegEncoder.h
 * \brief Grayscale JPEG encoding of 1-bpp packed bitmaps (libjpeg).
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::image {

/**
 * \brief Encodes a packed 1-bpp bitmap as a grayscale JPEG.
 *
 * Input rows are MSB-first with a set bit meaning black; they expand to
 * 8-bit gray (0x00 black / 0xFF white) band-wise during compression.
 * \param bits Packed rows, `stride * h` bytes.
 * \param w Width in pixels.
 * \param h Height in pixels.
 * \param stride Bytes per packed row, >= (w + 7) / 8.
 * \param quality JPEG quality 1..100 (0 defaults to 85).
 * \param out Caller buffer for the encoded JPEG.
 * \param out_size Capacity of `out` in bytes.
 * \param out_len Receives the encoded byte count.
 * \return 0 on success, -1 on encode failure, -2 on bad args, -3 when `out`
 *         is too small (`out_len` then holds the required size).
 */
int encodeMonoJpeg(const uint8_t* bits, uint16_t w, uint16_t h, uint16_t stride,
                   uint8_t quality, uint8_t* out, size_t out_size, size_t* out_len);

}  // namespace cdc::image
