/**
 * \file
 * \brief Discoverable/pairing-mode screen implementation.
 */

#include "cdc_os_ui/views/BlePairingView.h"
#include "cdc_os_ui/SleepManager.h"
#include "cdc_core/IKeyboardProvider.h"
#include "cdc_hal/IBluetoothController.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_ui/I18n.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/RenderHelpers.h"

#include <goodisplay/gdey029T94.h>

static constexpr const char* kSleepInhibitor = "ble_pair";

namespace cdc::ui {

void BlePairingView::onEnter(void* context) {
    (void)context;
    auto* ble = hal::getBluetoothControllerInstance();
    if (ble && !ble->isEnabled()) ble->enable();
    if (auto* kb = core::getKeyboard()) kb->setDiscoverable(true);
    SleepManager::instance().addSleepInhibitor(kSleepInhibitor);
    lastConnected_ = ble && ble->isConnected();
    lastUpdate_ = 0;
    dirty_ = true;
}

void BlePairingView::onExit() {
    if (auto* kb = core::getKeyboard()) kb->setDiscoverable(false);
    SleepManager::instance().removeSleepInhibitor(kSleepInhibitor);
}

void BlePairingView::onTick(uint32_t nowMs) {
    // Keep the badge awake and unlocked while discoverable so the central
    // numeric-comparison prompt is shown instead of being rejected on lock.
    ViewStack::instance().resetInactivityTimer();

    if (nowMs - lastUpdate_ >= 1000) {
        lastUpdate_ = nowMs;
        auto* ble = hal::getBluetoothControllerInstance();
        bool connected = ble && ble->isConnected();
        if (connected != lastConnected_) {
            lastConnected_ = connected;
            markDirty();
        }
    }
}

InputResult BlePairingView::onKey(char key) {
    if (key == KEY_NO) return InputResult::REQUEST_POP;
    return InputResult::IGNORED;
}

void BlePairingView::render(bool partial) {
    auto* display = hal::getDisplayInstance();
    if (!display) return;
    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;

    const uint16_t width = display->getWidth();
    const uint16_t height = display->getHeight();

    if (!partial) gfx->fillScreen(EPD_WHITE);

    gfx->setFont(nullptr);
    gfx->setTextColor(EPD_BLACK);
    gfx->setTextSize(1);

    render::drawHeaderCentered(gfx, ui::tr("core.ble_pairing_title"), 6, width);

    auto* ble = hal::getBluetoothControllerInstance();
    const char* name = (ble && ble->getDeviceName()) ? ble->getDeviceName() : "";
    const bool connected = ble && ble->isConnected();

    // Instruction
    gfx->setCursor(8, 34);
    render::printText(gfx, ui::tr("core.ble_pairing_instr"));

    // Advertised device name, emphasized and truncated to the panel width
    gfx->setTextSize(2);
    gfx->setCursor(8, 50);
    render::printTruncated(gfx, name, width - 16);
    gfx->setTextSize(1);

    // Connection status
    gfx->setCursor(8, 82);
    render::printText(gfx, connected ? ui::tr("core.ble_pairing_connected")
                                     : ui::tr("core.ble_pairing_waiting"));

    render::drawFooterBar(gfx, width, height, nullptr,
                          ui::tr("core.ble_pairing_exit"), false);

    clearDirty();
}

} // namespace cdc::ui
