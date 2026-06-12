#pragma once

#include "cdc_core/ModuleBase.h"
#include "cdc_core/IChallengeResponder.h"

namespace cdc::mod_2fa {

class TwoFaModule : public core::ModuleBase, public core::IChallengeResponder {
public:
    bool init() override;
    bool start() override;
    void stop() override;
    void onTick(uint32_t nowMs) override;

    const char* getVersion() const override { return "1.0"; }
    uint8_t getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) override;
    core::IModule::SlotRequest getSlotRequest() const override;
    void setSlotRange(const core::IModule::SlotRange& range) override;

    bool exportBackup(cJSON* out) override;
    core::IModule::BackupResult importBackup(const cJSON* in) override;

    // IChallengeResponder
    int challengeResponse(const char* entryName, const uint8_t* challenge,
                          size_t clen, uint8_t* out) override;
    int challengeResponseUsbSlot(const uint8_t* challenge, size_t clen,
                                 uint8_t* out, bool* touchRequiredOut) override;

    static TwoFaModule& instance();

private:
    TwoFaModule() : ModuleBase("mod_2fa") {}
    core::IModule::SlotRange slotRange_ = {};
};

} // namespace cdc::mod_2fa

extern "C" void mod_2fa_register();
