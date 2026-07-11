/*
 * ISO 7816 APDU Parser for CDC Badge
 *
 * Based on pico-openpgp (https://github.com/polhenarejos/pico-openpgp)
 * Original: Copyright (c) 2022 Pol Henarejos, AGPLv3
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// APDU Classes
#define CLA_ISO7816             0x00
#define CLA_CHAIN               0x10  // Command chaining

// APDU Instructions (ISO 7816-4 plus one vendor extension)
#define INS_SELECT              0xA4
#define INS_GET_DATA            0xCA
#define INS_PUT_DATA            0xDA
#define INS_PUT_DATA_ODD        0xDB  // PUT DATA (odd INS) with Extended Header List
#define INS_VERIFY              0x20
#define INS_CHANGE_PIN          0x24
#define INS_RESET_RETRY         0x2C
#define INS_PSO                 0x2A  // Perform Security Operation
#define INS_INTERNAL_AUTH       0x88
#define INS_GENERATE_KEYPAIR    0x47
#define INS_GET_CHALLENGE       0x84
#define INS_GET_RESPONSE        0xC0  // Drain remainder of a chained response (ISO 7816-4 §5.3.4)
#define INS_TERMINATE           0xE6
#define INS_ACTIVATE            0x44
#define INS_GET_VERSION         0xF1
#define INS_MSE                 0x22  // Manage Security Environment

// PSO Sub-commands (P1-P2)
#define PSO_CDS                 0x9E9A  // Compute Digital Signature
#define PSO_DEC                 0x8086  // Decipher
#define PSO_ENC                 0x8680  // Encipher

#ifdef __DOXYGEN__
namespace cdc::scard {
#endif

// Parsed APDU structure
typedef struct {
    uint8_t  cla;           // Class byte
    uint8_t  ins;           // Instruction byte
    uint8_t  p1;            // Parameter 1
    uint8_t  p2;            // Parameter 2
    uint16_t lc;            // Command data length (Nc)
    const uint8_t *data;    // Command data pointer
    uint32_t le;            // Expected response length (Ne)
    bool     extended;      // Extended APDU format
} apdu_t;

#ifdef __DOXYGEN__
} // namespace cdc::scard
#endif

// Parse raw APDU bytes into structure
// Returns true on success
bool apdu_parse(const uint8_t *raw, size_t raw_len, apdu_t *apdu);

// Build response APDU with status word
// Returns total response length
size_t apdu_build_response(uint8_t *buf, size_t buf_max,
                           const uint8_t *data, size_t data_len,
                           uint16_t sw);

// Build error response (SW only)
static inline size_t apdu_sw(uint8_t *buf, uint16_t sw) {
    buf[0] = (sw >> 8) & 0xFF;
    buf[1] = sw & 0xFF;
    return 2;
}

#ifdef __cplusplus
}
#endif

