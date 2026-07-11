#include "cdc_core/UsbManager.h"
#include "cdc_log.h"
#include "usb_badge/usb_hid.h"
#include <string.h>

static const char* TAG = "UsbManager";

namespace cdc::core {

/**
 * \brief Returns singleton USB manager instance.
 * \return Manager singleton reference.
 */
UsbManager& UsbManager::instance() {
    static UsbManager s_instance;
    return s_instance;
}

/**
 * \brief Initializes USB manager service state.
 * \return Always `true`.
 */
bool UsbManager::init() {
    state_ = ServiceState::INITIALIZED;
    return true;
}

/**
 * \brief Starts USB manager service state.
 * \return Always `true`.
 */
bool UsbManager::start() {
    state_ = ServiceState::STARTED;
    return true;
}

/**
 * \brief Stops USB manager service state.
 */
void UsbManager::stop() {
    state_ = ServiceState::STOPPED;
}

/**
 * \brief Counts HID interfaces currently present in the descriptor.
 * \return Number of active, non-suspended interface entries.
 */
uint8_t UsbManager::activeHidCount() const {
    uint8_t count = 0;
    for (const auto& entry : entries_) {
        if (entry.active && !entry.suspended) count++;
    }
    return count;
}

/**
 * \brief Checks whether another HID interface can be activated.
 * \param type Requested interface type (currently unused in check).
 * \return `true` when active-interface limit is not exceeded.
 */
bool UsbManager::canActivate(UsbHidInterface type) const {
    (void)type;
    return activeHidCount() < MAX_ACTIVE_HID;
}

/**
 * \brief Endpoints consumed by one interface spec, split by direction.
 * \param def Interface definition.
 * \return Usage (HID: 1 IN, plus 1 OUT when present; CCID: 1 IN + 1 OUT).
 */
usb_ep_usage_t UsbManager::interfaceEndpoints(const UsbInterfaceSpec& def) {
    if (def.cls == UsbInterfaceClass::Ccid) return {1, 1};
    return {1, static_cast<uint8_t>(def.hasOut ? 1 : 0)};
}

/**
 * \brief Endpoints in use per direction (CDC + non-suspended HID/CCID + MSC).
 * \return Usage split into IN and OUT endpoints.
 */
usb_ep_usage_t UsbManager::endpointsInUse() const {
    usb_ep_usage_t used = {0, 0};
    if (cdcEnabled_) {
        used.in_eps = USB_EP_BUDGET_CDC_IN;
        used.out_eps = USB_EP_BUDGET_CDC_OUT;
    }
    for (const auto& entry : entries_) {
        if (!entry.active || entry.suspended) continue;
        const usb_ep_usage_t u = interfaceEndpoints(entry.def);
        used.in_eps += u.in_eps;
        used.out_eps += u.out_eps;
    }
    if (mscActive_) {
        used.in_eps += USB_EP_BUDGET_MSC_IN;
        used.out_eps += USB_EP_BUDGET_MSC_OUT;
    }
    return used;
}

/**
 * \brief Registers a HID interface request from a module.
 * \param type HID interface slot.
 * \param moduleName Owning module name.
 * \param def Interface descriptor definition.
 * \return `true` on successful registration.
 */
bool UsbManager::registerInterface(UsbHidInterface type, const char* moduleName,
                                   const UsbInterfaceSpec& def) {
    const uint8_t idx = static_cast<uint8_t>(type);
    if (idx >= (sizeof(entries_) / sizeof(entries_[0]))) return false;

    auto& entry = entries_[idx];
    if (entry.active) {
        if (entry.owner && moduleName && strcmp(entry.owner, moduleName) == 0) {
            return true;
        }
        LOG_W(TAG, "Interface %d already owned by %s", idx, entry.owner ? entry.owner : "?");
        return false;
    }

    if (!canActivate(type)) {
        LOG_W(TAG, "HID interface limit reached (max %d)", MAX_ACTIVE_HID);
        return false;
    }

    const usb_ep_usage_t used = endpointsInUse();
    const usb_ep_usage_t need = interfaceEndpoints(def);
    if (!usb_ep_budget_fits(used, need)) {
        LOG_W(TAG, "USB endpoint budget exhausted (IN %d/%d +%d, OUT %d/%d +%d)",
              used.in_eps, USB_EP_BUDGET_MAX_IN, need.in_eps,
              used.out_eps, USB_EP_BUDGET_MAX_OUT, need.out_eps);
        return false;
    }

    entry.active = true;
    entry.owner = moduleName;
    entry.def = def;
    activeMask_ |= (1u << idx);
    LOG_I(TAG, "Registered HID interface %d for %s", idx, moduleName ? moduleName : "?");
    applyConfiguration();
    return true;
}

/**
 * \brief Unregisters a previously registered HID interface.
 * \param type HID interface slot.
 * \param moduleName Requesting module name.
 * \param apply `true` to re-enumerate immediately, `false` when the caller
 *        batches further changes and applies once.
 */
void UsbManager::unregisterInterface(UsbHidInterface type, const char* moduleName, bool apply) {
    const uint8_t idx = static_cast<uint8_t>(type);
    if (idx >= (sizeof(entries_) / sizeof(entries_[0]))) return;

    auto& entry = entries_[idx];
    if (!entry.active) return;
    if (entry.owner && moduleName && strcmp(entry.owner, moduleName) != 0) {
        LOG_W(TAG, "Interface %d owned by %s, not %s", idx,
              entry.owner ? entry.owner : "?", moduleName ? moduleName : "?");
        return;
    }

    entry.active = false;
    entry.suspended = false;
    entry.owner = nullptr;
    entry.def = {};
    activeMask_ &= ~(1u << idx);
    LOG_I(TAG, "Unregistered HID interface %d", idx);
    if (apply) applyConfiguration();
}

/**
 * \brief Temporarily removes a registered interface from the descriptor.
 * \param type Interface slot to suspend.
 * \param apply `true` to re-enumerate immediately.
 * \return `true` when the interface was active and is now suspended.
 */
bool UsbManager::suspendInterface(UsbHidInterface type, bool apply) {
    const uint8_t idx = static_cast<uint8_t>(type);
    if (idx >= (sizeof(entries_) / sizeof(entries_[0]))) return false;

    auto& entry = entries_[idx];
    if (!entry.active || entry.suspended) return false;

    entry.suspended = true;
    activeMask_ &= ~(1u << idx);
    LOG_I(TAG, "Suspended USB interface %d (owner %s)", idx, entry.owner ? entry.owner : "?");
    if (apply) applyConfiguration();
    return true;
}

/**
 * \brief Restores a previously suspended interface into the descriptor.
 * \param type Interface slot to resume.
 * \param apply `true` to re-enumerate immediately.
 * \return `true` when the interface was suspended and fits the budget again.
 */
bool UsbManager::resumeInterface(UsbHidInterface type, bool apply) {
    const uint8_t idx = static_cast<uint8_t>(type);
    if (idx >= (sizeof(entries_) / sizeof(entries_[0]))) return false;

    auto& entry = entries_[idx];
    if (!entry.active || !entry.suspended) return false;

    const usb_ep_usage_t used = endpointsInUse();
    const usb_ep_usage_t need = interfaceEndpoints(entry.def);
    if (!usb_ep_budget_fits(used, need)) {
        LOG_W(TAG, "Cannot resume interface %d: endpoint budget exhausted (IN %d/%d +%d)",
              idx, used.in_eps, USB_EP_BUDGET_MAX_IN, need.in_eps);
        return false;
    }

    entry.suspended = false;
    activeMask_ |= (1u << idx);
    LOG_I(TAG, "Resumed USB interface %d (owner %s)", idx, entry.owner ? entry.owner : "?");
    if (apply) applyConfiguration();
    return true;
}

/**
 * \brief Applies current interface set to USB HID stack.
 * \return `true` when configuration call succeeded.
 */
bool UsbManager::applyConfiguration() {
    needsReplug_ = false;
    UsbInterfaceSpec defs[3] = {};
    size_t count = 0;

    auto append_def = [&](UsbHidInterface type) {
        const auto& entry = entries_[static_cast<uint8_t>(type)];
        if (!entry.active || entry.suspended) return;
        defs[count++] = entry.def;
    };

    append_def(UsbHidInterface::Fido);
    append_def(UsbHidInterface::Keyboard);
    append_def(UsbHidInterface::Ccid);

    bool replug_needed = false;
    bool ok = usb_hid_apply_config(defs, count, &replug_needed);
    if (!ok || replug_needed) {
        needsReplug_ = true;
    }
    return ok;
}

/**
 * \brief Enables or disables the CDC serial interface with budget check.
 * \param on `true` to expose the serial console, `false` to remove it.
 * \return `true` on success; `false` if enabling would exceed the budget.
 */
bool UsbManager::setCdcEnabled(bool on) {
    if (cdcEnabled_ == on) return true;
    if (on) {
        const usb_ep_usage_t used = endpointsInUse();
        const usb_ep_usage_t need = {USB_EP_BUDGET_CDC_IN, USB_EP_BUDGET_CDC_OUT};
        if (!usb_ep_budget_fits(used, need)) {
            LOG_W(TAG, "USB endpoint budget exhausted for CDC (IN %d/%d, OUT %d/%d)",
                  used.in_eps, USB_EP_BUDGET_MAX_IN, used.out_eps, USB_EP_BUDGET_MAX_OUT);
            return false;
        }
    }
    cdcEnabled_ = on;
    LOG_I(TAG, "CDC serial %s", on ? "enabled" : "disabled");
    usb_hid_set_cdc(on);
    return true;
}

/**
 * \brief Reports whether an interface slot is suspended.
 * \param type Interface slot to query.
 * \return `true` when the slot is registered but currently suspended.
 */
bool UsbManager::isInterfaceSuspended(UsbHidInterface type) const {
    const uint8_t idx = static_cast<uint8_t>(type);
    if (idx >= (sizeof(entries_) / sizeof(entries_[0]))) return false;
    return entries_[idx].active && entries_[idx].suspended;
}

/**
 * \brief Registers the single USB Mass Storage LUN and re-enumerates.
 * \param owner Owning module name.
 * \return `true` on success; `false` if the endpoint budget is exhausted.
 */
bool UsbManager::registerMassStorage(const char* owner) {
    if (mscActive_) {
        if (mscOwner_ && owner && strcmp(mscOwner_, owner) == 0) return true;
        LOG_W(TAG, "MSC already owned by %s", mscOwner_ ? mscOwner_ : "?");
        return false;
    }
    const usb_ep_usage_t used = endpointsInUse();
    const usb_ep_usage_t need = {USB_EP_BUDGET_MSC_IN, USB_EP_BUDGET_MSC_OUT};
    if (!usb_ep_budget_fits(used, need)) {
        LOG_W(TAG, "USB endpoint budget exhausted for MSC (IN %d/%d, OUT %d/%d)",
              used.in_eps, USB_EP_BUDGET_MAX_IN, used.out_eps, USB_EP_BUDGET_MAX_OUT);
        return false;
    }
    mscActive_ = true;
    mscOwner_ = owner;
    LOG_I(TAG, "Registered MSC for %s", owner ? owner : "?");
    usb_hid_set_msc(true);
    return true;
}

/**
 * \brief Unregisters the MSC LUN and re-enumerates.
 * \param owner Owning module name.
 */
void UsbManager::unregisterMassStorage(const char* owner) {
    if (!mscActive_) return;
    if (mscOwner_ && owner && strcmp(mscOwner_, owner) != 0) {
        LOG_W(TAG, "MSC owned by %s, not %s", mscOwner_, owner ? owner : "?");
        return;
    }
    mscActive_ = false;
    mscOwner_ = nullptr;
    LOG_I(TAG, "Unregistered MSC");
    usb_hid_set_msc(false);
}

} // namespace cdc::core
