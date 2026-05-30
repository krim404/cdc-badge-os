/**
 * \file VfatModule.h
 * \brief Base module exposing the plugins FAT partition for inspection.
 *
 * Contributes a GUI file explorer under Tools -> Expert and a stateful "VFAT"
 * serial shell (LIST/CD/PWD/GET/PUT/DELETE/MKDIR/RMDIR/FREE). Not a plugin: it
 * has full (unsandboxed) access to the partition.
 */

#pragma once

#include "cdc_core/IModule.h"

namespace cdc::mod_vfat {

class VfatModule : public cdc::core::IModule {
public:
    static VfatModule& instance();

    const char* getName() const override { return "mod_vfat"; }
    const char* getVersion() const override { return "1.0.0"; }
    cdc::core::ServiceState getState() const override { return state_; }

    bool init() override;
    bool start() override;
    void stop() override;

    uint8_t getMenuItems(cdc::core::ModuleMenuItem* items, uint8_t maxItems) override;

private:
    VfatModule() = default;
    cdc::core::ServiceState state_ = cdc::core::ServiceState::UNINITIALIZED;
};

}  // namespace cdc::mod_vfat

extern "C" void mod_vfat_register(void);
