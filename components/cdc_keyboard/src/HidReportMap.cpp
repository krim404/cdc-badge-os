#include "cdc_keyboard/KeyboardReportMap.h"

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace cdc::keyboard {

void packBootReport(uint8_t modifier, const uint8_t* keycodes, uint8_t numKeys,
                    uint8_t* out) {
    if (!out) return;
    if (numKeys > 6) numKeys = 6;
    memset(out, 0, kBootReportSize);
    out[0] = modifier;
    if (keycodes && numKeys > 0) {
        memcpy(out + 2, keycodes, numKeys);
    }
}

/**
 * HID Report Descriptor for a standard keyboard
 *
 * This descriptor defines:
 * - Report ID 1 for keyboard input
 * - 8 modifier bits (Ctrl, Shift, Alt, GUI on both sides)
 * - 1 reserved byte
 * - 6 keycodes (standard USB keyboard rollover)
 *
 * Report format (8 bytes total):
 * Byte 0: Modifier keys (bitmask)
 * Byte 1: Reserved (always 0)
 * Bytes 2-7: Up to 6 simultaneous keycodes
 */
const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)

    // Modifier keys (8 bits)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control = 0xE0)
    0x29, 0xE7,        //   Usage Maximum (Right GUI = 0xE7)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1 bit)
    0x95, 0x08,        //   Report Count (8 bits = 1 byte)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)

    // Reserved byte (padding, always 0)
    0x75, 0x08,        //   Report Size (8 bits)
    0x95, 0x01,        //   Report Count (1 byte)
    0x81, 0x01,        //   Input (Constant) - Reserved

    // LED output report (Num/Caps/Scroll lock).
    // Report ID 2 keeps the output report addressable independently of the
    // keyboard input report (ID 1). Linux/Windows reject mixed-ID descriptors
    // that lack an explicit Report ID for the output collection.
    0x85, 0x02,        //   Report ID (2)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x05,        //   Report Count (5)
    0x91, 0x02,        //   Output (Data, Variable, Absolute)
    0x75, 0x03,        //   Report Size (3) - Padding
    0x95, 0x01,        //   Report Count (1)
    0x91, 0x01,        //   Output (Constant)

    // Keycodes (6 bytes for 6KRO) - restore Report ID 1 for the input report
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101 = Application key)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x75, 0x08,        //   Report Size (8 bits per keycode)
    0x95, 0x06,        //   Report Count (6 keycodes)
    0x81, 0x00,        //   Input (Data, Array)

    0xC0               // End Collection
};

const size_t HID_REPORT_MAP_SIZE = sizeof(HID_REPORT_MAP);

/**
 * \brief Returns the HID report descriptor buffer.
 * \return Pointer to the report-map byte array.
 */
const uint8_t* getHidReportMap() {
    return HID_REPORT_MAP;
}

/**
 * \brief Returns the byte size of the HID report descriptor.
 * \return Size of the report-map buffer in bytes.
 */
size_t getHidReportMapSize() {
    return HID_REPORT_MAP_SIZE;
}

} // namespace cdc::keyboard
