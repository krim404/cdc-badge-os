/**
 * \file host_api_usb.cpp
 * \brief Raw USB-CDC TX for plugins (thin wrapper over usb_badge usb_cdc).
 */

#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "usb_badge/usb_cdc.h"

extern "C" void* plg_get_active_plugin(void);

namespace {
bool allowed() {
    auto* p = static_cast<cdc::plugin_manager::Plugin*>(plg_get_active_plugin());
    return p && p->manifest().capabilities.usb_cdc;
}
}  // namespace

extern "C" {

int host_usb_cdc_write(const uint8_t* data, size_t len)
{
    if (!data && len) return HOST_ERR_INVALID_ARG;
    if (!allowed()) return HOST_ERR_NO_CAPABILITY;
    if (len == 0) return HOST_OK;
    size_t written = usb_cdc_write(data, len);
    return written == len ? HOST_OK : HOST_ERR_GENERIC;
}

}  // extern "C"
