#include "cdc_image/ImageDecoder.h"

#include "cdc_image/ImageFormat.h"
#include "cdc_core/feature_flags.h"

#include <cstring>

#include "esp_heap_caps.h"

#if FEATURE_IMG_PNG
#include "png.h"
#endif
#if FEATURE_IMG_JPEG
#include <csetjmp>
extern "C" {
#include "jpeglib.h"
}
#endif

namespace cdc::image {

#if FEATURE_IMG_JPEG
namespace {
struct JpegErrMgr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};
}  // namespace

extern "C" {
static void cdcJpegErrorExit(j_common_ptr cinfo) {
    std::longjmp(reinterpret_cast<JpegErrMgr*>(cinfo->err)->jb, 1);
}
static void cdcJpegOutput(j_common_ptr) {}  // silence libjpeg trace/warning output
}  // extern "C"
#endif

cdc::core::PsramUniquePtr<uint8_t> decodeToGray(const uint8_t* data, size_t len,
                                                uint32_t maxPixels,
                                                uint16_t& w, uint16_t& h,
                                                const char*& errorKey) {
    errorKey = nullptr;
    w = 0;
    h = 0;
    const ImageFormat fmt = detectImageFormat(data, len);

#if FEATURE_IMG_PNG
    if (fmt == ImageFormat::Png) {
        png_image image;
        std::memset(&image, 0, sizeof(image));
        image.version = PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_memory(&image, data, len)) {
            errorKey = "core.img_decode_failed";
            return {};
        }
        if (static_cast<uint32_t>(image.width) * image.height == 0 ||
            static_cast<uint32_t>(image.width) * image.height > maxPixels) {
            png_image_free(&image);
            errorKey = "core.img_too_large";
            return {};
        }
        image.format = PNG_FORMAT_GRAY;
        const uint16_t pw = static_cast<uint16_t>(image.width);
        const uint16_t ph = static_cast<uint16_t>(image.height);
        auto gray = cdc::core::psramAlloc<uint8_t>(static_cast<size_t>(pw) * ph);
        if (!gray) {
            png_image_free(&image);
            errorKey = "core.img_decode_failed";
            return {};
        }
        png_color bg;
        bg.red = 255;
        bg.green = 255;
        bg.blue = 255;
        if (!png_image_finish_read(&image, &bg, gray.get(), 0, nullptr)) {
            png_image_free(&image);
            errorKey = "core.img_decode_failed";
            return {};
        }
        png_image_free(&image);
        w = pw;
        h = ph;
        return gray;
    }
#endif

#if FEATURE_IMG_JPEG
    if (fmt == ImageFormat::Jpeg) {
        struct jpeg_decompress_struct cinfo;
        JpegErrMgr jerr;
        std::memset(&cinfo, 0, sizeof(cinfo));
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = cdcJpegErrorExit;
        jerr.pub.output_message = cdcJpegOutput;

        // Held across setjmp, so volatile: freed in the error path on a libjpeg
        // longjmp; adopted into a PsramUniquePtr only after the last error point.
        uint8_t* volatile rawGray = nullptr;
        if (setjmp(jerr.jb)) {
            jpeg_destroy_decompress(&cinfo);
            if (rawGray) heap_caps_free(rawGray);
            if (!errorKey) errorKey = "core.img_decode_failed";
            return {};
        }

        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(len));
        jpeg_read_header(&cinfo, TRUE);

        // DCT-scale down (1/1, 1/2, 1/4, 1/8) so the output fits the pixel cap.
        unsigned denom = 1;
        while (denom < 8 &&
               (static_cast<uint32_t>((cinfo.image_width + denom - 1) / denom) *
                ((cinfo.image_height + denom - 1) / denom)) > maxPixels) {
            denom <<= 1;
        }
        cinfo.scale_num = 1;
        cinfo.scale_denom = denom;
        cinfo.out_color_space = JCS_GRAYSCALE;
        cinfo.dct_method = JDCT_ISLOW;

        jpeg_start_decompress(&cinfo);
        const uint16_t pw = static_cast<uint16_t>(cinfo.output_width);
        const uint16_t ph = static_cast<uint16_t>(cinfo.output_height);
        if (static_cast<uint32_t>(pw) * ph == 0 ||
            static_cast<uint32_t>(pw) * ph > maxPixels) {
            errorKey = "core.img_too_large";
            jpeg_destroy_decompress(&cinfo);
            return {};
        }

        rawGray = static_cast<uint8_t*>(heap_caps_malloc(
            static_cast<size_t>(pw) * ph, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!rawGray) {
            errorKey = "core.img_decode_failed";
            jpeg_destroy_decompress(&cinfo);
            return {};
        }

        while (cinfo.output_scanline < cinfo.output_height) {
            JSAMPROW row = rawGray + static_cast<size_t>(cinfo.output_scanline) * pw;
            jpeg_read_scanlines(&cinfo, &row, 1);
        }

        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        w = pw;
        h = ph;
        return cdc::core::PsramUniquePtr<uint8_t>(rawGray);
    }
#endif

    errorKey = "core.img_unsupported";
    return {};
}

void downscaleGrayBox(const uint8_t* src, uint16_t sw, uint16_t sh,
                      uint8_t* dst, uint16_t dw, uint16_t dh) {
    if (!src || !dst || dw == 0 || dh == 0 || sw == 0 || sh == 0) return;
    for (uint16_t dy = 0; dy < dh; ++dy) {
        uint32_t sy0 = static_cast<uint32_t>(dy) * sh / dh;
        uint32_t sy1 = static_cast<uint32_t>(dy + 1) * sh / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > sh) sy1 = sh;
        for (uint16_t dx = 0; dx < dw; ++dx) {
            uint32_t sx0 = static_cast<uint32_t>(dx) * sw / dw;
            uint32_t sx1 = static_cast<uint32_t>(dx + 1) * sw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > sw) sx1 = sw;
            uint32_t sum = 0, cnt = 0;
            for (uint32_t yy = sy0; yy < sy1; ++yy) {
                for (uint32_t xx = sx0; xx < sx1; ++xx) {
                    sum += src[static_cast<size_t>(yy) * sw + xx];
                    ++cnt;
                }
            }
            dst[static_cast<size_t>(dy) * dw + dx] =
                cnt ? static_cast<uint8_t>(sum / cnt) : 0;
        }
    }
}

} // namespace cdc::image
