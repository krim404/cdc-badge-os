/**
 * \file
 * \brief USB HID keyboard transport: report delivery over the TinyUSB HID
 *        interrupt IN endpoint, registered on the UsbManager Keyboard slot.
 */

#include "mod_usbhid/UsbHidKeyboard.h"
#include "cdc_keyboard/KeyboardReportMap.h"
#include "cdc_core/UsbManager.h"
#include "usb_badge/usb_hid.h"
#include "usb_descriptors.h"
#include "cdc_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>

static const char* TAG = "UsbHID";

namespace cdc::mod_usbhid {

/** \brief Owning module name for the UsbManager Keyboard interface slot. */
static constexpr const char* USB_OWNER = "mod_usbhid";

/** \brief Standard 8-byte boot keyboard input report payload. */
struct KeyboardReport {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycodes[6];
};

/** \brief Last transmitted keyboard report, served on GET_REPORT. */
static KeyboardReport s_currentReport = {};

/**
 * \brief Handles HID GET_REPORT for the keyboard input report.
 * \param report_id Requested report ID.
 * \param report_type Requested HID report type.
 * \param buffer Destination buffer.
 * \param reqlen Requested length.
 * \return Number of bytes written.
 */
static uint16_t onGetReport(uint8_t report_id, uint8_t report_type,
                            uint8_t* buffer, uint16_t reqlen) {
    (void)report_id;
    if (report_type != HID_REPORT_TYPE_INPUT) return 0;
    uint16_t len = sizeof(s_currentReport);
    if (len > reqlen) len = reqlen;
    memcpy(buffer, &s_currentReport, len);
    return len;
}

/**
 * \brief Handles HID SET_REPORT (LED output report); accepted and ignored.
 * \param report_id Report ID.
 * \param report_type HID report type.
 * \param buffer Report payload.
 * \param bufsize Payload length.
 */
static void onSetReport(uint8_t report_id, uint8_t report_type,
                        uint8_t const* buffer, uint16_t bufsize) {
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
    // Boot-keyboard LED output report (Caps/Num/Scroll lock). The badge has no
    // status LEDs, so the host's LED state is acknowledged and discarded.
}

UsbHidKeyboard& UsbHidKeyboard::instance() {
    static UsbHidKeyboard inst;
    return inst;
}

uint8_t UsbHidKeyboard::hidInstance() const {
    const uint8_t mask = core::UsbManager::instance().activeInterfaceMask();
    const bool fidoActive =
        (mask & (1u << static_cast<uint8_t>(core::UsbHidInterface::Fido))) != 0;
    return fidoActive ? 1 : 0;
}

bool UsbHidKeyboard::registerUsb() {
    if (registered_) return true;

    loadSettings();

    core::UsbInterfaceSpec spec;
    spec.cls = core::UsbInterfaceClass::Hid;
    spec.name = "Keyboard";
    spec.reportDesc = keyboard::getHidReportMap();
    spec.reportDescLen = static_cast<uint16_t>(keyboard::getHidReportMapSize());
    spec.protocol = 1;  // HID_ITF_PROTOCOL_KEYBOARD
    spec.hasOut = false;
    spec.epInSize = EP_KEYBOARD_SIZE;
    spec.callbacks.onGetReport = onGetReport;
    spec.callbacks.onSetReport = onSetReport;

    if (!core::UsbManager::instance().registerInterface(
            core::UsbHidInterface::Keyboard, USB_OWNER, spec)) {
        LOG_W(TAG, "Failed to register USB keyboard interface (slot busy)");
        return false;
    }

    registered_ = true;
    LOG_I(TAG, "USB HID keyboard interface registered (instance=%u)", hidInstance());
    return true;
}

void UsbHidKeyboard::unregisterUsb() {
    if (!registered_) return;
    engine_.cancel();
    core::UsbManager::instance().unregisterInterface(
        core::UsbHidInterface::Keyboard, USB_OWNER);
    registered_ = false;
    LOG_I(TAG, "USB HID keyboard interface unregistered");
}

bool UsbHidKeyboard::isConnected() const {
    if (!registered_) return false;
    return usb_hid_ready() && usb_hid_instance_ready(hidInstance());
}

const char* UsbHidKeyboard::getStatusText() const {
    if (!registered_) return "Not registered";
    return isConnected() ? "Connected" : "Waiting for host";
}

bool UsbHidKeyboard::sendKeyReport(uint8_t modifier, const uint8_t* keycodes,
                                   uint8_t numKeys) {
    if (!registered_ || !usb_hid_ready()) return false;

    // The interrupt IN endpoint holds one report until the host polls it
    // (bInterval 10 ms); the previous report must drain before another one
    // can be queued, otherwise it is rejected.
    const uint8_t instance = hidInstance();
    for (int i = 0; i < 30 && !usb_hid_instance_ready(instance); i++) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!usb_hid_instance_ready(instance)) return false;

    static_assert(sizeof(s_currentReport) == keyboard::kBootReportSize,
                  "KeyboardReport must match the packed boot-report layout");
    keyboard::packBootReport(modifier, keycodes, numKeys,
                             reinterpret_cast<uint8_t*>(&s_currentReport));

    return usb_hid_send_report(instance, REPORT_ID_KEYBOARD,
                               reinterpret_cast<const uint8_t*>(&s_currentReport),
                               sizeof(s_currentReport));
}

} // namespace cdc::mod_usbhid
