#pragma once

#include "cdc_core/ModuleBase.h"

namespace cdc::mod_piv {

/**
 * \brief PIV smart-card module (NIST SP 800-73-4).
 *
 * Registers the PIV applet on the shared CCID interface. Storage lives in the
 * TROPIC01 secure element (keys 9A/9C/9E), a software P-256 key for 9D, and
 * NVS for certificates. Independent of any other module; degrades gracefully
 * when the CCID USB interface is unavailable.
 */
class PivModule : public core::ModuleBase {
public:
    PivModule() : ModuleBase("mod_piv") {}

    const char* getVersion() const override { return "1.0"; }
    bool init() override;
    bool start() override;
    void stop() override;

    core::IModule::SlotRequest getSlotRequest() const override;
    void setSlotRange(const core::IModule::SlotRange& range) override;

    static PivModule& instance();

private:
    core::IModule::SlotRange slotRange_ = {};
    bool usbAcquired_ = false;
};

} // namespace cdc::mod_piv

extern "C" void mod_piv_register();
