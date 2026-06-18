#pragma once

#include <cstddef>
#include <cstdint>

#include "cdc_core/Raii.h"

namespace cdc::image {

/**
 * \brief Decodes a PNG or JPEG into an 8-bit grayscale buffer (PSRAM).
 *
 * 0 = black, 255 = white. PNG transparency is composited onto white. The format
 * is auto-detected from magic bytes; unsupported formats fail. Images larger
 * than \p maxPixels are rejected (JPEG is first scaled down by the decoder when
 * possible). On failure an empty pointer is returned and \p errorKey is set to
 * an i18n key (`core.img_*`); nothing is allocated.
 *
 * \param data Encoded image bytes.
 * \param len Byte length.
 * \param maxPixels Maximum decoded width*height.
 * \param w Output: decoded width.
 * \param h Output: decoded height.
 * \param errorKey Output: i18n error key on failure, else nullptr.
 * \return Grayscale buffer of `w * h` bytes, or empty on failure.
 */
cdc::core::PsramUniquePtr<uint8_t> decodeToGray(const uint8_t* data, size_t len,
                                                uint32_t maxPixels,
                                                uint16_t& w, uint16_t& h,
                                                const char*& errorKey);

/**
 * \brief Box-average downscale of a grayscale image (`dw <= sw`, `dh <= sh`).
 * \param src Source grayscale, `sw * sh` bytes.
 * \param sw Source width.
 * \param sh Source height.
 * \param dst Destination grayscale, `dw * dh` bytes.
 * \param dw Destination width.
 * \param dh Destination height.
 */
void downscaleGrayBox(const uint8_t* src, uint16_t sw, uint16_t sh,
                      uint8_t* dst, uint16_t dw, uint16_t dh);

} // namespace cdc::image
