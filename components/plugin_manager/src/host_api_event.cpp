/**
 * \file host_api_event.cpp
 * \brief EventBus subscribe/unsubscribe/publish for plugins.
 *
 * Subscriptions are bound to the plugin instance that registered them
 * (foreground or background). When an event matches, the host dispatches
 * plugin_on_action(action_id, evt_type, evt_value) to the owning plugin
 * regardless of who currently has the UI focus.
 */

#include "cdc_core/EventBus.h"
#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginManager.h"
#include "plugin_manager/SlotTable.h"

extern "C" void* plg_get_active_plugin(void);
extern "C" void  plg_log_warn(const char* msg);

namespace {

struct PluginSubscription {
    void*    plugin     = nullptr;
    uint32_t action_id  = 0;
    uint32_t mask       = 0;
    uint8_t  bus_id     = 0;
    bool     used       = false;
};

constexpr size_t MAX_PLUGIN_SUBSCRIPTIONS = 16;
cdc::plugin_manager::SlotTable<PluginSubscription, MAX_PLUGIN_SUBSCRIPTIONS> s_subs{};

PluginSubscription* slotForId(uint32_t id) {
    return s_subs.lookup(static_cast<int>(id));
}

void on_bus_event(const cdc::core::Event& evt) {
    const uint32_t bit = 1u << static_cast<uint8_t>(evt.type);
    for (auto& s : s_subs.slots) {
        if (!s.used || (s.mask & bit) == 0) continue;
        auto* plugin = static_cast<cdc::plugin_manager::Plugin*>(s.plugin);
        cdc::plugin_manager::PluginManager::instance().dispatchActionTo(
            plugin,
            s.action_id,
            static_cast<uint32_t>(evt.type),
            evt.data.value);
    }
}

}  // namespace

extern "C" {

int host_event_subscribe(uint32_t event_mask, uint32_t action_id)
{
    auto* plugin = plg_get_active_plugin();
    if (!plugin) return HOST_ERR_NO_CAPABILITY;

    int slot_id = 0;
    PluginSubscription* slot = s_subs.allocate(slot_id);
    if (!slot) {
        plg_log_warn("EventBus: plugin subscription table full");
        return HOST_ERR_NO_MEMORY;
    }

    slot->plugin    = plugin;
    slot->action_id = action_id;
    slot->mask      = event_mask;
    slot->used      = true;
    slot->bus_id    = cdc::core::EventBus::instance().subscribe(on_bus_event, event_mask);

    return slot_id;
}

int host_event_unsubscribe(uint32_t subscription_id)
{
    auto* slot = slotForId(subscription_id);
    if (!slot) return HOST_ERR_NOT_FOUND;
    cdc::core::EventBus::instance().unsubscribe(slot->bus_id);
    *slot = PluginSubscription{};
    return HOST_OK;
}

int host_event_publish(uint32_t module_event_subtype, uint32_t value)
{
    // The v1 Event carries a single byte (data.value), defined by the core as the
    // module-event sub-type. The separate `value` argument has no transport slot
    // in this model and is not forwarded to subscribers.
    (void)value;
    cdc::core::Event evt{};
    evt.type       = cdc::core::EventType::MODULE_EVENT;
    evt.timestamp  = 0;
    evt.data.value = static_cast<uint8_t>(module_event_subtype & 0xff);
    cdc::core::EventBus::instance().publish(evt);
    return HOST_OK;
}

}  // extern "C"
