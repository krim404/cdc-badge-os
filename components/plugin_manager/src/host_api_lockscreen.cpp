/**
 * \file host_api_lockscreen.cpp
 * \brief Lockscreen quick-action registry exposed to plugins.
 *
 * Plugins call host_lockscreen_register_action() during plugin_init to add
 * a single item to the lockscreen's context menu. The label is resolved
 * via i18n at render time so it follows the active language without
 * re-registration.
 */

#include "plugin_manager/host_api.h"
#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginManager.h"
#include "plugin_manager/LockscreenRegistry.h"
#include "plugin_manager/SlotTable.h"
#include "cdc_views/ConfirmView.h"
#include "cdc_ui/ViewStack.h"
#include "host_str_conv.h"

#include <cstring>
#include <string>

extern "C" void* plg_get_active_plugin(void);

namespace cdc::plugin_manager {

namespace {

constexpr size_t MAX_LOCKSCREEN_ITEMS = 8;
SlotTable<LockscreenRegistration, MAX_LOCKSCREEN_ITEMS> s_items{};

LockscreenRegistration* slotFor(void* plugin)
{
    for (auto& s : s_items.slots) if (s.used && s.plugin == plugin) return &s;
    return nullptr;
}

// Single persistent Y/N alert that overlays whatever is on screen (lock screen
// included) and routes the answer back to the originating plugin, which may be
// running headless in the background.
struct AlertState {
    cdc::ui::ConfirmView view{};
    void*    plugin    = nullptr;
    uint32_t action_id = 0;
    bool     active    = false;
};
AlertState s_alert{};

cdc::ui::ConfirmView::Icon toConfirmIcon(uint8_t icon)
{
    switch (icon) {
        case UI_ICON_ERROR: return cdc::ui::ConfirmView::Icon::ERROR;
        case UI_ICON_ALERT: return cdc::ui::ConfirmView::Icon::WARNING;
        default:            return cdc::ui::ConfirmView::Icon::QUESTION;
    }
}

void onAlertResult(uint32_t answer)
{
    void* p = s_alert.plugin;
    uint32_t action_id = s_alert.action_id;
    s_alert.active = false;
    s_alert.plugin = nullptr;
    if (p) PluginManager::instance().dispatchActionTo(static_cast<Plugin*>(p), action_id, 0, answer);
}

void onAlertYes(void*) { onAlertResult(1); }
void onAlertNo(void*)  { onAlertResult(0); }

}  // namespace

uint8_t collectLockscreenItems(LockscreenRegistration* out, uint8_t max)
{
    uint8_t n = 0;
    for (auto& s : s_items.slots) {
        if (!s.used) continue;
        if (n >= max) break;
        out[n++] = s;
    }
    return n;
}

void clearLockscreenRegistrationFor(void* plugin)
{
    if (auto* slot = slotFor(plugin)) {
        *slot = LockscreenRegistration{};
    }
    // Drop ownership of a pending alert so its Y/N no longer dispatches into a
    // plugin that is being unloaded. The modal view is static and stays valid;
    // the user dismisses it normally and the answer is discarded.
    if (s_alert.active && s_alert.plugin == plugin) {
        s_alert.active = false;
        s_alert.plugin = nullptr;
    }
}

}  // namespace cdc::plugin_manager

extern "C" {

int host_lockscreen_register_action(const char* label_key, uint32_t action_id)
{
    auto* plugin = plg_get_active_plugin();
    if (!plugin)      return HOST_ERR_NO_CAPABILITY;
    if (!label_key)   return HOST_ERR_INVALID_ARG;

    using cdc::plugin_manager::LockscreenRegistration;

    LockscreenRegistration* slot = nullptr;
    for (auto& s : cdc::plugin_manager::s_items.slots) {
        if (s.used && s.plugin == plugin) { slot = &s; break; }
    }
    if (!slot) {
        int slot_id = 0;
        slot = cdc::plugin_manager::s_items.allocate(slot_id);
        if (!slot) return HOST_ERR_NO_MEMORY;
    }
    slot->plugin    = plugin;
    slot->action_id = action_id;
    std::strncpy(slot->label_key, label_key, sizeof(slot->label_key) - 1);
    slot->label_key[sizeof(slot->label_key) - 1] = '\0';
    slot->used = true;
    return HOST_OK;
}

int host_lockscreen_unregister_action(void)
{
    auto* plugin = plg_get_active_plugin();
    if (!plugin) return HOST_ERR_NO_CAPABILITY;
    cdc::plugin_manager::clearLockscreenRegistrationFor(plugin);
    return HOST_OK;
}

int host_lockscreen_alert(const char* text, uint8_t icon, uint32_t action_id)
{
    auto* plugin = plg_get_active_plugin();
    if (!plugin) return HOST_ERR_NO_CAPABILITY;
    if (!text)   return HOST_ERR_INVALID_ARG;

    auto& vs = cdc::ui::ViewStack::instance();
    // Never clobber an exclusive prompt (FIDO2). Other modals are fine: the
    // alert stacks on top and receives input until dismissed.
    if (vs.exclusiveOwner() != nullptr) return HOST_ERR_BUSY;

    std::string cp = cdc::plugin_manager::toDisplay(text);
    cdc::plugin_manager::s_alert.plugin    = plugin;
    cdc::plugin_manager::s_alert.action_id = action_id;
    cdc::plugin_manager::s_alert.active    = true;
    cdc::plugin_manager::s_alert.view.init(cp.c_str(),
                                           cdc::plugin_manager::toConfirmIcon(icon));
    cdc::plugin_manager::s_alert.view.setOnConfirm(&cdc::plugin_manager::onAlertYes, nullptr);
    cdc::plugin_manager::s_alert.view.setOnCancel (&cdc::plugin_manager::onAlertNo,  nullptr);
    vs.showModal(&cdc::plugin_manager::s_alert.view);
    vs.render();
    return HOST_OK;
}

}  // extern "C"
