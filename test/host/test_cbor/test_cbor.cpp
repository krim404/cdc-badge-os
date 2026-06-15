/**
 * \file
 * \brief Host unit test for the firmware CBOR encoder/reader (RFC 8949).
 *
 * Direct unit test: compiles the REAL firmware source
 * components/mod_fido2/src/cbor_helpers.cpp on the host. Its only non-portable
 * dependency is cdc_log.h (used for an overflow LOG_E); the neighbouring stub
 * cdc_log.h satisfies it, and the mod_fido2/cbor_helpers.h shim resolves the
 * header quote-include. Known-answer vectors are taken from RFC 8949 Appendix A.
 */

#include "../../../components/mod_fido2/src/cbor_helpers.cpp"

#include <cstdint>
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/// Asserts the writer produced exactly the expected bytes with no error.
static void assertEncoded(const cbor_writer_t* w, const uint8_t* expect, size_t n) {
    TEST_ASSERT_FALSE(cbor_writer_error(w));
    TEST_ASSERT_EQUAL_size_t(n, cbor_writer_length(w));
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(w->buffer, expect, n));
}

// --- Unsigned integer minimal-length encoding (RFC 8949 A) ---

static void test_encode_uint(void) {
    uint8_t buf[16];
    struct { uint64_t v; uint8_t out[9]; size_t n; } cases[] = {
        {0,            {0x00},                                      1},
        {10,           {0x0A},                                      1},
        {23,           {0x17},                                      1},  // last 1-byte
        {24,           {0x18, 0x18},                                2},  // first uint8
        {25,           {0x18, 0x19},                                2},
        {100,          {0x18, 0x64},                                2},
        {255,          {0x18, 0xFF},                                2},
        {256,          {0x19, 0x01, 0x00},                          3},
        {1000,         {0x19, 0x03, 0xE8},                          3},
        {65535,        {0x19, 0xFF, 0xFF},                          3},
        {65536,        {0x1A, 0x00, 0x01, 0x00, 0x00},              5},
        {1000000,      {0x1A, 0x00, 0x0F, 0x42, 0x40},              5},
        {4294967296ull,{0x1B,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00}, 9},
    };
    for (const auto& c : cases) {
        cbor_writer_t w; cbor_writer_init(&w, buf, sizeof(buf));
        cbor_encode_uint(&w, c.v);
        assertEncoded(&w, c.out, c.n);
    }
}

// --- Signed integer (negative uses major type 1) ---

static void test_encode_int(void) {
    uint8_t buf[16];
    struct { int64_t v; uint8_t out[9]; size_t n; } cases[] = {
        {0,    {0x00},        1},
        {-1,   {0x20},        1},
        {-10,  {0x29},        1},
        {-24,  {0x37},        1},  // last 1-byte negative
        {-25,  {0x38, 0x18},  2},
        {-100, {0x38, 0x63},  2},
        {-1000,{0x39, 0x03, 0xE7}, 3},
        {100,  {0x18, 0x64},  2},  // positive routed through unsigned
    };
    for (const auto& c : cases) {
        cbor_writer_t w; cbor_writer_init(&w, buf, sizeof(buf));
        cbor_encode_int(&w, c.v);
        assertEncoded(&w, c.out, c.n);
    }
}

// --- Byte and text strings ---

static void test_encode_bytes(void) {
    uint8_t buf[16];
    cbor_writer_t w; cbor_writer_init(&w, buf, sizeof(buf));
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    cbor_encode_bytes(&w, data, sizeof(data));
    const uint8_t exp[] = {0x44, 0x01, 0x02, 0x03, 0x04};  // bytes(4)
    assertEncoded(&w, exp, sizeof(exp));

    cbor_writer_t w2; cbor_writer_init(&w2, buf, sizeof(buf));
    cbor_encode_bytes(&w2, nullptr, 0);
    const uint8_t empty[] = {0x40};  // bytes(0)
    assertEncoded(&w2, empty, sizeof(empty));
}

static void test_encode_text(void) {
    uint8_t buf[16];
    cbor_writer_t w; cbor_writer_init(&w, buf, sizeof(buf));
    cbor_encode_text(&w, "a");
    const uint8_t a[] = {0x61, 0x61};  // text(1) "a"
    assertEncoded(&w, a, sizeof(a));

    cbor_writer_t w2; cbor_writer_init(&w2, buf, sizeof(buf));
    cbor_encode_text(&w2, "IETF");
    const uint8_t ietf[] = {0x64, 'I', 'E', 'T', 'F'};  // text(4)
    assertEncoded(&w2, ietf, sizeof(ietf));
}

// --- Simple values ---

static void test_encode_simple(void) {
    uint8_t buf[8];
    cbor_writer_t w; cbor_writer_init(&w, buf, sizeof(buf));
    cbor_encode_bool(&w, false);
    cbor_encode_bool(&w, true);
    cbor_encode_null(&w);
    const uint8_t exp[] = {0xF4, 0xF5, 0xF6};
    assertEncoded(&w, exp, sizeof(exp));
}

// --- Arrays and maps (definite length headers) ---

static void test_encode_array(void) {
    uint8_t buf[16];
    cbor_writer_t w; cbor_writer_init(&w, buf, sizeof(buf));
    cbor_encode_array(&w, 3);
    cbor_encode_uint(&w, 1);
    cbor_encode_uint(&w, 2);
    cbor_encode_uint(&w, 3);
    const uint8_t exp[] = {0x83, 0x01, 0x02, 0x03};  // [1,2,3]
    assertEncoded(&w, exp, sizeof(exp));
}

static void test_encode_map(void) {
    uint8_t buf[16];
    cbor_writer_t w; cbor_writer_init(&w, buf, sizeof(buf));
    cbor_encode_map(&w, 2);
    cbor_encode_uint(&w, 1);
    cbor_encode_uint(&w, 2);
    cbor_encode_uint(&w, 3);
    cbor_encode_uint(&w, 4);
    const uint8_t exp[] = {0xA2, 0x01, 0x02, 0x03, 0x04};  // {1:2,3:4}
    assertEncoded(&w, exp, sizeof(exp));

    // Map with text keys: {"a":1,"b":[2,3]} from RFC 8949 A.
    cbor_writer_t w2; cbor_writer_init(&w2, buf, sizeof(buf));
    cbor_encode_map(&w2, 2);
    cbor_encode_text(&w2, "a");
    cbor_encode_uint(&w2, 1);
    cbor_encode_text(&w2, "b");
    cbor_encode_array(&w2, 2);
    cbor_encode_uint(&w2, 2);
    cbor_encode_uint(&w2, 3);
    const uint8_t exp2[] = {0xA2, 0x61, 'a', 0x01, 0x61, 'b', 0x82, 0x02, 0x03};
    assertEncoded(&w2, exp2, sizeof(exp2));
}

// --- Writer overflow sets the error flag ---

static void test_encode_overflow(void) {
    uint8_t buf[2];
    cbor_writer_t w; cbor_writer_init(&w, buf, sizeof(buf));
    cbor_encode_uint(&w, 1000000);  // needs 5 bytes, only 2 available
    TEST_ASSERT_TRUE(cbor_writer_error(&w));
}

// --- Reader round-trips against the encoder ---

static void test_reader_roundtrip(void) {
    uint8_t buf[64];
    cbor_writer_t w; cbor_writer_init(&w, buf, sizeof(buf));
    cbor_encode_map(&w, 3);
    cbor_encode_uint(&w, 1);
    cbor_encode_int(&w, -7);
    cbor_encode_uint(&w, 2);
    cbor_encode_text(&w, "hi");
    cbor_encode_uint(&w, 3);
    const uint8_t blob[] = {0xDE, 0xAD, 0xBE, 0xEF};
    cbor_encode_bytes(&w, blob, sizeof(blob));
    TEST_ASSERT_FALSE(cbor_writer_error(&w));

    cbor_reader_t r; cbor_reader_init(&r, buf, cbor_writer_length(&w));
    TEST_ASSERT_EQUAL_INT(3, cbor_read_map(&r));

    uint64_t key = 0; int64_t sv = 0;
    TEST_ASSERT_TRUE(cbor_read_uint(&r, &key));
    TEST_ASSERT_EQUAL_UINT64(1u, key);
    TEST_ASSERT_TRUE(cbor_read_int(&r, &sv));
    TEST_ASSERT_EQUAL_INT64(-7, sv);

    TEST_ASSERT_TRUE(cbor_read_uint(&r, &key));
    TEST_ASSERT_EQUAL_UINT64(2u, key);
    char txt[8]; size_t txtLen = 0;
    TEST_ASSERT_TRUE(cbor_read_text(&r, txt, sizeof(txt), &txtLen));
    TEST_ASSERT_EQUAL_size_t(2u, txtLen);
    TEST_ASSERT_EQUAL_STRING("hi", txt);

    TEST_ASSERT_TRUE(cbor_read_uint(&r, &key));
    TEST_ASSERT_EQUAL_UINT64(3u, key);
    uint8_t out[8]; size_t outLen = 0;
    TEST_ASSERT_TRUE(cbor_read_bytes(&r, out, sizeof(out), &outLen));
    TEST_ASSERT_EQUAL_size_t(4u, outLen);
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(out, blob, sizeof(blob)));

    TEST_ASSERT_FALSE(cbor_reader_error(&r));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_encode_uint);
    RUN_TEST(test_encode_int);
    RUN_TEST(test_encode_bytes);
    RUN_TEST(test_encode_text);
    RUN_TEST(test_encode_simple);
    RUN_TEST(test_encode_array);
    RUN_TEST(test_encode_map);
    RUN_TEST(test_encode_overflow);
    RUN_TEST(test_reader_roundtrip);
    return UNITY_END();
}
