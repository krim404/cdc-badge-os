#pragma once

#include "cdc_core/IModule.h"

namespace cdc::mod_msc {

/**
 * USB Mass Storage Module
 *
 * Default-disabled module that exposes the badge's vfat volume to a connected
 * host as a removable USB drive. On start() it installs the MSC block backend
 * (backed by PluginStorage) and registers the LUN with UsbManager, which adds
 * the MSC interface and re-enumerates; on stop() it removes both and the drive
 * disappears. Competes for the USB endpoint budget, so start() may fail cleanly
 * when other USB services already consume the endpoints.
 */
class UsbMscModule : public core::IModule {
public:
    const char* getName() const override { return "mod_msc"; }
    core::ServiceState getState() const override { return state_; }
    bool init() override;
    bool start() override;
    void stop() override;
    void onTick(uint32_t nowMs) override;

    const char* getVersion() const override { return "1.0"; }
    uint8_t getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) override;

    static UsbMscModule& instance();

private:
    UsbMscModule() = default;
    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;
};

} // namespace cdc::mod_msc

extern "C" void mod_msc_register();
