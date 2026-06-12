#pragma once

#include "cdc_core/ModuleBase.h"

struct cJSON;

namespace cdc::mod_password {

class PasswordModule : public core::ModuleBase {
public:
    bool init() override;
    void stop() override;

    const char* getVersion() const override { return "1.0"; }
    uint8_t getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) override;
    core::IModule::SlotRequest getSlotRequest() const override;
    void setSlotRange(const core::IModule::SlotRange& range) override;

    bool exportBackup(cJSON* out) override;
    core::IModule::BackupResult importBackup(const cJSON* in) override;

    static PasswordModule& instance();

private:
    PasswordModule() : ModuleBase("mod_password") {}
    core::IModule::SlotRange slotRange_ = {};
};

} // namespace cdc::mod_password

extern "C" void mod_password_register();
