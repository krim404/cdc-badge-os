#include "cdc_core/UsbServiceManager.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/UsbManager.h"
#include "cdc_core/Raii.h"
#include "cdc_log.h"
#include <string.h>

static const char* TAG = "UsbSvcMgr";

// NVS namespace for core-service toggles (key = service id, u8 0/1).
// Module-backed services persist through ModuleRegistry instead.
static const char* USBSVC_NVS_NAMESPACE = "usbsvc";

namespace cdc::core {

UsbServiceManager& UsbServiceManager::instance() {
    static UsbServiceManager s_instance;
    return s_instance;
}

/**
 * \brief Built-in CDC service callbacks (descriptor work stays in UsbManager).
 */
static bool cdcServiceEnable() {
    return UsbManager::instance().setCdcEnabled(true);
}

static void cdcServiceDisable() {
    UsbManager::instance().setCdcEnabled(false);
}

static UsbServiceState cdcServiceState() {
    return UsbManager::instance().cdcEnabled() ? UsbServiceState::On : UsbServiceState::Off;
}

void UsbServiceManager::init() {
    UsbServiceDesc cdc = {};
    cdc.id = "cdc";
    cdc.labelKey = "core.usbsvc_cdc";
    cdc.cost = {USB_EP_BUDGET_CDC_IN, USB_EP_BUDGET_CDC_OUT};
    cdc.enable = cdcServiceEnable;
    cdc.disable = cdcServiceDisable;
    cdc.getState = cdcServiceState;
    registerService(cdc);
}

bool UsbServiceManager::registerService(const UsbServiceDesc& desc) {
    if (!desc.id || count_ >= MAX_SERVICES) return false;
    if (find(desc.id)) {
        LOG_W(TAG, "Service %s already registered", desc.id);
        return false;
    }

    services_[count_++] = desc;

    // Core services apply their persisted state right away. While the TinyUSB
    // stack is not started yet this is descriptor-only (no re-enumeration), so
    // boot ends up with a single enumeration of the final configuration.
    // Module-backed services need no action: ModuleRegistry skips disabled
    // modules at boot on its own.
    if (!desc.moduleName && desc.disable && !loadPersisted(desc.id, true)) {
        LOG_I(TAG, "Service %s persisted off, applying", desc.id);
        desc.disable();
    }
    return true;
}

bool UsbServiceManager::registerModuleService(const char* id, const char* labelKey,
                                              const char* moduleName, usb_ep_usage_t cost,
                                              const char* conflictsWith) {
    UsbServiceDesc desc = {};
    desc.id = id;
    desc.labelKey = labelKey;
    desc.cost = cost;
    desc.moduleName = moduleName;
    desc.conflictsWith = conflictsWith;
    return registerService(desc);
}

const UsbServiceDesc* UsbServiceManager::at(uint8_t idx) const {
    if (idx >= count_) return nullptr;
    return &services_[idx];
}

const UsbServiceDesc* UsbServiceManager::find(const char* id) const {
    if (!id) return nullptr;
    for (uint8_t i = 0; i < count_; i++) {
        if (strcmp(services_[i].id, id) == 0) return &services_[i];
    }
    return nullptr;
}

/**
 * \brief Resolves a module name to its ModuleRegistry index.
 * \return Index, or -1 when the module is not registered.
 */
int UsbServiceManager::moduleIndexByName(const char* moduleName) {
    auto& reg = ModuleRegistry::instance();
    for (uint8_t i = 0; i < reg.getModuleCount(); i++) {
        IModule* mod = reg.getModuleAt(i);
        if (mod && strcmp(mod->getName(), moduleName) == 0) return i;
    }
    return -1;
}

UsbServiceState UsbServiceManager::stateOf(const UsbServiceDesc& desc) const {
    if (desc.moduleName) {
        const int idx = moduleIndexByName(desc.moduleName);
        if (idx < 0) return UsbServiceState::Unavailable;
        auto& reg = ModuleRegistry::instance();
        if (!reg.isModuleEnabled(static_cast<uint8_t>(idx))) return UsbServiceState::Off;
        IModule* mod = reg.getModuleAt(static_cast<uint8_t>(idx));
        if (mod && mod->getState() == ServiceState::STARTED) return UsbServiceState::On;
        return UsbServiceState::Unavailable;
    }
    return desc.getState ? desc.getState() : UsbServiceState::Unavailable;
}

UsbServiceState UsbServiceManager::state(uint8_t idx) const {
    const UsbServiceDesc* desc = at(idx);
    return desc ? stateOf(*desc) : UsbServiceState::Unavailable;
}

UsbServiceManager::ToggleResult UsbServiceManager::toggleModuleService(const UsbServiceDesc& desc,
                                                                       bool enabled) {
    const int idx = moduleIndexByName(desc.moduleName);
    if (idx < 0) return ToggleResult::Failed;
    const uint8_t midx = static_cast<uint8_t>(idx);
    auto& reg = ModuleRegistry::instance();

    if (!enabled) {
        IModule* mod = reg.getModuleAt(midx);
        if (mod && mod->getState() == ServiceState::STARTED) {
            mod->stop();
        }
        reg.setModuleEnabled(midx, false);
        return ToggleResult::Ok;
    }

    reg.setModuleEnabled(midx, true);
    if (reg.startModule(midx)) return ToggleResult::Ok;

    // Keep the user's Off state authoritative when the start failed, so the
    // next boot does not retry a configuration that cannot come up.
    reg.setModuleEnabled(midx, false);
    switch (reg.classifyStartFailure(midx)) {
        case ModuleStartFailure::UsbBudgetFull:
            return ToggleResult::BudgetFull;
        case ModuleStartFailure::SlotError:
        case ModuleStartFailure::Generic:
        default:
            return ToggleResult::Failed;
    }
}

UsbServiceManager::ToggleResult UsbServiceManager::toggleCoreService(const UsbServiceDesc& desc,
                                                                     bool enabled) {
    if (enabled) {
        if (!desc.enable) return ToggleResult::Failed;
        if (!desc.enable()) {
            // The callback path enforces the budget in UsbManager; classify
            // after the fact so the UI can distinguish "does not fit" from
            // a genuine failure.
            const usb_ep_usage_t used = UsbManager::instance().endpointUsage();
            if (!usb_ep_budget_fits(used, desc.cost)) return ToggleResult::BudgetFull;
            return ToggleResult::Failed;
        }
    } else {
        if (!desc.disable) return ToggleResult::Failed;
        desc.disable();
    }
    persist(desc.id, enabled);
    return ToggleResult::Ok;
}

UsbServiceManager::ToggleResult UsbServiceManager::setEnabled(const char* id, bool enabled) {
    const UsbServiceDesc* desc = find(id);
    if (!desc) return ToggleResult::NotFound;

    const UsbServiceState current = stateOf(*desc);
    if (current == UsbServiceState::Suspended) return ToggleResult::Busy;
    if (enabled && current == UsbServiceState::On) return ToggleResult::Ok;
    if (!enabled && current == UsbServiceState::Off) return ToggleResult::Ok;

    // A service sharing the same USB slot must be off before enabling this one
    // (e.g. USB keyboard and OTP HID both claim the Keyboard slot).
    if (enabled && desc->conflictsWith) {
        const UsbServiceDesc* other = find(desc->conflictsWith);
        if (other && stateOf(*other) == UsbServiceState::On) return ToggleResult::SlotBusy;
    }

    return desc->moduleName ? toggleModuleService(*desc, enabled)
                            : toggleCoreService(*desc, enabled);
}

usb_ep_usage_t UsbServiceManager::usage() const {
    return UsbManager::instance().endpointUsage();
}

bool UsbServiceManager::loadPersisted(const char* id, bool defaultOn) const {
    NvsScope nvs(USBSVC_NVS_NAMESPACE, NVS_READONLY);
    if (!nvs) return defaultOn;
    uint8_t value = defaultOn ? 1 : 0;
    if (nvs_get_u8(nvs, id, &value) != ESP_OK) return defaultOn;
    return value != 0;
}

void UsbServiceManager::persist(const char* id, bool on) {
    NvsScope nvs(USBSVC_NVS_NAMESPACE, NVS_READWRITE);
    if (!nvs) {
        LOG_W(TAG, "Cannot persist %s=%d (NVS open failed)", id, on ? 1 : 0);
        return;
    }
    nvs_set_u8(nvs, id, on ? 1 : 0);
    if (nvs.commit() != ESP_OK) {
        LOG_W(TAG, "Cannot persist %s=%d (commit failed)", id, on ? 1 : 0);
    }
}

} // namespace cdc::core
