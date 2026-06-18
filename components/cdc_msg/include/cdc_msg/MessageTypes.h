#pragma once

#include <cstdint>

/**
 * \file MessageTypes.h
 * \brief Wire protocol constants, states, reasons and limits for the
 *        badge-to-badge message transfer framework.
 *
 * The transport is a lightweight, MIME-typed GATT profile modeled on the
 * Nordic UART Service pattern: a plaintext Control characteristic (sender ->
 * receiver), a plaintext Status notify characteristic (receiver -> sender) and
 * an encrypted Data characteristic (sender -> receiver). All multi-byte fields
 * are little-endian. Every field is bounds-checked against these limits before
 * use; wire data is never trusted.
 */

namespace cdc::msg {

/// Wire protocol version carried in the OFFER frame; a mismatch is rejected.
inline constexpr uint8_t kProtocolVersion = 1;

/// Control-characteristic opcodes (sender -> receiver), byte 0 of the frame.
enum class ControlOp : uint8_t {
    Offer = 0x01,  ///< Offer a typed payload (see OFFER layout below).
    Abort = 0x02,  ///< Sender aborts the in-flight transfer.
};

/// Data-characteristic opcodes (sender -> receiver), byte 0 of the frame.
enum class DataOp : uint8_t {
    Chunk    = 0x10,  ///< Payload chunk: [op][reserved][bytes...].
    Complete = 0x11,  ///< End of payload: [op][reserved][u32 crc32].
};

/// Status notification values (receiver -> sender), byte 0 of the notification.
enum class StatusOp : uint8_t {
    Accept   = 0x01,  ///< Receiver accepted; sender may start encryption.
    Decline  = 0x02,  ///< Receiver declined; byte 1 = Reason.
    Progress = 0x03,  ///< [op][u32 received] receive progress.
    Done     = 0x04,  ///< Payload received and delivered to a handler.
    Error    = 0x05,  ///< Transfer aborted; byte 1 = Reason.
    Busy     = 0x06,  ///< Receiver saturated; try again later.
    Queued   = 0x07,  ///< Offer queued behind an active transfer.
};

/// Reason codes carried by Decline/Error status notifications.
enum class Reason : uint8_t {
    None         = 0,
    NoHandler    = 1,  ///< No local module/plugin handles the offered MIME type.
    UserDeclined = 2,  ///< Local user rejected the consent prompt.
    TooLarge     = 3,  ///< totalLen exceeds kMaxPayloadBytes.
    Busy         = 4,  ///< A transfer is already in progress / queue full.
    Timeout      = 5,  ///< Consent or transfer timed out.
    BadFrame     = 6,  ///< Malformed / out-of-bounds frame.
    CrcMismatch  = 7,  ///< CRC32 or length mismatch at completion.
    Disconnected = 8,  ///< Peer disconnected mid-transfer.
    PairFailed   = 9,  ///< Numeric-comparison / encryption failed.
};

/// Sender (central) state machine.
enum class SendState : uint8_t {
    Idle, PickingPeer, Connecting, Discovering, Offering, AwaitingEncrypt, Streaming, Done, Failed,
};

/// Receiver (peripheral) active-session state machine.
enum class RecvState : uint8_t {
    Idle, AwaitingConsent, AwaitingEncrypt, Receiving, Delivering, Done, Failed,
};

// === Payload / framing limits =============================================

/// Largest payload accepted in one transfer. Larger OFFERs are declined before
/// any allocation. Matches HOST_MSG_PAYLOAD_MAX in the plugin host API.
inline constexpr uint32_t kMaxPayloadBytes = 4096;

/// MIME type buffer size including NUL (matches HOST_MSG_MIME_MAX).
inline constexpr uint8_t kMimeBufSize = 64;
/// Largest MIME string length on the wire / in the registry (excludes NUL).
inline constexpr uint8_t kMaxMimeLen = kMimeBufSize - 1;

/// Peer display-name buffer size including NUL (matches hal::BleScanResult::name).
inline constexpr uint8_t kNameBufSize = 32;
/// Largest peer-name string length on the wire (excludes NUL); fits in 5 bits.
inline constexpr uint8_t kMaxNameLen = kNameBufSize - 1;
/// Low 5 bits of the OFFER nameLen byte carry the name length (0..kMaxNameLen).
inline constexpr uint8_t kOfferNameLenMask = 0x1F;
/// Bit 7 of the OFFER nameLen byte: sender requests a session-persistent pairing.
inline constexpr uint8_t kOfferFlagPersist = 0x80;

/// OFFER fixed header length: op + ver + u32 totalLen + mimeLen + nameLen.
inline constexpr uint8_t kOfferHeaderLen = 8;
/// CHUNK fixed header length: op + reserved.
inline constexpr uint8_t kChunkHeaderLen = 2;
/// COMPLETE frame length: op + reserved + u32 crc32.
inline constexpr uint8_t kCompleteLen = 6;

// === Timeouts (evaluated in tick(), milliseconds) =========================

inline constexpr uint32_t kConnectTimeoutMs      = 10000;
inline constexpr uint32_t kDiscoveryTimeoutMs    = 5000;
inline constexpr uint32_t kConsentTimeoutMs      = 30000;
inline constexpr uint32_t kEncryptTimeoutMs      = 30000;  ///< Numeric comparison can take a while.
inline constexpr uint32_t kTransferIdleTimeoutMs = 8000;

// === Concurrency (pending-offer queue) ====================================

/// Pending inbound offers buffered behind the single active session.
inline constexpr uint8_t  kOfferQueueDepth    = 4;
/// How long a sender waits while QUEUED before timing out.
inline constexpr uint32_t kQueueWaitTimeoutMs = kConsentTimeoutMs * kOfferQueueDepth;

// === DoS / abuse resistance ===============================================

/// Global, address-independent prompt budget: at most this many consent
/// prompts within kPromptWindowMs, regardless of source. The authoritative
/// control, because BLE addresses rotate.
inline constexpr uint8_t  kMaxPromptsPerWindow = 5;
inline constexpr uint32_t kPromptWindowMs      = 30000;
/// Global quiet period after the user declines an offer.
inline constexpr uint32_t kQuietCooldownMs     = 20000;

/// Per-connection best-effort rate table (an optimization, not relied upon).
inline constexpr uint8_t  kRateTableSize      = 8;
inline constexpr uint8_t  kMaxOffersPerWindow = 3;
inline constexpr uint32_t kOfferWindowMs      = 10000;

/// Discovered peer summary returned by the discovery/scan API.
struct PeerInfo {
    char    name[kNameBufSize];
    uint8_t addr[6];
    uint8_t addrType;  ///< 0 = public, 1 = random
    int8_t  rssi;      ///< dBm
};

}  // namespace cdc::msg
