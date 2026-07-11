/**
 * \brief T=1 transport state machine (card side) implementation.
 */

#include "cdc_scard/t1/t1_state.h"

#include <string.h>

namespace {

constexpr uint8_t kCardNad = 0x00;

uint8_t flip(uint8_t bit) {
    return bit ? 0 : 1;
}

}  // namespace

static size_t encode_rblock(t1_state_t *state, uint8_t nr, t1_r_error_t err) {
    size_t len = 0;
    t1_block_encode_r(kCardNad, nr, err, state->use_crc,
                      state->control_block, sizeof(state->control_block), &len);
    state->control_len = len;
    return len;
}

static size_t encode_sblock_response(t1_state_t *state, t1_s_subtype_t subtype,
                                     const uint8_t *inf, uint8_t inf_len) {
    size_t len = 0;
    t1_block_encode_s(kCardNad, subtype, inf, inf_len, state->use_crc,
                      state->control_block, sizeof(state->control_block), &len);
    state->control_len = len;
    return len;
}

void t1_state_init(t1_state_t *state, bool use_crc) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->ns_send = 0;
    state->ns_expected = 0;
    state->ifsc = T1_DEFAULT_IFSC;
    state->ifsd = T1_DEFAULT_IFSC;
    state->use_crc = use_crc;
}

static t1_feed_t handle_iblock(t1_state_t *state, const t1_block_t *blk) {
    if (!blk->edc_ok) {
        encode_rblock(state, state->ns_expected, T1_R_EDC_ERR);
        return T1_FEED_NEED_OUT;
    }
    if (blk->i_ns != state->ns_expected) {
        // Out-of-sequence: ask for the expected N(S) via R-block.
        encode_rblock(state, state->ns_expected, T1_R_OTHER_ERR);
        return T1_FEED_NEED_OUT;
    }
    // Sequence OK: append payload.
    if (!state->apdu_in_progress) {
        state->apdu_len = 0;
        state->apdu_in_progress = true;
    }
    if (state->apdu_len + blk->inf_len > sizeof(state->apdu_buf)) {
        // APDU too large; abort.
        encode_sblock_response(state, T1_S_ABORT_RESP, nullptr, 0);
        state->apdu_in_progress = false;
        state->apdu_len = 0;
        return T1_FEED_NEED_OUT;
    }
    if (blk->inf_len > 0) {
        memcpy(state->apdu_buf + state->apdu_len, blk->inf, blk->inf_len);
        state->apdu_len += blk->inf_len;
    }

    state->ns_expected = flip(state->ns_expected);

    if (blk->i_more) {
        // Reader has more data; ACK with R-block carrying our updated N(R).
        encode_rblock(state, state->ns_expected, T1_R_OK);
        return T1_FEED_WAIT_MORE;
    }
    state->apdu_in_progress = false;
    return T1_FEED_APDU_READY;
}

static t1_feed_t handle_rblock(t1_state_t *state, const t1_block_t *blk) {
    // Reader is asking for a specific N(S). Common during response-chunk
    // transmission. Simplest correct behaviour: re-arm ns_send to whatever
    // the reader expects and let the next outbound chunk go out.
    if (!blk->edc_ok) {
        encode_rblock(state, state->ns_expected, T1_R_EDC_ERR);
        return T1_FEED_NEED_OUT;
    }
    if (state->resp_pending) {
        state->ns_send = blk->r_nr;
    }
    return T1_FEED_IGNORED;
}

static t1_feed_t handle_sblock(t1_state_t *state, const t1_block_t *blk) {
    if (!blk->edc_ok) {
        encode_rblock(state, state->ns_expected, T1_R_EDC_ERR);
        return T1_FEED_NEED_OUT;
    }
    switch (blk->s_subtype) {
        case T1_S_IFS_REQ:
            if (blk->inf_len == 1 && blk->inf != nullptr) {
                state->ifsd = blk->inf[0];
                encode_sblock_response(state, T1_S_IFS_RESP, blk->inf, 1);
                return T1_FEED_NEED_OUT;
            }
            encode_rblock(state, state->ns_expected, T1_R_OTHER_ERR);
            return T1_FEED_NEED_OUT;
        case T1_S_ABORT_REQ:
            // Drop any pending chained APDU / response.
            state->apdu_in_progress = false;
            state->apdu_len = 0;
            state->resp_pending = false;
            state->resp_len = 0;
            state->resp_sent = 0;
            encode_sblock_response(state, T1_S_ABORT_RESP, nullptr, 0);
            return T1_FEED_NEED_OUT;
        case T1_S_WTX_REQ:
            // Reader is granting us more time; reply with same INF.
            encode_sblock_response(state, T1_S_WTX_RESP, blk->inf, blk->inf_len);
            return T1_FEED_NEED_OUT;
        case T1_S_RESYNCH_REQ:
            // Reset sequence numbers and pending traffic.
            state->ns_send = 0;
            state->ns_expected = 0;
            state->apdu_in_progress = false;
            state->apdu_len = 0;
            state->resp_pending = false;
            state->resp_len = 0;
            state->resp_sent = 0;
            encode_sblock_response(state, T1_S_RESYNCH_RESP, nullptr, 0);
            return T1_FEED_NEED_OUT;
        default:
            // Unknown subtype — ignore per ISO 7816-3 robustness guidance.
            return T1_FEED_IGNORED;
    }
}

t1_feed_t t1_state_feed(t1_state_t *state, const t1_block_t *blk) {
    if (!state || !blk) return T1_FEED_IGNORED;
    switch (blk->kind) {
        case T1_BLOCK_I:
            return handle_iblock(state, blk);
        case T1_BLOCK_R:
            return handle_rblock(state, blk);
        case T1_BLOCK_S:
            return handle_sblock(state, blk);
    }
    return T1_FEED_IGNORED;
}

bool t1_state_queue_response(t1_state_t *state, const uint8_t *resp, size_t resp_len) {
    if (!state || (resp_len > 0 && !resp)) return false;
    if (resp_len > sizeof(state->resp_buf)) return false;
    if (resp_len > 0) {
        memcpy(state->resp_buf, resp, resp_len);
    }
    state->resp_len = resp_len;
    state->resp_sent = 0;
    state->resp_pending = (resp_len > 0);
    return true;
}

size_t t1_state_next_outbound(t1_state_t *state, uint8_t *out, size_t out_cap) {
    if (!state || !out) return 0;

    // Control blocks (R / S responses) always go first.
    if (state->control_len > 0) {
        const size_t n = state->control_len;
        if (n > out_cap) return 0;
        memcpy(out, state->control_block, n);
        state->control_len = 0;
        return n;
    }

    if (!state->resp_pending) {
        return 0;
    }

    const size_t remaining = state->resp_len - state->resp_sent;
    if (remaining == 0) {
        state->resp_pending = false;
        return 0;
    }

    const uint16_t chunk_cap = state->ifsd ? state->ifsd : T1_DEFAULT_IFSC;
    const size_t chunk_len = (remaining > chunk_cap) ? chunk_cap : remaining;
    const bool more = (chunk_len < remaining);

    size_t encoded = 0;
    t1_status_t status = t1_block_encode_i(
        kCardNad, state->ns_send, more,
        state->resp_buf + state->resp_sent, static_cast<uint8_t>(chunk_len),
        state->use_crc, out, out_cap, &encoded);
    if (status != T1_OK) {
        return 0;
    }

    state->resp_sent += chunk_len;
    state->ns_send = flip(state->ns_send);
    if (!more) {
        state->resp_pending = false;
    }
    return encoded;
}
