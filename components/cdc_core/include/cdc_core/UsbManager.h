#pragma once

#include "cdc_core/IService.h"
#include "usb_badge/usb_endpoint_budget.h"
#include <cstdint>

namespace cdc::core {

enum class UsbHidInterface : uint8_t {
    Fido = 0,
    Keyboard = 1,
    Ccid = 2,
};

enum class UsbInterfaceClass : uint8_t {
    Hid = 0,
    Ccid = 1,
};

struct UsbHidCallbacks {
    uint16_t (*onGetReport)(uint8_t report_id, uint8_t report_type,
                            uint8_t* buffer, uint16_t reqlen) = nullptr;
    void (*onSetReport)(uint8_t report_id, uint8_t report_type,
                        uint8_t const* buffer, uint16_t bufsize) = nullptr;
    void (*onReportComplete)(uint8_t const* report, uint16_t len) = nullptr;
};

struct UsbInterfaceSpec {
    UsbInterfaceClass cls = UsbInterfaceClass::Hid;
    const char* name = nullptr;  // Interface name for USB descriptor
    const uint8_t* reportDesc = nullptr;
    uint16_t reportDescLen = 0;
    uint8_t protocol = 0;  // HID protocol (0=none, 1=keyboard)
    bool hasOut = false;
    uint16_t epInSize = 64;
    uint16_t epOutSize = 64;
    UsbHidCallbacks callbacks = {};

    // Optional device-descriptor VID/PID hint. When non-zero, an active
    // interface carrying these values overrides the default device VID/PID so
    // the host recognizes the composite as a specific product (e.g. mod_otphid
    // presents OnlyKey's KeePassXC-whitelisted IDs). 0 means "no preference".
    uint16_t preferredVid = 0;
    uint16_t preferredPid = 0;
};

/**
 * UsbManager - HID interface arbitration
 *
 * CDC is enabled by default (handled by usb_badge) but can be toggled via
 * setCdcEnabled() to free its endpoints. HID interfaces are optional and
 * registered by modules at runtime.
 */
class UsbManager : public IService {
public:
    static UsbManager& instance();

    const char* getName() const override { return "UsbManager"; }

    bool init() override;
    bool start() override;
    void stop() override;
    ServiceState getState() const override { return state_; }

    bool registerInterface(UsbHidInterface type, const char* moduleName,
                           const UsbInterfaceSpec& def);
    void unregisterInterface(UsbHidInterface type, const char* moduleName,
                             bool apply = true);

    /**
     * \brief Temporarily removes a registered interface from the descriptor.
     *
     * The registration (owner and spec) is kept so resumeInterface() can
     * restore it; only the endpoint budget and the USB configuration change.
     * Used to free an IN endpoint for a short-lived interface (e.g. the USB
     * keyboard borrowing the CCID slot during autotype).
     * \param type Interface slot to suspend.
     * \param apply `true` to re-enumerate immediately, `false` when the caller
     *        batches further changes and applies once.
     * \return `true` when the interface was active and is now suspended.
     */
    bool suspendInterface(UsbHidInterface type, bool apply = true);

    /**
     * \brief Restores a previously suspended interface into the descriptor.
     * \param type Interface slot to resume.
     * \param apply `true` to re-enumerate immediately.
     * \return `true` when the interface was suspended and fits the budget again.
     */
    bool resumeInterface(UsbHidInterface type, bool apply = true);

    uint8_t activeInterfaceMask() const { return activeMask_; }
    bool applyConfiguration();
    bool needsReplug() const { return needsReplug_; }

    /**
     * \brief Registers the single USB Mass Storage LUN and re-enumerates.
     * \param owner Owning module name.
     * \return `true` on success; `false` if the USB endpoint budget is exhausted.
     */
    bool registerMassStorage(const char* owner);

    /**
     * \brief Unregisters the MSC LUN and re-enumerates (drive disappears).
     * \param owner Owning module name.
     */
    void unregisterMassStorage(const char* owner);

    /**
     * \brief Reports whether the MSC LUN is currently active.
     * \return `true` when the MSC interface is part of the descriptor.
     */
    bool massStorageActive() const { return mscActive_; }

    /**
     * \brief Reports whether the HID interface budget is exhausted.
     * \return `true` when active HID interfaces reached MAX_ACTIVE_HID.
     */
    bool hidSlotsFull() const { return activeHidCount() >= MAX_ACTIVE_HID; }

    /**
     * \brief Enables or disables the CDC serial interface.
     *
     * Disabling frees the CDC endpoints (2 IN / 1 OUT) for other services and
     * re-enumerates immediately. Enabling checks the endpoint budget first.
     * \param on `true` to expose the serial console, `false` to remove it.
     * \return `true` on success; `false` if enabling would exceed the budget.
     */
    bool setCdcEnabled(bool on);

    /**
     * \brief Reports whether the CDC serial interface is part of the descriptor.
     * \return `true` when CDC is enabled.
     */
    bool cdcEnabled() const { return cdcEnabled_; }

    /**
     * \brief Reports whether an interface slot is suspended (endpoint borrowed).
     * \param type Interface slot to query.
     * \return `true` when the slot is registered but currently suspended.
     */
    bool isInterfaceSuspended(UsbHidInterface type) const;

    /**
     * \brief Current endpoint usage per direction, for display and prediction.
     * \return Usage split into IN and OUT endpoints.
     */
    usb_ep_usage_t endpointUsage() const { return endpointsInUse(); }

    /**
     * \brief Tests whether a toggle newly introduced a replug requirement.
     *
     * Callers snapshot needsReplug() before performing a module toggle and pass
     * it here afterwards; returns `true` only on a false-to-true transition.
     * \param wasNeededBefore needsReplug() value captured before the toggle.
     * \return `true` if a replug is now required but was not before.
     */
    bool newlyRequiresReplug(bool wasNeededBefore) const {
        return !wasNeededBefore && needsReplug_;
    }

private:
    struct InterfaceEntry {
        bool active = false;
        bool suspended = false;
        const char* owner = nullptr;
        UsbInterfaceSpec def = {};
    };

    uint8_t activeHidCount() const;
    bool canActivate(UsbHidInterface type) const;

    // Endpoints consumed by CDC plus non-suspended HID/CCID and (if active) MSC,
    // split by direction. The ESP32-S3 limits live in usb_endpoint_budget.h;
    // the IN direction (4 non-control endpoints) is the binding constraint.
    usb_ep_usage_t endpointsInUse() const;
    static usb_ep_usage_t interfaceEndpoints(const UsbInterfaceSpec& def);

    static constexpr uint8_t MAX_ACTIVE_HID = 2;

    ServiceState state_ = ServiceState::STOPPED;
    uint8_t activeMask_ = 0;
    bool needsReplug_ = false;
    bool cdcEnabled_ = true;
    bool mscActive_ = false;
    const char* mscOwner_ = nullptr;
    InterfaceEntry entries_[3] = {};
};

} // namespace cdc::core
