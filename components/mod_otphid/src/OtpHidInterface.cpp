/**
 * \file
 * \brief Yubico-OTP HID transport: report descriptor and UsbManager
 *        Keyboard-slot registration. The feature-report exchange (status,
 *        HMAC-SHA1 challenge-response, CRC16) lives in \ref OtpHidCr.
 *
 * Protocol references:
 *  - Yubico HMAC-SHA1 challenge-response over HID feature reports.
 *  - yubikey-personalization ykcore/ykdef.h: status_st layout, FEATURE_RPT_SIZE,
 *    slot/flag bits, and the GET-status read path.
 */

#include "mod_otphid/OtpHidInterface.h"
#include "mod_otphid/OtpHidConstants.h"
#include "mod_otphid/OtpHidCr.h"
#include "cdc_core/UsbManager.h"
#include "usb_badge/usb_hid.h"
#include "usb_descriptors.h"
#include "cdc_log.h"

extern "C" {
#include "class/hid/hid.h"
}

#include <cstring>

static const char* TAG = "OtpHID";

namespace cdc::mod_otphid {

/** \brief Owning module name for the UsbManager Keyboard interface slot. */
static constexpr const char* USB_OWNER = "mod_otphid";

/**
 * \brief Yubico OTP HID report descriptor (vendor-defined usage page 0xFF00).
 *
 * One 8-byte input report and one 8-byte feature report. Host tools open this
 * interface and exchange status / challenge-response frames over the feature
 * report channel; the input report mirrors the real device and is unused for CR.
 */
static const uint8_t kOtpReportDesc[] = {
    0x06, 0x00, 0xFF,        // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,              // Usage (Vendor 0x01)
    0xA1, 0x01,              // Collection (Application)
    0x19, 0x01,              //   Usage Minimum (1)
    0x29, FEATURE_RPT_SIZE,  //   Usage Maximum (8)
    0x15, 0x00,              //   Logical Minimum (0)
    0x26, 0xFF, 0x00,        //   Logical Maximum (255)
    0x75, 0x08,              //   Report Size (8 bits)
    0x95, FEATURE_RPT_SIZE,  //   Report Count (8 bytes)
    0x81, 0x02,              //   Input (Data, Variable, Absolute)
    0x19, 0x01,              //   Usage Minimum (1)
    0x29, FEATURE_RPT_SIZE,  //   Usage Maximum (8)
    0xB1, 0x02,              //   Feature (Data, Variable, Absolute)
    0xC0                     // End Collection
};

/**
 * \brief Handles HID GET feature reads (status or CR readback).
 *
 * Runs on the TinyUSB task. Delegates to \ref OtpHidCr, which returns either
 * the emulated YubiKey status or the next sequenced challenge-response frame.
 *
 * \param report_id Requested report ID (unused; single report ID 0).
 * \param report_type Requested HID report type.
 * \param buffer Destination buffer.
 * \param reqlen Requested length.
 * \return Number of bytes written.
 */
static uint16_t onGetReport(uint8_t report_id, uint8_t report_type,
                            uint8_t* buffer, uint16_t reqlen) {
    (void)report_id;
    if (report_type != HID_REPORT_TYPE_FEATURE) return 0;
    return OtpHidCr::instance().onGetReport(buffer, reqlen);
}

/**
 * \brief Handles HID SET feature writes (challenge frames).
 *
 * Runs on the TinyUSB task. Delegates to \ref OtpHidCr, which reassembles the
 * Yubico frame and records a validated challenge; no crypto/UI happens here.
 *
 * \param report_id Report ID.
 * \param report_type HID report type.
 * \param buffer Report payload.
 * \param bufsize Payload length.
 */
static void onSetReport(uint8_t report_id, uint8_t report_type,
                        uint8_t const* buffer, uint16_t bufsize) {
    (void)report_id;
    if (report_type != HID_REPORT_TYPE_FEATURE) return;
    OtpHidCr::instance().onSetReport(buffer, bufsize);
}

OtpHidInterface& OtpHidInterface::instance() {
    static OtpHidInterface inst;
    return inst;
}

bool OtpHidInterface::registerUsb() {
    if (registered_) return true;

    core::UsbInterfaceSpec spec;
    spec.cls = core::UsbInterfaceClass::Hid;
    spec.name = "OTP";
    spec.reportDesc = kOtpReportDesc;
    spec.reportDescLen = static_cast<uint16_t>(sizeof(kOtpReportDesc));
    spec.protocol = 0;  // HID_ITF_PROTOCOL_NONE (vendor-defined, not a keyboard)
    spec.hasOut = false;
    spec.epInSize = FEATURE_RPT_SIZE;
    spec.preferredVid = USB_VID_ONLYKEY;
    spec.preferredPid = USB_PID_ONLYKEY;
    spec.callbacks.onGetReport = onGetReport;
    spec.callbacks.onSetReport = onSetReport;

    if (!core::UsbManager::instance().registerInterface(
            core::UsbHidInterface::Keyboard, USB_OWNER, spec)) {
        LOG_W(TAG, "Failed to register OTP HID interface (slot busy)");
        return false;
    }

    OtpHidCr::instance().reset();
    registered_ = true;
    LOG_I(TAG, "OTP HID interface registered (OnlyKey VID/PID)");
    return true;
}

void OtpHidInterface::unregisterUsb() {
    if (!registered_) return;
    core::UsbManager::instance().unregisterInterface(
        core::UsbHidInterface::Keyboard, USB_OWNER);
    OtpHidCr::instance().reset();
    registered_ = false;
    LOG_I(TAG, "OTP HID interface unregistered");
}

bool OtpHidInterface::isConnected() const {
    if (!registered_) return false;
    return usb_hid_ready();
}

} // namespace cdc::mod_otphid
