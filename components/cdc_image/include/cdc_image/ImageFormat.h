#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::image {

/// \brief Supported encoded image formats.
enum class ImageFormat : uint8_t {
    Unknown = 0,
    Png,
    Jpeg,
};

/**
 * \brief Detects the image format from leading magic bytes.
 * \param data Encoded image bytes (may be null).
 * \param len Byte length.
 * \return The detected format, or \ref ImageFormat::Unknown when the data is
 *         too short or unrecognized.
 */
ImageFormat detectImageFormat(const uint8_t* data, size_t len);

} // namespace cdc::image
