#pragma once

#include "cdc_core/ModuleBase.h"

namespace cdc::mod_sao {

class SaoModule : public core::ModuleBase {
public:
    static SaoModule& instance();

    const char* getVersion() const override { return "1.0.0"; }

    bool init() override;
    bool start() override;

    uint8_t getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) override;
    void onUnlock() override;

private:
    SaoModule() : core::ModuleBase("mod_sao") {}
};

} // namespace cdc::mod_sao

extern "C" void mod_sao_register();
