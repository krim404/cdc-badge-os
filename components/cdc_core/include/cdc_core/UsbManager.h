#pragma once

#include "cdc_core/IService.h"
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
 * CDC is always enabled (handled by usb_badge). HID interfaces are optional and
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
    void unregisterInterface(UsbHidInterface type, const char* moduleName);

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
        const char* owner = nullptr;
        UsbInterfaceSpec def = {};
    };

    uint8_t activeHidCount() const;
    bool canActivate(UsbHidInterface type) const;

    // Endpoints currently consumed by CDC plus active HID/CCID and (if active) MSC.
    uint8_t endpointsInUse() const;
    static uint8_t interfaceEndpoints(const UsbInterfaceSpec& def);

    static constexpr uint8_t MAX_ACTIVE_HID = 2;
    // Mirrors CFG_TUD_ENDPOINT_MAX in include/tusb_config.h. CDC always uses 3
    // endpoints (notif/out/in); MSC needs 1 bulk IN + 1 bulk OUT.
    static constexpr uint8_t MAX_ENDPOINTS = 8;
    static constexpr uint8_t CDC_ENDPOINTS = 3;
    static constexpr uint8_t MSC_ENDPOINTS = 2;

    ServiceState state_ = ServiceState::STOPPED;
    uint8_t activeMask_ = 0;
    bool needsReplug_ = false;
    bool mscActive_ = false;
    const char* mscOwner_ = nullptr;
    InterfaceEntry entries_[3] = {};
};

} // namespace cdc::core
