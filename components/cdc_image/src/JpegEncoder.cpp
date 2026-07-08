/**
 * \file JpegEncoder.cpp
 * \brief Grayscale JPEG encoding of 1-bpp packed bitmaps via libjpeg.
 *
 * libjpeg's jpeg_mem_dest owns and grows its own buffer; the result is copied
 * into the caller's buffer afterwards, so a too-small caller buffer reports
 * the required size instead of corrupting memory.
 */

#include "cdc_image/JpegEncoder.h"
#include "cdc_core/Raii.h"

#include <csetjmp>
#include <cstdlib>
#include <cstring>

#include "jpeglib.h"

namespace cdc::image {

namespace {

struct EncErrorMgr {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

void encErrorExit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<EncErrorMgr*>(cinfo->err);
    longjmp(err->jump, 1);
}

}  // namespace

int encodeMonoJpeg(const uint8_t* bits, uint16_t w, uint16_t h, uint16_t stride,
                   uint8_t quality, uint8_t* out, size_t out_size, size_t* out_len) {
    if (!bits || !out || !out_len || w == 0 || h == 0) return -2;
    if (stride < (w + 7) / 8) return -2;
    if (quality == 0) quality = 85;
    if (quality > 100) quality = 100;

    auto row = cdc::core::psramAlloc<uint8_t>(w);
    if (!row) return -1;

    struct jpeg_compress_struct cinfo;
    EncErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = encErrorExit;

    unsigned char* jpegBuf = nullptr;
    size_t jpegLen = 0;

    if (setjmp(jerr.jump)) {
        jpeg_destroy_compress(&cinfo);
        if (jpegBuf) free(jpegBuf);
        return -1;
    }

    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &jpegBuf, &jpegLen);

    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 1;
    cinfo.in_color_space = JCS_GRAYSCALE;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t* packed = bits + static_cast<size_t>(cinfo.next_scanline) * stride;
        for (uint16_t x = 0; x < w; x++) {
            const bool black = (packed[x >> 3] & (0x80u >> (x & 7))) != 0;
            row[x] = black ? 0x00 : 0xFF;
        }
        JSAMPROW rows[1] = { row.get() };
        jpeg_write_scanlines(&cinfo, rows, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    *out_len = static_cast<size_t>(jpegLen);
    int rc = 0;
    if (jpegLen <= out_size) {
        std::memcpy(out, jpegBuf, jpegLen);
    } else {
        rc = -3;
    }
    free(jpegBuf);
    return rc;
}

}  // namespace cdc::image
