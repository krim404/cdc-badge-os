/**
 * \file host_api_strings.cpp
 * \brief WAMR-facing wrappers around the plugin-boundary string codecs.
 *
 * Since the host API unified on UTF-8, the UI/canvas/display functions convert
 * internally and plugins no longer need to pre-convert. These two functions
 * remain for explicit use (e.g. targeting a Latin-1 GFX font).
 */

#include "plugin_manager/host_api.h"
#include "cdc_views/RenderHelpers.h"
#include "host_str_conv.h"

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
    return cdc::plugin_manager::copyUtf8(in, out, out_size);
}
