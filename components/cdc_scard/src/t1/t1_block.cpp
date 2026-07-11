/**
 * \brief T=1 block encoder / decoder.
 *
 * Pure syntax layer. State and reassembly live in t1_state.cpp.
 */

#include "cdc_scard/t1/t1_block.h"

#include <string.h>

namespace {

constexpr uint8_t kPcbBlockKindMask = 0xC0;
constexpr uint8_t kPcbIBlock        = 0x00;  // bit 8 = 0
constexpr uint8_t kPcbRBlock        = 0x80;  // bits 8-7 = 10
constexpr uint8_t kPcbSBlock        = 0xC0;  // bits 8-7 = 11

// I-block PCB bits.
constexpr uint8_t kPcbINs           = 0x40;  // bit 7
constexpr uint8_t kPcbIMore         = 0x20;  // bit 6
constexpr uint8_t kPcbIReservedMask = 0x1F;  // bits 5-1 must be zero

// R-block PCB bits.
constexpr uint8_t kPcbRNr           = 0x10;  // bit 5
constexpr uint8_t kPcbRErrMask      = 0x03;  // bits 2-1
constexpr uint8_t kPcbRReservedMask = 0x2C;  // bits 6, 4, 3 must be zero

// S-block PCB bits.
constexpr uint8_t kPcbSSubtypeMask  = 0x3F;  // bits 6-1 carry the subtype

}  // namespace

uint8_t t1_lrc(const uint8_t *buf, size_t len) {
    uint8_t lrc = 0;
    for (size_t i = 0; i < len; ++i) {
        lrc ^= buf[i];
    }
    return lrc;
}

uint16_t t1_crc(const uint8_t *buf, size_t len) {
    // ISO/IEC 13239 (a.k.a. CRC-CCITT, polynomial 0x1021), initial 0xFFFF,
    // no final XOR, MSB-first.
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(buf[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

static t1_status_t append_edc(uint8_t *out, size_t prologue_and_inf_len,
                              bool use_crc, size_t out_cap, size_t *out_len) {
    const size_t edc_len = use_crc ? 2 : 1;
    if (prologue_and_inf_len + edc_len > out_cap) {
        return T1_ERR_OUT_BUF;
    }
    if (use_crc) {
        uint16_t crc = t1_crc(out, prologue_and_inf_len);
        out[prologue_and_inf_len]     = static_cast<uint8_t>((crc >> 8) & 0xFF);
        out[prologue_and_inf_len + 1] = static_cast<uint8_t>(crc & 0xFF);
    } else {
        out[prologue_and_inf_len] = t1_lrc(out, prologue_and_inf_len);
    }
    *out_len = prologue_and_inf_len + edc_len;
    return T1_OK;
}

t1_status_t t1_block_encode_i(uint8_t nad, uint8_t ns, bool more,
                              const uint8_t *inf, uint8_t inf_len,
                              bool use_crc,
                              uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!out || !out_len) return T1_ERR_NULL;
    if (inf_len > T1_INF_MAX) return T1_ERR_INF_TOO_BIG;
    if (inf_len > 0 && !inf) return T1_ERR_NULL;
    if (ns > 1) return T1_ERR_PCB;

    const size_t prologue = 3;
    if (prologue + inf_len > out_cap) {
        return T1_ERR_OUT_BUF;
    }

    uint8_t pcb = kPcbIBlock;
    if (ns) pcb |= kPcbINs;
    if (more) pcb |= kPcbIMore;

    out[0] = nad;
    out[1] = pcb;
    out[2] = inf_len;
    if (inf_len > 0) {
        memcpy(out + prologue, inf, inf_len);
    }

    return append_edc(out, prologue + inf_len, use_crc, out_cap, out_len);
}

t1_status_t t1_block_encode_r(uint8_t nad, uint8_t nr, t1_r_error_t err,
                              bool use_crc,
                              uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!out || !out_len) return T1_ERR_NULL;
    if (nr > 1) return T1_ERR_PCB;
    if (static_cast<uint8_t>(err) > 0x02) return T1_ERR_PCB;

    if (3 > out_cap) return T1_ERR_OUT_BUF;

    uint8_t pcb = kPcbRBlock;
    if (nr) pcb |= kPcbRNr;
    pcb |= static_cast<uint8_t>(err);

    out[0] = nad;
    out[1] = pcb;
    out[2] = 0;

    return append_edc(out, 3, use_crc, out_cap, out_len);
}

t1_status_t t1_block_encode_s(uint8_t nad, t1_s_subtype_t subtype,
                              const uint8_t *inf, uint8_t inf_len,
                              bool use_crc,
                              uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!out || !out_len) return T1_ERR_NULL;
    if (inf_len > T1_INF_MAX) return T1_ERR_INF_TOO_BIG;
    if (inf_len > 0 && !inf) return T1_ERR_NULL;
    if (static_cast<uint8_t>(subtype) & ~kPcbSSubtypeMask) return T1_ERR_PCB;

    const size_t prologue = 3;
    if (prologue + inf_len > out_cap) {
        return T1_ERR_OUT_BUF;
    }

    out[0] = nad;
    out[1] = static_cast<uint8_t>(kPcbSBlock | static_cast<uint8_t>(subtype));
    out[2] = inf_len;
    if (inf_len > 0) {
        memcpy(out + prologue, inf, inf_len);
    }

    return append_edc(out, prologue + inf_len, use_crc, out_cap, out_len);
}

t1_status_t t1_block_decode(const uint8_t *buf, size_t buf_len, bool use_crc,
                            t1_block_t *out) {
    if (!buf || !out) return T1_ERR_NULL;
    if (buf_len < 4) return T1_ERR_SHORT;          // NAD + PCB + LEN + at least EDC byte
    if (use_crc && buf_len < 5) return T1_ERR_SHORT;

    const uint8_t len_byte = buf[2];
    if (len_byte == T1_LEN_RESERVED) return T1_ERR_LEN;

    const size_t edc_len = use_crc ? 2 : 1;
    const size_t expected_total = static_cast<size_t>(3) + len_byte + edc_len;
    if (buf_len < expected_total) return T1_ERR_LEN;

    // Validate EDC over [NAD .. INF].
    if (use_crc) {
        uint16_t expected = t1_crc(buf, 3 + len_byte);
        uint16_t got = static_cast<uint16_t>((buf[3 + len_byte] << 8) | buf[3 + len_byte + 1]);
        out->edc_ok = (expected == got);
    } else {
        uint8_t expected = t1_lrc(buf, 3 + len_byte);
        out->edc_ok = (expected == buf[3 + len_byte]);
    }
    out->use_crc = use_crc;

    out->nad = buf[0];
    out->pcb = buf[1];
    out->inf_len = len_byte;
    out->inf = (len_byte > 0) ? buf + 3 : nullptr;

    const uint8_t pcb = out->pcb;
    const uint8_t kind_bits = pcb & kPcbBlockKindMask;

    if ((pcb & 0x80) == 0) {
        // I-block.
        if (pcb & kPcbIReservedMask) return T1_ERR_PCB;
        out->kind = T1_BLOCK_I;
        out->i_ns = (pcb & kPcbINs) ? 1 : 0;
        out->i_more = (pcb & kPcbIMore) != 0;
    } else if (kind_bits == kPcbRBlock) {
        if (pcb & kPcbRReservedMask) return T1_ERR_PCB;
        if (len_byte != 0) return T1_ERR_LEN;
        out->kind = T1_BLOCK_R;
        out->r_nr = (pcb & kPcbRNr) ? 1 : 0;
        out->r_err = static_cast<t1_r_error_t>(pcb & kPcbRErrMask);
    } else {
        out->kind = T1_BLOCK_S;
        out->s_subtype = static_cast<t1_s_subtype_t>(pcb & kPcbSSubtypeMask);
    }

    return T1_OK;
}
