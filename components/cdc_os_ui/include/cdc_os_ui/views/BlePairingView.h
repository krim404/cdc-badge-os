#pragma once

#include "cdc_ui/IView.h"
#include <cstdint>

namespace cdc::ui {

/**
 * \brief Discoverable/pairing-mode screen.
 *
 * Makes the badge discoverable so a host can initiate BLE bonding. While shown
 * it puts the keyboard provider into its discoverable state (advertising the HID
 * profile), holds a sleep inhibitor, and keeps the inactivity timer reset so the
 * central numeric-comparison prompt is shown instead of being rejected on lock.
 *
 * Keys:
 *   N = Leave pairing mode
 */
class BlePairingView : public ViewBase {
public:
    void onEnter(void* context) override;
    void onExit() override;
    void onTick(uint32_t nowMs) override;
    void render(bool partial) override;
    InputResult onKey(char key) override;
    const char* getName() const override { return "BlePairingView"; }

private:
    uint32_t lastUpdate_ = 0;
    bool lastConnected_ = false;
};

} // namespace cdc::ui
