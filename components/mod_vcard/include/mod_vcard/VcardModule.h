#pragma once

#include "cdc_core/IModule.h"

struct cJSON;

namespace cdc::mod_vcard {

class VcardModule : public core::IModule {
public:
    static VcardModule& instance();

    const char* getName() const override { return "mod_vcard"; }
    const char* getVersion() const override { return "1.0.0"; }
    core::ServiceState getState() const override { return state_; }

    bool init() override;
    bool start() override;
    void stop() override;

    uint8_t getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) override;
    uint8_t getLockScreenContextItems(core::LockScreenContextItem* items, uint8_t maxItems) override;
    void onTick(uint32_t nowMs) override;

    bool exportBackup(cJSON* out) override;
    core::IModule::BackupResult importBackup(const cJSON* in) override;

private:
    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;
};

} // namespace cdc::mod_vcard

extern "C" void mod_vcard_register();
