#pragma once

#include "cdc_ui/IView.h"
#include <cstdint>

namespace cdc::ui {

/**
 * BLE pairing numeric-comparison prompt view.
 *
 * Shown when a remote peer requests Numeric Comparison pairing. Displays the
 * 6-digit code that the host shows and waits for the user to accept (Y) or
 * reject (N). On timeout, the prompt rejects automatically.
 *
 * Keys:
 *   Y = Accept pairing (sends respondToNumericComparison(handle, true))
 *   N = Reject pairing
 */
class BlePairingPromptView : public ViewBase {
public:
    /**
     * Configure the prompt and reset timeout.
     * \param connHandle BLE connection handle being paired.
     * \param passkey Six-digit confirmation code shown by the remote host.
     * \param timeoutMs Timeout in milliseconds before automatic rejection.
     */
    void prepare(uint16_t connHandle, uint32_t passkey, uint32_t timeoutMs = 30000);

    /**
     * Install an accept handler used when the badge is locked. When set, pressing
     * Y dismisses the prompt and invokes the handler (which drives PIN-unlock)
     * instead of accepting the pairing directly. Cleared by prepare().
     */
    void setOnLockedAccept(void (*cb)(void*), void* userData) {
        onLockedAccept_ = cb;
        lockedAcceptUd_ = userData;
    }

    // IView
    void render(bool partial) override;
    InputResult onKey(char key) override;
    void onTick(uint32_t nowMs) override;
    void onEnter(void* context) override;
    const char* getName() const override { return "BlePairingPromptView"; }
    const char* getFooterHint() const override { return "[Y] OK  [N] Cancel"; }

private:
    uint16_t connHandle_ = 0xFFFF;
    uint32_t passkey_ = 0;
    uint32_t timeoutMs_ = 30000;
    uint32_t enteredAtMs_ = 0;
    bool responded_ = false;
    void (*onLockedAccept_)(void*) = nullptr;
    void* lockedAcceptUd_ = nullptr;

    void respond(bool accept);
};

} // namespace cdc::ui
