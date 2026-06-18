/**
 * \file
 * \brief Host unit test for the authenticatorLargeBlobs write-session logic.
 *
 * Direct unit test: compiles the REAL firmware source
 * components/mod_fido2/src/LargeBlobStore.cpp on the host. The source has no
 * non-portable dependencies; the neighbouring mod_fido2/LargeBlobStore.h shim
 * resolves the header quote-include. The canonical empty-array bytes are a
 * known-answer vector (0x80 || LEFT(SHA-256(0x80), 16)).
 */

#include "../../../components/mod_fido2/src/LargeBlobStore.cpp"

#include <cstdint>
#include <cstring>
#include <unity.h>

using cdc::mod_fido2::LargeBlobWriteSession;
using cdc::mod_fido2::LbResult;
using cdc::mod_fido2::kLargeBlobEmpty;
using cdc::mod_fido2::kLargeBlobEmptyLen;
using cdc::mod_fido2::kLargeBlobMaxArray;

void setUp(void) {}
void tearDown(void) {}

// --- Canonical empty large-blob array (known-answer vector) ---

static void test_empty_array_constant(void) {
    // 0x80 (CBOR []) followed by the first 16 bytes of SHA-256(0x80).
    static const uint8_t expected[17] = {
        0x80,
        0x76, 0xbe, 0x8b, 0x52, 0x8d, 0x00, 0x75, 0xf7,
        0xaa, 0xe9, 0x8d, 0x6f, 0xa5, 0x7a, 0x6d, 0x3c,
    };
    TEST_ASSERT_EQUAL_UINT16(17, kLargeBlobEmptyLen);
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(kLargeBlobEmpty, expected, sizeof(expected)));
}

// --- begin() length and storage bounds ---

static void test_begin_bounds(void) {
    uint8_t buf[kLargeBlobMaxArray];
    LargeBlobWriteSession s;

    // Below the 17-byte minimum.
    s.reset();
    TEST_ASSERT_EQUAL_INT((int)LbResult::BadLength, (int)s.begin(buf, sizeof(buf), 16));
    TEST_ASSERT_FALSE(s.active());

    // Above the 1024-byte ceiling.
    s.reset();
    TEST_ASSERT_EQUAL_INT((int)LbResult::StorageFull,
                          (int)s.begin(buf, sizeof(buf), kLargeBlobMaxArray + 1));

    // Exceeds the buffer capacity even when within the protocol ceiling.
    s.reset();
    TEST_ASSERT_EQUAL_INT((int)LbResult::StorageFull, (int)s.begin(buf, 64, 100));

    // Valid extremes.
    s.reset();
    TEST_ASSERT_EQUAL_INT((int)LbResult::Ok, (int)s.begin(buf, sizeof(buf), 17));
    s.reset();
    TEST_ASSERT_EQUAL_INT((int)LbResult::Ok,
                          (int)s.begin(buf, sizeof(buf), kLargeBlobMaxArray));
    TEST_ASSERT_TRUE(s.active());
    TEST_ASSERT_EQUAL_UINT16(0, s.nextOffset());
}

// --- Single-fragment write completes and preserves bytes ---

static void test_single_fragment(void) {
    uint8_t buf[kLargeBlobMaxArray];
    LargeBlobWriteSession s;
    uint8_t payload[40];
    for (int i = 0; i < 40; i++) payload[i] = (uint8_t)(i + 1);

    TEST_ASSERT_EQUAL_INT((int)LbResult::Ok, (int)s.begin(buf, sizeof(buf), 40));
    TEST_ASSERT_FALSE(s.complete());
    TEST_ASSERT_EQUAL_INT((int)LbResult::Ok, (int)s.append(0, payload, 40));
    TEST_ASSERT_TRUE(s.complete());
    TEST_ASSERT_EQUAL_UINT16(40, s.length());
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(s.data(), payload, 40));
}

// --- Multi-fragment write in order ---

static void test_multi_fragment(void) {
    uint8_t buf[kLargeBlobMaxArray];
    LargeBlobWriteSession s;
    uint8_t a[20], b[20];
    for (int i = 0; i < 20; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)(100 + i); }

    TEST_ASSERT_EQUAL_INT((int)LbResult::Ok, (int)s.begin(buf, sizeof(buf), 40));
    TEST_ASSERT_EQUAL_INT((int)LbResult::Ok, (int)s.append(0, a, 20));
    TEST_ASSERT_FALSE(s.complete());
    TEST_ASSERT_EQUAL_UINT16(20, s.nextOffset());
    TEST_ASSERT_EQUAL_INT((int)LbResult::Ok, (int)s.append(20, b, 20));
    TEST_ASSERT_TRUE(s.complete());
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(s.data(), a, 20));
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(s.data() + 20, b, 20));
}

// --- Sequence and bounds errors ---

static void test_sequence_errors(void) {
    uint8_t buf[kLargeBlobMaxArray];
    LargeBlobWriteSession s;
    uint8_t chunk[20] = {0};

    // No active session -> BadSeq.
    s.reset();
    TEST_ASSERT_EQUAL_INT((int)LbResult::BadSeq, (int)s.append(0, chunk, 20));

    // Wrong starting offset -> BadSeq.
    TEST_ASSERT_EQUAL_INT((int)LbResult::Ok, (int)s.begin(buf, sizeof(buf), 40));
    TEST_ASSERT_EQUAL_INT((int)LbResult::BadSeq, (int)s.append(20, chunk, 20));

    // Gap after a valid first fragment -> BadSeq.
    TEST_ASSERT_EQUAL_INT((int)LbResult::Ok, (int)s.append(0, chunk, 20));
    TEST_ASSERT_EQUAL_INT((int)LbResult::BadSeq, (int)s.append(40, chunk, 20));

    // Fragment overruns the declared total -> BadLength (20 > 18).
    s.reset();
    TEST_ASSERT_EQUAL_INT((int)LbResult::Ok, (int)s.begin(buf, sizeof(buf), 18));
    TEST_ASSERT_EQUAL_INT((int)LbResult::BadLength, (int)s.append(0, chunk, 20));
}

// --- reset() clears in-progress state ---

static void test_reset(void) {
    uint8_t buf[kLargeBlobMaxArray];
    LargeBlobWriteSession s;
    uint8_t chunk[10] = {0};

    TEST_ASSERT_EQUAL_INT((int)LbResult::Ok, (int)s.begin(buf, sizeof(buf), 20));
    TEST_ASSERT_EQUAL_INT((int)LbResult::Ok, (int)s.append(0, chunk, 10));
    s.reset();
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_EQUAL_INT((int)LbResult::BadSeq, (int)s.append(10, chunk, 10));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_array_constant);
    RUN_TEST(test_begin_bounds);
    RUN_TEST(test_single_fragment);
    RUN_TEST(test_multi_fragment);
    RUN_TEST(test_sequence_errors);
    RUN_TEST(test_reset);
    return UNITY_END();
}
