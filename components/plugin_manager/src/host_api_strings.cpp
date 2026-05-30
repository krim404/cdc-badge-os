/**
 * \file host_api_strings.cpp
 * \brief Thin WAMR-facing wrapper around cdc::ui::render::decodeWebText.
 */

#include "plugin_manager/host_api.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_core/Cp437.h"

#include <cstring>
#include <string>

extern "C" int host_str_to_display(const char* in, char* out, size_t out_size, uint32_t target)
{
    if (!in || !out || out_size == 0) return HOST_ERR_INVALID_ARG;
    auto t = (target == HOST_STR_TARGET_LATIN1)
                 ? cdc::ui::render::DisplayTarget::Latin1
                 : cdc::ui::render::DisplayTarget::Cp437;
    cdc::ui::render::decodeWebText(in, out, out_size, t);
    return HOST_OK;
}

extern "C" int host_str_to_utf8(const char* in, char* out, size_t out_size)
{
    if (!in || !out || out_size == 0) return HOST_ERR_INVALID_ARG;
    std::string u = cdc::core::cp437::toUtf8(in);
    size_t n = (u.size() < out_size - 1) ? u.size() : out_size - 1;
    std::memcpy(out, u.data(), n);
    out[n] = '\0';
    return static_cast<int>(n);
}
