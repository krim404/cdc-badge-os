/**
 * \file
 * \brief authenticatorLargeBlobs write-session accumulator and the canonical
 *        empty large-blob array constant.
 */

#include "mod_fido2/LargeBlobStore.h"

#include <cstring>

namespace cdc::mod_fido2 {

// 0x80 (empty CBOR array) followed by the first 16 bytes of SHA-256(0x80).
const uint8_t kLargeBlobEmpty[kLargeBlobEmptyLen] = {
    0x80,
    0x76, 0xbe, 0x8b, 0x52, 0x8d, 0x00, 0x75, 0xf7,
    0xaa, 0xe9, 0x8d, 0x6f, 0xa5, 0x7a, 0x6d, 0x3c,
};

void LargeBlobWriteSession::reset() {
    buffer_ = nullptr;
    capacity_ = 0;
    expected_ = 0;
    received_ = 0;
    active_ = false;
}

LbResult LargeBlobWriteSession::begin(uint8_t* buffer, uint16_t capacity, uint32_t totalLength) {
    uint16_t limit = capacity < kLargeBlobMaxArray ? capacity : kLargeBlobMaxArray;
    if (totalLength > limit) {
        return LbResult::StorageFull;
    }
    if (totalLength < kLargeBlobEmptyLen) {
        return LbResult::BadLength;
    }
    buffer_ = buffer;
    capacity_ = capacity;
    expected_ = static_cast<uint16_t>(totalLength);
    received_ = 0;
    active_ = true;
    return LbResult::Ok;
}

LbResult LargeBlobWriteSession::append(uint32_t offset, const uint8_t* data, uint16_t len) {
    if (!active_) {
        return LbResult::BadSeq;
    }
    if (offset != received_) {
        return LbResult::BadSeq;
    }
    if (offset + static_cast<uint32_t>(len) > expected_) {
        return LbResult::BadLength;
    }
    if (len && data) {
        memcpy(buffer_ + offset, data, len);
    }
    received_ = static_cast<uint16_t>(received_ + len);
    return LbResult::Ok;
}

}  // namespace cdc::mod_fido2
