#include "cdc_image/ImageFormat.h"

namespace cdc::image {

ImageFormat detectImageFormat(const uint8_t* d, size_t n) {
    if (!d) return ImageFormat::Unknown;
    if (n >= 4 && d[0] == 0x89 && d[1] == 0x50 && d[2] == 0x4E && d[3] == 0x47) {
        return ImageFormat::Png;
    }
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
        return ImageFormat::Jpeg;
    }
    return ImageFormat::Unknown;
}

} // namespace cdc::image
