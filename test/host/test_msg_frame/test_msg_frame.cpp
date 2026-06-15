/**
 * \file
 * \brief Host test for the message-transfer OFFER framing bounds.
 *
 * The real validator (cdc::msg::MessageTransfer::onControlWrite) pulls in
 * IBluetoothController / ServiceRegistry / the receiver state machine, none of
 * which are host-portable; testing the symbol directly requires firmware
 * refactoring (RF-02), deferred.
 *
 * Instead this is a self-contained reimplementation of the documented OFFER
 * framing rules from MessageTransfer.cpp:onControlWrite / MessageTypes.h. The
 * REAL firmware limit/opcode constants are pulled in from MessageTypes.h (via
 * the neighbouring shim), so a constant drift in the firmware breaks this test.
 * The reimplemented bounds logic mirrors the firmware byte-for-byte:
 *
 *   OFFER layout (little-endian): [op=0x01][ver][u32 totalLen][mimeLen][nameLen]
 *                                 [mime bytes][name bytes]
 *   Reject (BadFrame) when: len < kOfferHeaderLen, ver != kProtocolVersion,
 *     mimeLen == 0, mimeLen > kMaxMimeLen, nameLen > kMaxNameLen,
 *     kOfferHeaderLen + mimeLen + nameLen > len.
 *   Reject (TooLarge) when: totalLen == 0 or totalLen > kMaxPayloadBytes.
 *   Opcode handling: 0x02 = Abort (handled, no parse), neither Offer nor Abort
 *     is ignored.
 */

#include "cdc_msg/MessageTypes.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <unity.h>

using namespace cdc::msg;

void setUp(void) {}
void tearDown(void) {}

/// Outcome of validating a control-characteristic frame, mirroring the
/// firmware's status replies for an OFFER.
enum class OfferResult {
    Ignored,    ///< Opcode is neither Offer nor Abort.
    Abort,      ///< Abort opcode.
    BadFrame,   ///< Decline(BadFrame): structural / bounds failure.
    TooLarge,   ///< Decline(TooLarge): totalLen out of range.
    Accepted,   ///< Passed all framing/bounds checks (handler step is external).
};

/// Reads a little-endian u32, mirroring MessageTransfer.cpp:rdU32.
static uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// Reimplementation of the OFFER bounds-first validation. Returns the framing
/// verdict; it stops before the (external) handler/rate-limit steps.
static OfferResult validateOffer(const uint8_t* data, uint32_t len) {
    if (len == 0) return OfferResult::Ignored;
    const uint8_t op = data[0];
    if (op == static_cast<uint8_t>(ControlOp::Abort)) return OfferResult::Abort;
    if (op != static_cast<uint8_t>(ControlOp::Offer)) return OfferResult::Ignored;

    if (len < kOfferHeaderLen) return OfferResult::BadFrame;

    const uint8_t  ver = data[1];
    const uint32_t totalLen = rdU32(&data[2]);
    const uint8_t  mimeLen = data[6];
    const uint8_t  nameLen = data[7];

    if (ver != kProtocolVersion || mimeLen == 0 || mimeLen > kMaxMimeLen ||
        nameLen > kMaxNameLen ||
        static_cast<uint32_t>(kOfferHeaderLen) + mimeLen + nameLen > len) {
        return OfferResult::BadFrame;
    }
    if (totalLen == 0 || totalLen > kMaxPayloadBytes) {
        return OfferResult::TooLarge;
    }
    return OfferResult::Accepted;
}

/// Builds a well-formed OFFER frame for the given parameters.
static std::vector<uint8_t> buildOffer(uint8_t ver, uint32_t totalLen,
                                       uint8_t mimeLen, uint8_t nameLen) {
    std::vector<uint8_t> f;
    f.push_back(static_cast<uint8_t>(ControlOp::Offer));
    f.push_back(ver);
    f.push_back(static_cast<uint8_t>(totalLen));
    f.push_back(static_cast<uint8_t>(totalLen >> 8));
    f.push_back(static_cast<uint8_t>(totalLen >> 16));
    f.push_back(static_cast<uint8_t>(totalLen >> 24));
    f.push_back(mimeLen);
    f.push_back(nameLen);
    for (uint8_t i = 0; i < mimeLen; ++i) f.push_back('a');
    for (uint8_t i = 0; i < nameLen; ++i) f.push_back('b');
    return f;
}

/// The firmware limit constants have the documented values.
static void test_limit_constants(void) {
    TEST_ASSERT_EQUAL_UINT32(4096u, kMaxPayloadBytes);
    TEST_ASSERT_EQUAL_UINT8(63u, kMaxMimeLen);   // mime <= 63 (64-byte buf incl NUL)
    TEST_ASSERT_EQUAL_UINT8(31u, kMaxNameLen);   // name <= 31 (32-byte buf incl NUL)
    TEST_ASSERT_EQUAL_UINT8(8u, kOfferHeaderLen);
    TEST_ASSERT_EQUAL_UINT8(1u, kProtocolVersion);
}

/// A minimal well-formed OFFER is accepted.
static void test_valid_offer_accepted(void) {
    auto f = buildOffer(kProtocolVersion, 1, 1, 0);
    TEST_ASSERT_TRUE(OfferResult::Accepted ==
                     validateOffer(f.data(), static_cast<uint32_t>(f.size())));

    auto f2 = buildOffer(kProtocolVersion, kMaxPayloadBytes, kMaxMimeLen, kMaxNameLen);
    TEST_ASSERT_TRUE(OfferResult::Accepted ==
                     validateOffer(f2.data(), static_cast<uint32_t>(f2.size())));
}

/// Opcode handling: Abort handled, unknown ignored, empty ignored.
static void test_opcodes(void) {
    uint8_t abort[8] = {static_cast<uint8_t>(ControlOp::Abort)};
    TEST_ASSERT_TRUE(OfferResult::Abort == validateOffer(abort, sizeof(abort)));

    uint8_t unknown[8] = {0x7F};
    TEST_ASSERT_TRUE(OfferResult::Ignored == validateOffer(unknown, sizeof(unknown)));

    TEST_ASSERT_TRUE(OfferResult::Ignored == validateOffer(nullptr, 0));
}

/// A short frame (below the fixed header) is a BadFrame.
static void test_short_header_rejected(void) {
    auto f = buildOffer(kProtocolVersion, 1, 1, 0);
    for (uint32_t l = 1; l < kOfferHeaderLen; ++l) {
        TEST_ASSERT_TRUE(OfferResult::BadFrame == validateOffer(f.data(), l));
    }
}

/// Protocol-version mismatch is a BadFrame.
static void test_version_mismatch_rejected(void) {
    auto f = buildOffer(static_cast<uint8_t>(kProtocolVersion + 1), 1, 1, 0);
    TEST_ASSERT_TRUE(OfferResult::BadFrame ==
                     validateOffer(f.data(), static_cast<uint32_t>(f.size())));
}

/// mimeLen bounds: 0 rejected, > kMaxMimeLen rejected, boundary accepted.
static void test_mime_len_bounds(void) {
    auto zero = buildOffer(kProtocolVersion, 1, 0, 0);
    TEST_ASSERT_TRUE(OfferResult::BadFrame ==
                     validateOffer(zero.data(), static_cast<uint32_t>(zero.size())));

    auto atMax = buildOffer(kProtocolVersion, 1, kMaxMimeLen, 0);
    TEST_ASSERT_TRUE(OfferResult::Accepted ==
                     validateOffer(atMax.data(), static_cast<uint32_t>(atMax.size())));

    // mimeLen = kMaxMimeLen + 1 (== 64). Provide enough bytes so only the
    // mimeLen-too-large rule fires, not the length-overflow rule.
    auto over = buildOffer(kProtocolVersion, 1, static_cast<uint8_t>(kMaxMimeLen + 1), 0);
    TEST_ASSERT_TRUE(OfferResult::BadFrame ==
                     validateOffer(over.data(), static_cast<uint32_t>(over.size())));
}

/// nameLen bounds: > kMaxNameLen rejected, boundary accepted, 0 allowed.
static void test_name_len_bounds(void) {
    auto atMax = buildOffer(kProtocolVersion, 1, 1, kMaxNameLen);
    TEST_ASSERT_TRUE(OfferResult::Accepted ==
                     validateOffer(atMax.data(), static_cast<uint32_t>(atMax.size())));

    auto over = buildOffer(kProtocolVersion, 1, 1, static_cast<uint8_t>(kMaxNameLen + 1));
    TEST_ASSERT_TRUE(OfferResult::BadFrame ==
                     validateOffer(over.data(), static_cast<uint32_t>(over.size())));
}

/// A frame that claims more mime/name bytes than it carries is a BadFrame.
static void test_truncated_payload_rejected(void) {
    auto f = buildOffer(kProtocolVersion, 1, 10, 5);
    // Truncate so the declared mime+name no longer fit.
    uint32_t shortLen = kOfferHeaderLen + 10 + 5 - 1;
    TEST_ASSERT_TRUE(OfferResult::BadFrame == validateOffer(f.data(), shortLen));
}

/// totalLen bounds: 0 and > kMaxPayloadBytes are TooLarge; boundary accepted.
static void test_total_len_bounds(void) {
    auto zero = buildOffer(kProtocolVersion, 0, 1, 0);
    TEST_ASSERT_TRUE(OfferResult::TooLarge ==
                     validateOffer(zero.data(), static_cast<uint32_t>(zero.size())));

    auto atMax = buildOffer(kProtocolVersion, kMaxPayloadBytes, 1, 0);
    TEST_ASSERT_TRUE(OfferResult::Accepted ==
                     validateOffer(atMax.data(), static_cast<uint32_t>(atMax.size())));

    auto over = buildOffer(kProtocolVersion, kMaxPayloadBytes + 1, 1, 0);
    TEST_ASSERT_TRUE(OfferResult::TooLarge ==
                     validateOffer(over.data(), static_cast<uint32_t>(over.size())));
}

/// Fixed frame lengths documented for CHUNK and COMPLETE.
static void test_data_frame_lengths(void) {
    TEST_ASSERT_EQUAL_UINT8(2u, kChunkHeaderLen);  // [op][reserved]
    TEST_ASSERT_EQUAL_UINT8(6u, kCompleteLen);     // [op][reserved][u32 crc32]
    TEST_ASSERT_EQUAL_UINT8(0x10, static_cast<uint8_t>(DataOp::Chunk));
    TEST_ASSERT_EQUAL_UINT8(0x11, static_cast<uint8_t>(DataOp::Complete));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_limit_constants);
    RUN_TEST(test_valid_offer_accepted);
    RUN_TEST(test_opcodes);
    RUN_TEST(test_short_header_rejected);
    RUN_TEST(test_version_mismatch_rejected);
    RUN_TEST(test_mime_len_bounds);
    RUN_TEST(test_name_len_bounds);
    RUN_TEST(test_truncated_payload_rejected);
    RUN_TEST(test_total_len_bounds);
    RUN_TEST(test_data_frame_lengths);
    return UNITY_END();
}
