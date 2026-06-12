#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::mod_otphid {

/**
 * \brief Yubico HMAC-SHA1 challenge-response over HID feature reports.
 *
 * Implements the device side of the YubiKey slot-2 challenge-response exchange
 * that KeePassXC / ykchalresp speak over the OTP HID feature-report channel:
 *
 *  - The host writes a 70-byte Yubico frame (64-byte payload + slot + CRC16 +
 *    filler) in 8-byte feature reports, each carrying 7 data bytes plus a
 *    sequence/flag byte (`seq | SLOT_WRITE_FLAG`). \ref onSetReport reassembles
 *    the frame and validates the CRC16 (ISO13239) on the SET path.
 *  - The HMAC itself is computed by the shared `IChallengeResponder` service
 *    (the designated USB-CR entry in the 2FA module); this class never hashes.
 *  - When the entry requires touch, the response is withheld behind an E-Paper
 *    confirmation. While withheld, the GET path returns frames with
 *    `RESP_PENDING_FLAG` set so the host keeps polling, matching how a real
 *    YubiKey reports a pending touch.
 *  - Once ready, \ref onGetReport streams the 22-byte response (20-byte digest
 *    + CRC16) back in sequenced 8-byte feature reports.
 *
 * Cross-task discipline (mirrors mod_2fa BLE CR): the TinyUSB feature-report
 * callbacks (\ref onSetReport, \ref onGetReport) only buffer state; all crypto,
 * UI and ServiceRegistry access happen on the main task in \ref tick.
 */
class OtpHidCr {
public:
    static OtpHidCr& instance();

    /**
     * \brief Resets all CR state (call on interface (un)register).
     */
    void reset();

    /**
     * \brief HID GET_FEATURE handler (TinyUSB task): status or CR readback.
     * \param buffer Destination, must hold at least 8 bytes.
     * \param reqlen Requested length.
     * \return Bytes written (8 on success, 0 otherwise).
     */
    uint16_t onGetReport(uint8_t* buffer, uint16_t reqlen);

    /**
     * \brief HID SET_FEATURE handler (TinyUSB task): challenge frame assembly.
     * \param buffer Feature-report payload.
     * \param bufsize Payload length.
     */
    void onSetReport(uint8_t const* buffer, uint16_t bufsize);

    /**
     * \brief Main-task tick: computes a pending challenge and gates on touch.
     * \param nowMs Current uptime in milliseconds.
     */
    void tick(uint32_t nowMs);

    /**
     * \brief Computes the CRC16 (ISO13239) over \p data.
     * \param data Input bytes.
     * \param len Input length.
     * \return 16-bit CRC.
     */
    static uint16_t crc16(const uint8_t* data, size_t len);

private:
    OtpHidCr() = default;

    void deliverConfirmedResponse();
    static void onTouchConfirm(void* userData);
    static void onTouchCancel(void* userData);
};

} // namespace cdc::mod_otphid
