#include "piv_tlv.h"

#include <cstring>

namespace cdc::mod_piv {

namespace {

// Reads a BER tag at *p (bounded by end). Returns tag value (1 or 2 bytes) and
// advances *p, or returns false on malformed input.
bool readTag(const uint8_t** p, const uint8_t* end, uint32_t* tagOut) {
    if (*p >= end) return false;
    uint8_t first = *(*p)++;
    uint32_t tag = first;
    if ((first & 0x1F) == 0x1F) {
        // Multi-byte tag: subsequent bytes with bit 8 set continue the tag.
        // PIV tags are at most 2 bytes, so read exactly one continuation.
        if (*p >= end) return false;
        tag = (tag << 8) | *(*p)++;
    }
    *tagOut = tag;
    return true;
}

// Reads a BER length at *p (bounded by end). Advances *p on success.
bool readLen(const uint8_t** p, const uint8_t* end, size_t* lenOut) {
    if (*p >= end) return false;
    uint8_t first = *(*p)++;
    if (first < 0x80) {
        *lenOut = first;
        return true;
    }
    uint8_t nbytes = first & 0x7F;
    if (nbytes == 0 || nbytes > 3) return false;  // 0 = indefinite (unsupported)
    if (*p + nbytes > end) return false;
    size_t len = 0;
    for (uint8_t i = 0; i < nbytes; i++) {
        len = (len << 8) | *(*p)++;
    }
    *lenOut = len;
    return true;
}

} // namespace

bool tlvFind(const uint8_t* buf, size_t len, uint32_t tag,
             const uint8_t** valueOut, size_t* valueLenOut) {
    if (!buf) return false;
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    while (p < end) {
        uint32_t t = 0;
        if (!readTag(&p, end, &t)) return false;
        size_t l = 0;
        if (!readLen(&p, end, &l)) return false;
        if (p + l > end) return false;
        if (t == tag) {
            if (valueOut) *valueOut = p;
            if (valueLenOut) *valueLenOut = l;
            return true;
        }
        p += l;
    }
    return false;
}

size_t tlvWriteTag(uint8_t* buf, size_t cap, uint32_t tag) {
    if (tag <= 0xFF) {
        if (cap < 1) return 0;
        buf[0] = static_cast<uint8_t>(tag);
        return 1;
    }
    if (cap < 2) return 0;
    buf[0] = static_cast<uint8_t>((tag >> 8) & 0xFF);
    buf[1] = static_cast<uint8_t>(tag & 0xFF);
    return 2;
}

size_t tlvWriteLen(uint8_t* buf, size_t cap, size_t len) {
    if (len < 0x80) {
        if (cap < 1) return 0;
        buf[0] = static_cast<uint8_t>(len);
        return 1;
    }
    if (len <= 0xFF) {
        if (cap < 2) return 0;
        buf[0] = 0x81;
        buf[1] = static_cast<uint8_t>(len);
        return 2;
    }
    if (cap < 3) return 0;
    buf[0] = 0x82;
    buf[1] = static_cast<uint8_t>((len >> 8) & 0xFF);
    buf[2] = static_cast<uint8_t>(len & 0xFF);
    return 3;
}

bool tlvWrite(uint8_t* buf, size_t cap, size_t* pos, uint32_t tag,
              const uint8_t* value, size_t valueLen) {
    size_t p = *pos;
    size_t n = tlvWriteTag(buf + p, cap - p, tag);
    if (n == 0) return false;
    p += n;
    n = tlvWriteLen(buf + p, cap - p, valueLen);
    if (n == 0) return false;
    p += n;
    if (p + valueLen > cap) return false;
    if (valueLen && value) {
        memcpy(buf + p, value, valueLen);
    }
    p += valueLen;
    *pos = p;
    return true;
}

namespace {

// DER-encodes one 32-byte unsigned big-endian integer as an INTEGER (tag 0x02),
// stripping leading zeros and prepending 0x00 when the high bit is set.
size_t derInteger(const uint8_t* in32, uint8_t* out, size_t outCap) {
    size_t start = 0;
    while (start < 31 && in32[start] == 0x00) start++;  // keep at least one byte
    bool needPad = (in32[start] & 0x80) != 0;
    size_t contentLen = (32 - start) + (needPad ? 1 : 0);
    if (outCap < 2 + contentLen) return 0;
    size_t p = 0;
    out[p++] = 0x02;
    out[p++] = static_cast<uint8_t>(contentLen);
    if (needPad) out[p++] = 0x00;
    memcpy(out + p, in32 + start, 32 - start);
    p += (32 - start);
    return p;
}

} // namespace

size_t derEncodeEcdsaSig(const uint8_t rs[64], uint8_t* out, size_t outCap) {
    uint8_t body[80];
    size_t bp = 0;
    size_t n = derInteger(rs, body + bp, sizeof(body) - bp);
    if (n == 0) return 0;
    bp += n;
    n = derInteger(rs + 32, body + bp, sizeof(body) - bp);
    if (n == 0) return 0;
    bp += n;

    // SEQUENCE wrapper. Body is always < 0x80, so length is single-byte.
    if (outCap < 2 + bp) return 0;
    out[0] = 0x30;
    out[1] = static_cast<uint8_t>(bp);
    memcpy(out + 2, body, bp);
    return 2 + bp;
}

} // namespace cdc::mod_piv
