/**
 * \file host_api_lifecycle.cpp
 * \brief Plugin lifecycle host API: opt-in background residency.
 *
 * A `background`/`autoload` capability is only permission. By default a plugin
 * is torn down when the user leaves it (or, for autoload, right after boot
 * init). Calling host_set_resident(true) — typically in plugin_init for an
 * autoload service, or while running for a background one — makes PluginManager
 * keep the instance resident. Calling it with false opts back out.
 */

#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"

namespace pm = cdc::plugin_manager;

extern "C" void* plg_get_active_plugin(void);

extern "C" int host_set_resident(bool resident)
{
    auto* p = static_cast<pm::Plugin*>(plg_get_active_plugin());
    if (!p) return HOST_ERR_GENERIC;
    p->setResidentRequested(resident);
    return HOST_OK;
}
