/**
 * \file host_api_power.cpp
 * \brief Real implementations of the power-state host API.
 */

#include "cdc_hal/IPowerManager.h"
#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "cdc_os_ui/SleepManager.h"

using cdc::hal::IPowerManager;
using cdc::hal::getPowerManagerInstance;

extern "C" void* plg_get_active_plugin(void);

extern "C" {

uint16_t host_battery_mv(void)
{
    auto* p = getPowerManagerInstance();
    return p ? static_cast<uint16_t>(p->getBatteryVoltage()) : 0;
}

uint8_t host_battery_pct(void)
{
    auto* p = getPowerManagerInstance();
    return p ? p->getBatteryPercent() : 0;
}

bool host_is_usb_connected(void)
{
    auto* p = getPowerManagerInstance();
    return p ? p->isUsbConnected() : false;
}

uint8_t host_power_source(void)
{
    auto* p = getPowerManagerInstance();
    if (!p) return POWER_SRC_UNKNOWN;
    // HAL PowerSource enum ordering differs from the POWER_SRC_* ABI values.
    switch (p->getPowerSource()) {
        case cdc::hal::PowerSource::BATTERY: return POWER_SRC_BATTERY;
        case cdc::hal::PowerSource::USB:     return POWER_SRC_USB;
        default:                             return POWER_SRC_UNKNOWN;
    }
}

uint8_t host_charge_status(void)
{
    auto* p = getPowerManagerInstance();
    return p ? static_cast<uint8_t>(p->getChargeStatus()) : CHARGE_NOT_CHARGING;
}

bool host_is_battery_low(void)
{
    auto* p = getPowerManagerInstance();
    return p ? p->isBatteryLow() : false;
}

bool host_is_battery_critical(void)
{
    auto* p = getPowerManagerInstance();
    return p ? p->isBatteryCritical() : false;
}

void host_set_sleep_inhibit(uint32_t on)
{
    auto* plugin = static_cast<cdc::plugin_manager::Plugin*>(plg_get_active_plugin());
    if (!plugin) return;
    // Key the inhibitor by the plugin id (stable for the plugin's RAM
    // lifetime). PluginManager releases it on unload via applySleepInhibitor.
    auto& sm = cdc::ui::SleepManager::instance();
    if (on) {
        sm.addSleepInhibitor(plugin->id().c_str());
    } else {
        sm.removeSleepInhibitor(plugin->id().c_str());
    }
}

}  // extern "C"
