#include "mod_piv/PivModule.h"
#include "piv_defs.h"
#include "piv_keys.h"

#include "cdc_core/ModuleRegistry.h"
#include "cdc_scard/applet.h"
#include "cdc_scard/scard_usb.h"
#include "cdc_log.h"

static const char* TAG = "mod_piv";

namespace cdc::mod_piv {

PivModule& PivModule::instance() {
    static PivModule inst;
    return inst;
}

bool PivModule::init() {
    LOG_I(TAG, "Initializing PIV module");
    core::ModuleRegistry::instance().registerModule(this);

    if (!slotRange_.hasEcc || !slotRange_.hasRmem) {
        core::ModuleRegistry::instance().reportModuleError(getName(), "PIV slot range missing");
        state_ = core::ServiceState::ERROR;
        return false;
    }
    if (!piv_init(slotRange_)) {
        core::ModuleRegistry::instance().reportModuleError(getName(), "PIV backend init failed");
        state_ = core::ServiceState::ERROR;
        return false;
    }
    core::ModuleRegistry::instance().clearModuleErrorByName(getName());
    state_ = core::ServiceState::INITIALIZED;
    return true;
}

bool PivModule::start() {
    if (state_ != core::ServiceState::INITIALIZED && state_ != core::ServiceState::STOPPED) {
        return false;
    }
    // The CCID interface may be unavailable (service disabled or USB budget
    // exhausted). The module still starts; the applet simply stays inactive
    // until the interface comes back.
    usbAcquired_ = scard_usb_acquire();
    if (!usbAcquired_) {
        LOG_W(TAG, "CCID interface unavailable, PIV applet inactive");
    } else if (!scard_register_applet(piv_applet(), false)) {
        core::ModuleRegistry::instance().reportModuleError(getName(), "PIV applet registration failed");
    }
    state_ = core::ServiceState::STARTED;
    return true;
}

void PivModule::stop() {
    if (usbAcquired_) {
        scard_unregister_applet("piv");
        scard_usb_release();
        usbAcquired_ = false;
    }
    core::ModuleBase::stop();
}

core::IModule::SlotRequest PivModule::getSlotRequest() const {
    core::IModule::SlotRequest req = {};
    req.mapName = getName();
    req.minEccSlots = 4;   // 9A, 9C, (reserved), 9E
    req.minRmemSlots = 4;  // piv_state, reserved, 9D key, reserved
    return req;
}

void PivModule::setSlotRange(const core::IModule::SlotRange& range) {
    slotRange_ = range;
}

} // namespace cdc::mod_piv

extern "C" void mod_piv_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        cdc::mod_piv::PivModule::instance().init();
    });
}
