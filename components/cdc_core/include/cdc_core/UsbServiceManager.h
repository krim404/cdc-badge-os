#pragma once

#include "usb_badge/usb_endpoint_budget.h"
#include <cstdint>

namespace cdc::core {

/**
 * \brief User-visible state of a toggleable USB service.
 */
enum class UsbServiceState : uint8_t {
    Off,          ///< Disabled by the user.
    On,           ///< Enabled and part of the USB descriptor.
    Suspended,    ///< Registered but endpoint borrowed (e.g. CCID during autotype).
    Unavailable,  ///< Enabled but not operational (e.g. no applet module active).
};

/**
 * \brief Descriptor for one toggleable USB service.
 *
 * Two kinds of services register here:
 * - Core services (CDC, CCID) provide enable/disable/getState callbacks and
 *   are persisted in the manager's own NVS namespace ("usbsvc").
 * - Module-backed services set moduleName instead; toggle and persistence are
 *   delegated to ModuleRegistry (single source of truth: "modules"/"disabled").
 */
struct UsbServiceDesc {
    const char* id = nullptr;        ///< Stable lowercase id ("cdc", "ccid", "kbd", ...).
    const char* labelKey = nullptr;  ///< i18n key for the display name.
    usb_ep_usage_t cost = {};        ///< Endpoint demand when active (display + prediction).

    // Core-service callbacks; all nullptr for module-backed services.
    bool (*enable)() = nullptr;              ///< May re-enumerate; false on failure.
    void (*disable)() = nullptr;
    UsbServiceState (*getState)() = nullptr;

    const char* moduleName = nullptr;    ///< Non-null: delegate to ModuleRegistry.
    const char* conflictsWith = nullptr; ///< Service id sharing the same USB slot.
};

/**
 * UsbServiceManager - user-facing on/off registry for USB services
 *
 * Sits above UsbManager: never touches descriptors itself, only invokes the
 * registered callbacks (core services) or ModuleRegistry start/stop
 * (module-backed services). Endpoint budget enforcement stays in UsbManager;
 * this class classifies failures for the UI and persists user intent.
 */
class UsbServiceManager {
public:
    static constexpr uint8_t MAX_SERVICES = 8;

    /**
     * \brief Outcome of a setEnabled() request, for UI/serial feedback.
     */
    enum class ToggleResult : uint8_t {
        Ok,
        BudgetFull,  ///< USB endpoint budget exhausted.
        SlotBusy,    ///< Conflicting service holds the shared USB slot.
        Busy,        ///< Service is suspended (endpoint borrowed); retry later.
        Failed,      ///< Enable/disable callback or module start failed.
        NotFound,    ///< Unknown service id.
    };

    static UsbServiceManager& instance();

    /**
     * \brief Loads persisted state and registers the built-in "cdc" service.
     *
     * Call from main before module initializers run and before the TinyUSB
     * stack starts, so persisted-off services never enter the first descriptor.
     */
    void init();

    /**
     * \brief Registers a core service and applies its persisted state.
     * \param desc Service descriptor (strings must remain valid).
     * \return `true` when registered; `false` when full or id duplicated.
     */
    bool registerService(const UsbServiceDesc& desc);

    /**
     * \brief Registers a module-backed service (toggle via ModuleRegistry).
     * \param id Stable service id.
     * \param labelKey i18n key for the display name.
     * \param moduleName Registered module name (e.g. "mod_usbhid").
     * \param cost Endpoint demand when active.
     * \param conflictsWith Optional id of a service sharing the same USB slot.
     * \return `true` when registered; `false` when full or id duplicated.
     */
    bool registerModuleService(const char* id, const char* labelKey,
                               const char* moduleName, usb_ep_usage_t cost,
                               const char* conflictsWith = nullptr);

    uint8_t count() const { return count_; }
    const UsbServiceDesc* at(uint8_t idx) const;
    const UsbServiceDesc* find(const char* id) const;

    /**
     * \brief Current state of the service at \p idx.
     */
    UsbServiceState state(uint8_t idx) const;

    /**
     * \brief Enables or disables a service and persists the outcome on success.
     * \param id Service id.
     * \param enabled Desired state.
     * \return Classified result for UI/serial feedback.
     */
    ToggleResult setEnabled(const char* id, bool enabled);

    /**
     * \brief Current endpoint usage per direction (forwarded from UsbManager).
     */
    usb_ep_usage_t usage() const;

private:
    UsbServiceManager() = default;

    UsbServiceState stateOf(const UsbServiceDesc& desc) const;
    ToggleResult toggleModuleService(const UsbServiceDesc& desc, bool enabled);
    ToggleResult toggleCoreService(const UsbServiceDesc& desc, bool enabled);
    static int moduleIndexByName(const char* moduleName);

    bool loadPersisted(const char* id, bool defaultOn) const;
    void persist(const char* id, bool on);

    UsbServiceDesc services_[MAX_SERVICES] = {};
    uint8_t count_ = 0;
};

} // namespace cdc::core
