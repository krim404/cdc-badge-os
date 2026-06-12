#pragma once

#include <cstdint>
#include <cstddef>

namespace cdc::keyboard {

/** \brief Size of the standard boot-keyboard input report in bytes. */
static constexpr size_t kBootReportSize = 8;

/**
 * \brief Packs a boot-keyboard input report into an 8-byte buffer.
 *
 * Lays out the canonical report (modifier, reserved=0, up to 6 keycodes) shared
 * by every keyboard transport. Excess keycodes are clamped to 6 and unused
 * keycode slots are zeroed.
 *
 * \param modifier Modifier bitmask (Modifier flags).
 * \param keycodes Array of keycodes; may be `nullptr` when \p numKeys is 0.
 * \param numKeys Number of valid keycodes (clamped to 6).
 * \param out Destination buffer of at least \ref kBootReportSize bytes.
 */
void packBootReport(uint8_t modifier, const uint8_t* keycodes, uint8_t numKeys,
                    uint8_t* out);

/**
 * \brief Returns the standard 8-byte boot-keyboard HID report descriptor.
 *
 * The descriptor is transport-agnostic: it is served verbatim by the BLE HID
 * Report Map characteristic (0x2A4B) and by the USB HID interface.
 * \return Pointer to the report-map byte array.
 */
const uint8_t* getHidReportMap();

/**
 * \brief Returns the byte size of the HID report descriptor.
 * \return Size of the report-map buffer in bytes.
 */
size_t getHidReportMapSize();

} // namespace cdc::keyboard
