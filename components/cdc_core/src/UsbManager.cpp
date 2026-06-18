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
 * \brief Counts currently active HID interfaces.
 * \return Number of active interface entries.
 */
uint8_t UsbManager::activeHidCount() const {
    uint8_t count = 0;
    for (const auto& entry : entries_) {
        if (entry.active) count++;
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
 * \brief Endpoints consumed by one interface spec.
 * \param def Interface definition.
 * \return Endpoint count (HID: 1, or 2 with an OUT endpoint; CCID: 2).
 */
uint8_t UsbManager::interfaceEndpoints(const UsbInterfaceSpec& def) {
    if (def.cls == UsbInterfaceClass::Ccid) return 2;
    return def.hasOut ? 2 : 1;
}

/**
 * \brief Total endpoints in use (CDC + active HID/CCID + MSC).
 * \return Endpoint count.
 */
uint8_t UsbManager::endpointsInUse() const {
    uint8_t n = CDC_ENDPOINTS;
    for (const auto& entry : entries_) {
        if (entry.active) n += interfaceEndpoints(entry.def);
    }
    if (mscActive_) n += MSC_ENDPOINTS;
    return n;
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

    if (endpointsInUse() + interfaceEndpoints(def) > MAX_ENDPOINTS) {
        LOG_W(TAG, "USB endpoint budget exhausted (have %d/%d, +%d)",
              endpointsInUse(), MAX_ENDPOINTS, interfaceEndpoints(def));
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
 */
void UsbManager::unregisterInterface(UsbHidInterface type, const char* moduleName) {
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
    entry.owner = nullptr;
    entry.def = {};
    activeMask_ &= ~(1u << idx);
    LOG_I(TAG, "Unregistered HID interface %d", idx);
    applyConfiguration();
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
        if (!entry.active) return;
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
    if (endpointsInUse() + MSC_ENDPOINTS > MAX_ENDPOINTS) {
        LOG_W(TAG, "USB endpoint budget exhausted for MSC (have %d/%d)",
              endpointsInUse(), MAX_ENDPOINTS);
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
