/*
 * USB CCID (Chip Card Interface Device) for CDC Badge
 *
 * Based on pico-openpgp (https://github.com/polhenarejos/pico-openpgp)
 * Original: Copyright (c) 2022 Pol Henarejos, AGPLv3
 *
 * CCID Class: 0x0B (Smart Card)
 * Uses Gemalto VID/PID for libccid whitelist compatibility
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// CCID USB Class
#define USB_CLASS_CCID          0x0B

// VID/PID for libccid whitelist compatibility (Gemalto GemPC433)
#define CCID_USB_VID            0x08e6
#define CCID_USB_PID            0x4433

// CCID Message Types (PC to Reader)
#define CCID_PC_TO_RDR_ICC_POWER_ON     0x62
#define CCID_PC_TO_RDR_ICC_POWER_OFF    0x63
#define CCID_PC_TO_RDR_GET_SLOT_STATUS  0x65
#define CCID_PC_TO_RDR_XFR_BLOCK        0x6F
#define CCID_PC_TO_RDR_GET_PARAMETERS   0x6C
#define CCID_PC_TO_RDR_RESET_PARAMETERS 0x6D
#define CCID_PC_TO_RDR_SET_PARAMETERS   0x61
#define CCID_PC_TO_RDR_SECURE           0x69

// CCID Message Types (Reader to PC)
#define CCID_RDR_TO_PC_DATA_BLOCK       0x80
#define CCID_RDR_TO_PC_SLOT_STATUS      0x81
#define CCID_RDR_TO_PC_PARAMETERS       0x82

// CCID Slot Status
#define CCID_ICC_PRESENT_ACTIVE         0x00
#define CCID_ICC_PRESENT_INACTIVE       0x01
#define CCID_ICC_NOT_PRESENT            0x02

// CCID Command Status
#define CCID_CMD_STATUS_OK              0x00
#define CCID_CMD_STATUS_FAILED          0x40
#define CCID_CMD_STATUS_TIME_EXT        0x80

// CCID Error Codes
#define CCID_ERROR_CMD_ABORTED          0xFF
#define CCID_ERROR_ICC_MUTE             0xFE
#define CCID_ERROR_XFR_PARITY_ERROR     0xFD
#define CCID_ERROR_XFR_OVERRUN          0xFC
#define CCID_ERROR_HW_ERROR             0xFB
#define CCID_ERROR_CMD_NOT_SUPPORTED    0x00

// Buffer sizes
#define CCID_MAX_MSG_SIZE               2048
#define CCID_HEADER_SIZE                10

// CCID Message Header (10 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  bMessageType;
    uint32_t dwLength;
    uint8_t  bSlot;
    uint8_t  bSeq;
    uint8_t  bSpecific[3];
} ccid_header_t;

// Initialize CCID interface
bool ccid_init(void);

// Process incoming CCID message
// Returns response length
int ccid_process_message(const uint8_t *msg, size_t msg_len,
                         uint8_t *resp, size_t resp_max);

// Get ATR (Answer To Reset)
const uint8_t* ccid_get_atr(size_t *len);

// Card present status
bool ccid_card_present(void);

#ifdef __cplusplus
}
#endif

