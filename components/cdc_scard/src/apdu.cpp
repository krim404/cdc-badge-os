/**
 * \brief ISO 7816 APDU parsing/building helpers for CDC Badge smartcard stack.
 *
 * Based on pico-openpgp (https://github.com/polhenarejos/pico-openpgp).
 */

#include "cdc_scard/apdu.h"
#include <string.h>

/**
 * \brief Parses raw APDU bytes into structured representation.
 * \param raw Raw APDU buffer.
 * \param raw_len Length of `raw`.
 * \param apdu Output APDU structure.
 * \return `true` if APDU framing is valid and parsing succeeded.
 */
bool apdu_parse(const uint8_t *raw, size_t raw_len, apdu_t *apdu) {
    if (!raw || !apdu || raw_len < 4) {
        return false;
    }

    memset(apdu, 0, sizeof(apdu_t));

    apdu->cla = raw[0];
    apdu->ins = raw[1];
    apdu->p1  = raw[2];
    apdu->p2  = raw[3];

    // Case 1: No Lc, no Le (just CLA INS P1 P2)
    if (raw_len == 4) {
        apdu->lc = 0;
        apdu->le = 0;
        apdu->data = nullptr;
        return true;
    }

    size_t pos = 4;

    // Check for extended APDU (first length byte is 0x00)
    if (raw[pos] == 0x00 && raw_len > 7) {
        apdu->extended = true;

        // Extended Lc (3 bytes: 0x00 + 2 bytes)
        if (pos + 3 <= raw_len) {
            apdu->lc = static_cast<uint16_t>((raw[pos + 1] << 8) | raw[pos + 2]);
            pos += 3;
        }
    } else {
        apdu->extended = false;

        // Short Lc (1 byte) or Le
        if (raw_len == 5) {
            // Case 2: Le only
            apdu->lc = 0;
            apdu->le = raw[pos] == 0 ? 256 : raw[pos];
            apdu->data = nullptr;
            return true;
        }

        apdu->lc = raw[pos];
        pos++;
    }

    // Command data
    if (apdu->lc > 0) {
        if (pos + apdu->lc > raw_len) {
            return false;  // Not enough data
        }
        apdu->data = raw + pos;
        pos += apdu->lc;
    }

    // Le field (optional)
    if (pos < raw_len) {
        if (apdu->extended) {
            // Extended Le (2 bytes)
            if (pos + 2 <= raw_len) {
                uint16_t le_val = static_cast<uint16_t>((raw[pos] << 8) | raw[pos + 1]);
                apdu->le = (le_val == 0) ? 65536 : le_val;
            }
        } else {
            // Short Le (1 byte)
            apdu->le = (raw[pos] == 0) ? 256 : raw[pos];
        }
    }

    return true;
}

/**
 * \brief Builds APDU response payload with status word trailer.
 * \param buf Output response buffer.
 * \param buf_max Capacity of `buf`.
 * \param data Optional payload bytes.
 * \param data_len Payload length.
 * \param sw ISO7816 status word.
 * \return Total bytes written to `buf`.
 */
size_t apdu_build_response(uint8_t *buf, size_t buf_max,
                           const uint8_t *data, size_t data_len,
                           uint16_t sw) {
    if (!buf || buf_max < 2) {
        return 0;
    }

    size_t total = data_len + 2;
    if (total > buf_max) {
        // Truncate data if necessary
        data_len = buf_max - 2;
        total = buf_max;
    }

    if (data && data_len > 0) {
        memcpy(buf, data, data_len);
    }

    buf[data_len] = (sw >> 8) & 0xFF;
    buf[data_len + 1] = sw & 0xFF;

    return total;
}
