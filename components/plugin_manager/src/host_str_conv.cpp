/**
 * \file host_str_conv.cpp
 * \brief Implementation of the plugin-boundary UTF-8 <-> CP437 helpers.
 */

#include "host_str_conv.h"

#include "plugin_manager/host_api.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_core/Cp437.h"

#include <cstring>

namespace cdc::plugin_manager {

std::string toDisplay(const char* utf8)
{
    if (!utf8 || !*utf8) return std::string();
    size_t cap = std::strlen(utf8) + 1;
    std::string out;
    out.resize(cap);
    cdc::ui::render::decodeWebText(utf8, &out[0], cap, cdc::ui::render::DisplayTarget::Cp437);
    out.resize(std::strlen(out.c_str()));
    return out;
}

int copyUtf8(const char* cp437, char* out, size_t out_size)
{
    if (!out || out_size == 0) return HOST_ERR_INVALID_ARG;
    std::string u = cp437 ? cdc::core::cp437::toUtf8(cp437) : std::string();
    size_t n = u.size();
    if (n >= out_size) {
        n = out_size - 1;
        // Back off so the buffer never ends inside a multi-byte UTF-8 sequence.
        while (n > 0 && (static_cast<unsigned char>(u[n]) & 0xC0) == 0x80) --n;
    }
    std::memcpy(out, u.data(), n);
    out[n] = '\0';
    return static_cast<int>(n);
}

}  // namespace cdc::plugin_manager
