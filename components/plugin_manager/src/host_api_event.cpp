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
#include "cdc_hal/IKeypad.h"
#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginManager.h"
#include "plugin_manager/SlotTable.h"

extern "C" void* plg_get_active_plugin(void);
extern "C" void  plg_log_warn(const char* msg);

namespace {

// The public EVENT_* bits (host_api.h) MUST equal 1u << the matching core
// EventType ordinal, because on_bus_event matches subscriptions with
// `1u << evt.type`. cdc::core::EventType is the single source of truth; these
// asserts fail the build if the enum is ever reordered out from under the
// public constants (instead of silently breaking plugin subscriptions).
static_assert(EVENT_KEY_PRESSED       == cdc::core::EventBus::eventMask(cdc::core::EventType::KEY_PRESSED), "EVENT_* drift");
static_assert(EVENT_KEY_RELEASED      == cdc::core::EventBus::eventMask(cdc::core::EventType::KEY_RELEASED), "EVENT_* drift");
static_assert(EVENT_KEY_LONG_PRESS    == cdc::core::EventBus::eventMask(cdc::core::EventType::KEY_LONG_PRESS), "EVENT_* drift");
static_assert(EVENT_POWER_USB_CONN    == cdc::core::EventBus::eventMask(cdc::core::EventType::POWER_USB_CONNECTED), "EVENT_* drift");
static_assert(EVENT_POWER_USB_DISCONN == cdc::core::EventBus::eventMask(cdc::core::EventType::POWER_USB_DISCONNECTED), "EVENT_* drift");
static_assert(EVENT_POWER_CHARGING    == cdc::core::EventBus::eventMask(cdc::core::EventType::POWER_CHARGING), "EVENT_* drift");
static_assert(EVENT_POWER_BATT_LOW    == cdc::core::EventBus::eventMask(cdc::core::EventType::POWER_BATTERY_LOW), "EVENT_* drift");
static_assert(EVENT_POWER_BATT_CRIT   == cdc::core::EventBus::eventMask(cdc::core::EventType::POWER_BATTERY_CRITICAL), "EVENT_* drift");
static_assert(EVENT_SYSTEM_UNLOCK     == cdc::core::EventBus::eventMask(cdc::core::EventType::SYSTEM_UNLOCK), "EVENT_* drift");
static_assert(EVENT_SYSTEM_LOCK       == cdc::core::EventBus::eventMask(cdc::core::EventType::SYSTEM_LOCK), "EVENT_* drift");
static_assert(EVENT_SYSTEM_SLEEP      == cdc::core::EventBus::eventMask(cdc::core::EventType::SYSTEM_SLEEP), "EVENT_* drift");
static_assert(EVENT_SYSTEM_WAKE       == cdc::core::EventBus::eventMask(cdc::core::EventType::SYSTEM_WAKE), "EVENT_* drift");
static_assert(EVENT_BLE_CONNECTED     == cdc::core::EventBus::eventMask(cdc::core::EventType::BLE_CONNECTED), "EVENT_* drift");
static_assert(EVENT_BLE_DISCONNECTED  == cdc::core::EventBus::eventMask(cdc::core::EventType::BLE_DISCONNECTED), "EVENT_* drift");
static_assert(EVENT_TIMER_TICK        == cdc::core::EventBus::eventMask(cdc::core::EventType::TIMER_TICK), "EVENT_* drift");
static_assert(EVENT_MODULE_EVENT      == cdc::core::EventBus::eventMask(cdc::core::EventType::MODULE_EVENT), "EVENT_* drift");
static_assert(EVENT_DISPLAY_REFRESH   == cdc::core::EventBus::eventMask(cdc::core::EventType::DISPLAY_REFRESH), "EVENT_* drift");

struct PluginSubscription {
    void*    plugin     = nullptr;
    uint32_t action_id  = 0;
    uint32_t mask       = 0;
    uint8_t  bus_id     = 0;
    bool     used       = false;
};

constexpr size_t MAX_PLUGIN_SUBSCRIPTIONS = 16;
cdc::plugin_manager::SlotTable<PluginSubscription, MAX_PLUGIN_SUBSCRIPTIONS> s_subs{};

// on_bus_event is registered with the core EventBus exactly once; the per-plugin
// fan-out happens in on_bus_event, so plugin subscriptions never consume more
// than one slot of the small core handler table.
bool    s_core_subscribed = false;
uint8_t s_core_bus_id     = 0;

PluginSubscription* slotForId(uint32_t id) {
    return s_subs.lookup(static_cast<int>(id));
}

// Keep the keypad in deferred short-press mode whenever any plugin is
// subscribed to KEY_LONG_PRESS, so a held key yields only the long-press and
// not the tap. Recomputed from the table on every subscribe/unsubscribe.
void update_long_press_defer() {
    constexpr uint32_t kLongPressBit =
        1u << static_cast<uint8_t>(cdc::core::EventType::KEY_LONG_PRESS);
    bool wanted = false;
    for (auto& s : s_subs.slots) {
        if (s.used && (s.mask & kLongPressBit)) {
            wanted = true;
            break;
        }
    }
    if (auto* kp = cdc::hal::getKeypadInstance()) {
        kp->setDeferShortPress(cdc::hal::IKeypad::DEFER_SRC_EVENT, wanted);
    }
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
    if (!s_core_subscribed) {
        s_core_bus_id = cdc::core::EventBus::instance().subscribe(on_bus_event, 0xFFFFFFFFu);
        s_core_subscribed = true;
    }
    slot->bus_id    = s_core_bus_id;

    update_long_press_defer();
    return slot_id;
}

int host_event_unsubscribe(uint32_t subscription_id)
{
    auto* slot = slotForId(subscription_id);
    if (!slot) return HOST_ERR_NOT_FOUND;
    // The shared core subscription stays registered for the process lifetime;
    // only the per-plugin fan-out slot is released here.
    *slot = PluginSubscription{};
    update_long_press_defer();
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
