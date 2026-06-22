/**
 * \file
 * \brief BLE pairing numeric-comparison prompt view implementation.
 */

#include "cdc_os_ui/views/BlePairingPromptView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_log.h"

#include <goodisplay/gdey029T94.h>
#include <cstdio>

static const char* TAG = "BLE_PAIR_UI";

namespace cdc::ui {

void BlePairingPromptView::prepare(uint16_t connHandle, uint32_t passkey,
                                    uint32_t timeoutMs) {
    connHandle_ = connHandle;
    passkey_ = passkey;
    timeoutMs_ = timeoutMs;
    responded_ = false;
    enteredAtMs_ = 0;
    onLockedAccept_ = nullptr;
    lockedAcceptUd_ = nullptr;
    dirty_ = true;
}

void BlePairingPromptView::onEnter(void* context) {
    (void)context;
    dirty_ = true;
}

void BlePairingPromptView::onTick(uint32_t nowMs) {
    if (responded_) return;
    if (enteredAtMs_ == 0) {
        enteredAtMs_ = nowMs;
        return;
    }
    if (nowMs - enteredAtMs_ >= timeoutMs_) {
        LOG_W(TAG, "Pairing prompt timed out, rejecting");
        respond(false);
    }
}

InputResult BlePairingPromptView::onKey(char key) {
    if (responded_) return InputResult::CONSUMED;

    if (key == KEY_YES) {
        if (onLockedAccept_) {
            // Locked: do not pair yet. Dismiss the prompt and hand off to the
            // PIN-unlock flow; the pairing is accepted only once the PIN succeeds.
            responded_ = true;
            ViewStack::instance().hideModal();
            onLockedAccept_(lockedAcceptUd_);
        } else {
            respond(true);
        }
        return InputResult::CONSUMED;
    }
    if (key == KEY_NO) {
        respond(false);
        return InputResult::CONSUMED;
    }
    return InputResult::IGNORED;
}

void BlePairingPromptView::respond(bool accept) {
    responded_ = true;
    auto* ble = hal::getBluetoothControllerInstance();
    if (ble) {
        ble->respondToNumericComparison(connHandle_, accept);
    }
    LOG_I(TAG, "Pairing %s by user", accept ? "accepted" : "rejected");
    ViewStack::instance().hideModal();
}

void BlePairingPromptView::render(bool partial) {
    (void)partial;

    auto* display = hal::getDisplayInstance();
    if (!display) return;
    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;

    constexpr int BOX_W = 240;
    constexpr int BOX_H = 96;
    const int boxX = (display->getWidth() - BOX_W) / 2;
    const int boxY = (display->getHeight() - BOX_H) / 2;

    render::drawDialogFrame(gfx, boxX, boxY, BOX_W, BOX_H);

    gfx->setTextColor(EPD_BLACK);
    gfx->setTextSize(1);

    gfx->setCursor(boxX + 14, boxY + 14);
    gfx->print("BLE Pairing Request");

    char codeBuf[16];
    snprintf(codeBuf, sizeof(codeBuf), "%03lu %03lu",
             (unsigned long)(passkey_ / 1000), (unsigned long)(passkey_ % 1000));

    gfx->setTextSize(2);
    gfx->setCursor(boxX + 48, boxY + 36);
    gfx->print(codeBuf);

    gfx->setTextSize(1);
    gfx->setCursor(boxX + 14, boxY + BOX_H - 16);
    gfx->print("[Y] Accept   [N] Reject");

    clearDirty();
}

} // namespace cdc::ui
