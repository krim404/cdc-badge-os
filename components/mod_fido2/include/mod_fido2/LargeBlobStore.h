/**
 * \file
 * \brief Portable authenticatorLargeBlobs write-session accumulator and the
 *        canonical empty large-blob array constant (CTAP2.1 Section 6.10).
 *
 * Pure logic with no crypto or persistence so it can be unit-tested on the
 * host. The session writes into a caller-owned buffer (so the large staging
 * buffer can live in PSRAM); the caller verifies the trailing SHA-256 checksum
 * once \ref LargeBlobWriteSession::complete is true and then persists it.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::mod_fido2 {

/// Largest serialized large-blob array, advertised as maxSerializedLargeBlobArray.
inline constexpr uint16_t kLargeBlobMaxArray = 1024;

/// Length of the canonical empty large-blob array: 0x80 followed by the left
/// 16 bytes of SHA-256(0x80).
inline constexpr uint16_t kLargeBlobEmptyLen = 17;

/// Canonical empty large-blob array bytes.
extern const uint8_t kLargeBlobEmpty[kLargeBlobEmptyLen];

/// Outcome of a write-session step, mapped to a CTAP status by the caller.
enum class LbResult : uint8_t {
    Ok,
    StorageFull,  ///< declared total length exceeds the buffer / kLargeBlobMaxArray
    BadLength,    ///< declared total too small, or a fragment overruns the total
    BadSeq,       ///< fragment offset out of order, or no active session
};

/**
 * \brief Accumulates an offset-chunked authenticatorLargeBlobs write into a
 *        caller-owned buffer.
 */
class LargeBlobWriteSession {
public:
    /// Discards any partial write and returns to the idle state.
    void reset();

    /**
     * \brief Begins a write declaring the full serialized length.
     * \param buffer Destination buffer owned by the caller.
     * \param capacity Capacity of \p buffer in bytes.
     * \param totalLength Declared array length, valid range 17..min(capacity,1024).
     * \return Ok, StorageFull (too large), or BadLength (< 17).
     */
    LbResult begin(uint8_t* buffer, uint16_t capacity, uint32_t totalLength);

    /**
     * \brief Appends one fragment at \p offset.
     * \param offset Byte offset of this fragment; must equal \ref nextOffset.
     * \param data Fragment bytes (may be null when \p len is 0).
     * \param len Fragment length.
     * \return Ok, BadSeq (no session or wrong offset), or BadLength (overrun).
     */
    LbResult append(uint32_t offset, const uint8_t* data, uint16_t len);

    /// True once the declared length has been fully received.
    bool complete() const { return active_ && received_ == expected_; }
    /// True while a write is in progress.
    bool active() const { return active_; }
    /// Offset the next fragment must start at.
    uint16_t nextOffset() const { return received_; }
    /// Pointer to the accumulated buffer (valid after \ref begin).
    const uint8_t* data() const { return buffer_; }
    /// Declared total length of the current write.
    uint16_t length() const { return expected_; }

private:
    uint8_t* buffer_ = nullptr;
    uint16_t capacity_ = 0;
    uint16_t expected_ = 0;
    uint16_t received_ = 0;
    bool active_ = false;
};

}  // namespace cdc::mod_fido2
