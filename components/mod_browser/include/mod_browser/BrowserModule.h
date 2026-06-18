#pragma once

#include "cdc_core/ModuleBase.h"

namespace cdc::mod_browser {

/**
 * \brief Text-stripper web browser module, surfaced under the Tools menu.
 */
class BrowserModule : public core::ModuleBase {
public:
    static BrowserModule& instance();

    const char* getVersion() const override { return "1.0.0"; }

    bool init() override;
    bool start() override;

    uint8_t getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) override;
    void onTick(uint32_t nowMs) override;

private:
    BrowserModule() : core::ModuleBase("mod_browser") {}
};

}  // namespace cdc::mod_browser

extern "C" void mod_browser_register();
