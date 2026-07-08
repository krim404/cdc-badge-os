/**
 * \file host_api_qr.cpp
 * \brief QR host API: thin mapping onto the cdc_views QrRaster engine.
 *
 * No capability gate - pure compute on caller-supplied memory, like the
 * crypto helpers.
 */

#include "plugin_manager/host_api.h"
#include "cdc_views/QrRaster.h"

namespace qr = cdc::ui::qr;

extern "C" {

int host_qr_measure(const char* data, uint8_t max_version, uint8_t ecc,
                    uint16_t* out_modules) {
    if (!data || !data[0] || !out_modules) return HOST_ERR_INVALID_ARG;
    int rc = qr::measure(data, max_version, ecc, out_modules);
    if (rc == -2) return HOST_ERR_INVALID_ARG;
    return rc == 0 ? HOST_OK : HOST_ERR_GENERIC;
}

int host_qr_render_bitmap(const char* data, uint8_t max_version, uint8_t ecc,
                          uint8_t scale, uint8_t quiet_modules,
                          uint8_t* out, size_t out_size,
                          uint16_t* out_stride_bytes, uint16_t* out_height_px) {
    if (!data || !data[0] || !out || !out_stride_bytes || !out_height_px) {
        return HOST_ERR_INVALID_ARG;
    }
    int rc = qr::render(data, max_version, ecc, scale, quiet_modules,
                        out, out_size, out_stride_bytes, out_height_px);
    switch (rc) {
        case 0:  return HOST_OK;
        case -2: return HOST_ERR_INVALID_ARG;
        case -3: return HOST_ERR_NO_MEMORY;
        default: return HOST_ERR_GENERIC;
    }
}

}  // extern "C"
