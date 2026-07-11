/**
 * \brief ISO 7816-3 T=1 block-format encoder / decoder.
 *
 * A T=1 block has the shape:
 *
 *   +-----+-----+-----+----------------+-----+
 *   | NAD | PCB | LEN | INF (0..254 B) | EDC |
 *   +-----+-----+-----+----------------+-----+
 *      1B    1B    1B   LEN bytes        1B (LRC) or 2B (CRC)
 *
 * - NAD: source/destination address (0x00 in the OpenPGP smartcard role).
 * - PCB: protocol control byte; encodes block type, sequence numbers, and
 *        more-data flag.
 * - LEN: information field length (0..254). 0xFF is reserved.
 * - INF: payload bytes.
 * - EDC: error-detection code. LRC = XOR of NAD..INF; CRC = CRC-16 over the
 *        same bytes (selected via T=1 protocol parameters in ATR).
 *
 * This header exposes only the syntax layer. The state machine (sequence
 * numbers, IFSC/IFSD negotiation, reassembly of chained APDUs) lives in
 * t1_state.h.
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \brief Maximum information-field length per ISO 7816-3 T=1 (LEN byte). */
#define T1_INF_MAX 254

/** \brief Default IFSC at protocol start (ISO 7816-3, before negotiation). */
#define T1_DEFAULT_IFSC 32

/** \brief Reserved LEN value; must not appear on the wire. */
#define T1_LEN_RESERVED 0xFF

/** \brief T=1 block taxonomy. */
typedef enum {
    T1_BLOCK_I,  /**< Information block (carries APDU bytes). */
    T1_BLOCK_R,  /**< Receive-ready block (ACK / error). */
    T1_BLOCK_S,  /**< Supervisory block (IFS, ABORT, WTX, RESYNCH). */
} t1_block_kind_t;

/** \brief R-block error indicator (PCB bits 1-0). */
typedef enum {
    T1_R_OK         = 0x00, /**< No error, request retransmission of N(R). */
    T1_R_EDC_ERR    = 0x01, /**< EDC / parity error detected. */
    T1_R_OTHER_ERR  = 0x02, /**< Other error. */
} t1_r_error_t;

/** \brief S-block subtype (PCB bits 5-0). */
typedef enum {
    T1_S_RESYNCH_REQ  = 0x00,
    T1_S_IFS_REQ      = 0x01,
    T1_S_ABORT_REQ    = 0x02,
    T1_S_WTX_REQ      = 0x03,
    T1_S_RESYNCH_RESP = 0x20,
    T1_S_IFS_RESP     = 0x21,
    T1_S_ABORT_RESP   = 0x22,
    T1_S_WTX_RESP     = 0x23,
} t1_s_subtype_t;

/** \brief Decoded block. INF points into the buffer supplied to t1_block_decode. */
typedef struct {
    uint8_t nad;
    uint8_t pcb;
    t1_block_kind_t kind;
    /* I-block fields. */
    uint8_t i_ns;       /**< N(S) sequence number, 0 or 1. */
    bool    i_more;     /**< More-data flag (chaining bit). */
    /* R-block fields. */
    uint8_t r_nr;       /**< N(R) expected sequence number, 0 or 1. */
    t1_r_error_t r_err; /**< R-block error indicator. */
    /* S-block fields. */
    t1_s_subtype_t s_subtype;
    /* Payload. */
    const uint8_t *inf;
    uint8_t inf_len;
    /* EDC validation. */
    bool edc_ok;
    bool use_crc;       /**< true if 2-byte CRC was expected; false for LRC. */
} t1_block_t;

/** \brief Result of t1_block_decode / t1_block_encode. */
typedef enum {
    T1_OK = 0,
    T1_ERR_SHORT,        /**< Buffer too small for header. */
    T1_ERR_LEN,          /**< LEN byte is reserved (0xFF) or buffer mismatch. */
    T1_ERR_INF_TOO_BIG,  /**< INF length > T1_INF_MAX. */
    T1_ERR_EDC,          /**< EDC mismatch. */
    T1_ERR_PCB,          /**< PCB encoding invalid (e.g. R-block with non-zero reserved bits). */
    T1_ERR_OUT_BUF,      /**< Output buffer too small. */
    T1_ERR_NULL,         /**< NULL argument where forbidden. */
} t1_status_t;

/**
 * \brief Compute the longitudinal redundancy check (LRC) of a buffer.
 *
 * LRC is the bytewise XOR of all bytes in \p buf. Used as the default
 * EDC byte when the T=1 protocol parameters do not select CRC.
 *
 * \param buf Buffer pointer.
 * \param len Number of bytes to fold into the LRC.
 * \return XOR of all bytes.
 */
uint8_t t1_lrc(const uint8_t *buf, size_t len);

/**
 * \brief Compute the ISO/IEC 13239 CRC-16 of a buffer (polynomial 0x1021,
 * initial value 0xFFFF, no final XOR, MSB first). Used as the alternative
 * EDC when CRC is negotiated.
 */
uint16_t t1_crc(const uint8_t *buf, size_t len);

/**
 * \brief Encode an I-block.
 * \param nad Node address.
 * \param ns  Sequence number (0 or 1).
 * \param more More-data flag (chaining).
 * \param inf Information field payload (may be NULL when inf_len == 0).
 * \param inf_len Payload length (<= T1_INF_MAX).
 * \param use_crc Use 2-byte CRC instead of LRC.
 * \param out Output buffer (caller-allocated, at least 4 + inf_len + (use_crc ? 1 : 0) bytes).
 * \param out_cap Output capacity in bytes.
 * \param out_len Receives total bytes written.
 */
t1_status_t t1_block_encode_i(uint8_t nad, uint8_t ns, bool more,
                              const uint8_t *inf, uint8_t inf_len,
                              bool use_crc,
                              uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * \brief Encode an R-block.
 * \param nad Node address.
 * \param nr Expected sequence number (0 or 1).
 * \param err Error indicator.
 * \param use_crc EDC algorithm select.
 * \param out Output buffer.
 * \param out_cap Output buffer capacity.
 * \param out_len Receives bytes written.
 */
t1_status_t t1_block_encode_r(uint8_t nad, uint8_t nr, t1_r_error_t err,
                              bool use_crc,
                              uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * \brief Encode an S-block.
 * \param nad Node address.
 * \param subtype S-block subtype (request or response).
 * \param inf Optional info payload (e.g. IFS new value).
 * \param inf_len Length of the info payload (0..254).
 * \param use_crc EDC algorithm select.
 * \param out Output buffer.
 * \param out_cap Output buffer capacity.
 * \param out_len Receives bytes written.
 */
t1_status_t t1_block_encode_s(uint8_t nad, t1_s_subtype_t subtype,
                              const uint8_t *inf, uint8_t inf_len,
                              bool use_crc,
                              uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * \brief Decode a T=1 block.
 *
 * The decoder validates LEN, EDC, and PCB encoding rules. Reserved bits in
 * I- and R-blocks must be zero; otherwise T1_ERR_PCB is returned.
 *
 * \param buf Input bytes.
 * \param buf_len Number of bytes available.
 * \param use_crc If true, expect 2-byte CRC; otherwise LRC.
 * \param out Decoded block (INF pointer aliases into \p buf).
 */
t1_status_t t1_block_decode(const uint8_t *buf, size_t buf_len, bool use_crc,
                            t1_block_t *out);

#ifdef __cplusplus
}
#endif

