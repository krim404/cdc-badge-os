/**
 * \brief ISO 7816-3 T=1 transport state machine (card side).
 *
 * Layered on top of t1_block.h. Owns:
 * - sequence-number bookkeeping (N(S), N(R))
 * - IFSC negotiation
 * - reassembly of chained reader-to-card APDUs (chaining bit set in I-blocks)
 * - chunking of card-to-reader responses according to the negotiated IFSD
 * - response generation for S-blocks (IFS, ABORT, WTX, RESYNCH)
 * - R-block emission on EDC error / unexpected sequence number
 *
 * The CCID layer (ccid.cpp) drives this state machine through three calls:
 *   1. t1_state_feed()         on every incoming block
 *   2. t1_state_queue_response() when the APDU dispatcher returns
 *   3. t1_state_next_outbound() until it returns 0
 */

#pragma once
#include "cdc_scard/t1/t1_block.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \brief Maximum APDU size we accept across chained I-blocks (extended-length cap). */
#ifndef T1_MAX_APDU_LEN
#define T1_MAX_APDU_LEN 4096
#endif

/** \brief Maximum response size that can be chunked back. */
#ifndef T1_MAX_RESPONSE_LEN
#define T1_MAX_RESPONSE_LEN 4096
#endif

/** \brief Outcome of feeding a block to the state machine. */
typedef enum {
    T1_FEED_NEED_OUT,    /**< A response block (R / S / ACK) is queued; drain via t1_state_next_outbound. */
    T1_FEED_APDU_READY,  /**< A complete APDU is in state->apdu_buf (length state->apdu_len). */
    T1_FEED_WAIT_MORE,   /**< Reader chained more data; we ACKed with an R-block. */
    T1_FEED_IGNORED,     /**< Block ignored (decoder error already surfaced via outbound). */
} t1_feed_t;

/** \brief Card-side T=1 state. Embedded entirely in caller-provided storage. */
typedef struct {
    /* Sequence-number bookkeeping. */
    uint8_t ns_send;       /**< Next N(S) we will set on outgoing I-block. */
    uint8_t ns_expected;   /**< N(S) we expect on the next incoming I-block. */

    /* Negotiated frame sizes. */
    uint16_t ifsc;         /**< Max INF length we can RECEIVE. Initialised to T1_DEFAULT_IFSC. */
    uint16_t ifsd;         /**< Max INF length the READER can RECEIVE. Initialised to T1_DEFAULT_IFSC. */

    /* EDC selection (driven by ATR / protocol parameters). */
    bool use_crc;

    /* Reassembly buffer for incoming chained APDU. */
    uint8_t apdu_buf[T1_MAX_APDU_LEN];
    size_t  apdu_len;
    bool    apdu_in_progress;

    /* Outgoing response chunking. */
    uint8_t resp_buf[T1_MAX_RESPONSE_LEN];
    size_t  resp_len;
    size_t  resp_sent;
    bool    resp_pending;

    /* Pending control reply (R / S block) — drained before resp_buf chunks. */
    uint8_t control_block[T1_INF_MAX + 5]; /* worst case: NAD+PCB+LEN + INF + 2-byte CRC */
    size_t  control_len;
} t1_state_t;

/**
 * \brief Initialise the state machine. Sets sequence numbers to 0, IFSC/IFSD to defaults.
 * \param state State storage (caller-owned, will be zeroed).
 * \param use_crc EDC choice; must match what's negotiated with the reader via ATR.
 */
void t1_state_init(t1_state_t *state, bool use_crc);

/**
 * \brief Feed an incoming decoded block to the state machine.
 *
 * On EDC errors or unexpected sequence numbers the function queues an R-block
 * (control_block). Mainline I-blocks either complete an APDU (T1_FEED_APDU_READY)
 * or ACK chained pieces (T1_FEED_WAIT_MORE). S-blocks are handled and their
 * responses queued.
 */
t1_feed_t t1_state_feed(t1_state_t *state, const t1_block_t *block);

/**
 * \brief Register the application's APDU response so the state machine can
 *        chunk it into I-blocks.
 *
 * Must only be called after t1_state_feed returned T1_FEED_APDU_READY.
 * \return false if `resp_len > T1_MAX_RESPONSE_LEN`.
 */
bool t1_state_queue_response(t1_state_t *state, const uint8_t *resp, size_t resp_len);

/**
 * \brief Emit the next outbound block (control reply or response chunk).
 * \param state State machine.
 * \param out Output buffer.
 * \param out_cap Output buffer capacity.
 * \return Number of bytes written. Zero means nothing to send.
 */
size_t t1_state_next_outbound(t1_state_t *state, uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

