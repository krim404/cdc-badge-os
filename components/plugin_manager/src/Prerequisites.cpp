#include "plugin_manager/Prerequisites.h"
#include "cdc_hal/IPowerManager.h"
#include "cdc_hal/IRtc.h"
#include "cdc_os_ui/WifiHandlers.h"
#include "cdc_log.h"

#include "esp_heap_caps.h"

#include <cstdlib>

namespace cdc::plugin_manager {

static const char* TAG = "PLG_PRE";

namespace {

int as_int(const std::map<std::string, std::string>& params,
           const std::string& key, int dflt)
{
    auto it = params.find(key);
    if (it == params.end()) return dflt;
    return std::atoi(it->second.c_str());
}

PrereqResult prereq_wifi_connected(const PrerequisiteSpec& spec)
{
    (void)spec;
    bool ok = cdc::ui::WifiHandlers::instance().acquire();
    return ok ? PrereqResult::Ok : PrereqResult::HardFailed;
}

PrereqResult prereq_time_synced(const PrerequisiteSpec&)
{
    auto* rtc = cdc::hal::getRtcInstance();
    return (rtc && rtc->isTimeSet()) ? PrereqResult::Ok : PrereqResult::HardFailed;
}

PrereqResult prereq_battery_min(const PrerequisiteSpec& spec)
{
    int min_pct = as_int(spec.params, "min_pct", 0);
    auto* p = cdc::hal::getPowerManagerInstance();
    if (!p) return PrereqResult::HardFailed;
    return (p->getBatteryPercent() >= min_pct) ? PrereqResult::Ok : PrereqResult::HardFailed;
}

PrereqResult prereq_usb_connected(const PrerequisiteSpec&)
{
    auto* p = cdc::hal::getPowerManagerInstance();
    return (p && p->isUsbConnected()) ? PrereqResult::Ok : PrereqResult::HardFailed;
}

PrereqResult prereq_not_charging(const PrerequisiteSpec&)
{
    auto* p = cdc::hal::getPowerManagerInstance();
    if (!p) return PrereqResult::HardFailed;
    auto s = p->getChargeStatus();
    bool charging = (s == cdc::hal::ChargeStatus::FAST_CHARGE) ||
                    (s == cdc::hal::ChargeStatus::PRE_CHARGE);
    return charging ? PrereqResult::HardFailed : PrereqResult::Ok;
}

PrereqResult prereq_min_free_psram(const PrerequisiteSpec& spec)
{
    int min_kb = as_int(spec.params, "kb", 0);
    size_t free_b = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return (free_b >= static_cast<size_t>(min_kb) * 1024)
           ? PrereqResult::Ok : PrereqResult::HardFailed;
}

PrereqResult prereq_min_free_dram(const PrerequisiteSpec& spec)
{
    int min_kb = as_int(spec.params, "kb", 0);
    size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    return (free_b >= static_cast<size_t>(min_kb) * 1024)
           ? PrereqResult::Ok : PrereqResult::HardFailed;
}

PrereqResult prereq_unlocked(const PrerequisiteSpec&)
{
    // Plugin start happens from the post-unlock UI, so this is always true
    // by the time we get here. Kept as an explicit prerequisite anyway so
    // future paths (auto-start from boot etc.) can still gate on it.
    return PrereqResult::Ok;
}

PrereqResult check_one(const PrerequisiteSpec& spec)
{
    if (spec.name == "wifi_connected")    return prereq_wifi_connected(spec);
    if (spec.name == "time_synced")       return prereq_time_synced(spec);
    if (spec.name == "battery_min")       return prereq_battery_min(spec);
    if (spec.name == "usb_connected")     return prereq_usb_connected(spec);
    if (spec.name == "not_charging")      return prereq_not_charging(spec);
    if (spec.name == "min_free_psram")    return prereq_min_free_psram(spec);
    if (spec.name == "min_free_dram")     return prereq_min_free_dram(spec);
    if (spec.name == "unlocked")          return prereq_unlocked(spec);
    LOG_W(TAG, "unknown prerequisite '%s' - treating as soft-pass", spec.name.c_str());
    return PrereqResult::Ok;
}

void release_one(const std::string& prereq_name)
{
    if (prereq_name == "wifi_connected") {
        cdc::ui::WifiHandlers::instance().release();
    }
    // Other prerequisites are read-only - no cleanup.
}

}  // namespace

PrereqResult Prerequisites::walk(Plugin& plugin,
                                 std::string& out_failed_name,
                                 std::string& out_on_fail)
{
    for (const auto& spec : plugin.manifest().prerequisites) {
        PrereqResult r = check_one(spec);
        if (r == PrereqResult::Ok) {
            plugin.acquired_prereqs.push_back(spec.name);
            continue;
        }
        out_failed_name = spec.name;
        out_on_fail     = spec.on_fail.empty() ? "abort" : spec.on_fail;
        LOG_W(TAG, "prerequisite '%s' failed for %s (on_fail=%s)",
              spec.name.c_str(), plugin.id().c_str(), out_on_fail.c_str());
        if (out_on_fail == "abort") return PrereqResult::HardFailed;
        return PrereqResult::SoftFailed;
    }
    return PrereqResult::Ok;
}

void Prerequisites::release(Plugin& plugin)
{
    for (auto it = plugin.acquired_prereqs.rbegin();
         it != plugin.acquired_prereqs.rend(); ++it) {
        release_one(*it);
    }
    plugin.acquired_prereqs.clear();
}

}  // namespace cdc::plugin_manager
