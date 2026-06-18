/**
 * \file
 * \brief Host test for the OpenPGP KDF-DO (tag F9) byte codec.
 *
 * Compiles the real firmware source (kdf.cpp) on the host and exercises
 * parse / build / round-trip / disabled / rejection paths. A drift in the
 * codec breaks this test.
 */

#include "../../../components/mod_gpg/src/openpgp/kdf.cpp"

#include <cstdint>
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/// build_disabled emits the canonical "KDF off" body 81 01 00.
static void test_build_disabled(void) {
    uint8_t out[8];
    size_t len = 0;
    TEST_ASSERT_EQUAL(KDF_OK, kdf_do_build_disabled(out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_size_t(3, len);
    const uint8_t expect[3] = {0x81, 0x01, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, out, 3);
}

/// A disabled body parses back to KDF_ALGO_NONE.
static void test_parse_disabled(void) {
    const uint8_t in[3] = {0x81, 0x01, 0x00};
    kdf_do_t kdf;
    TEST_ASSERT_EQUAL(KDF_OK, kdf_do_parse(in, sizeof(in), &kdf));
    TEST_ASSERT_EQUAL(KDF_ALGO_NONE, kdf.algo);
}

/// An empty DO is accepted as disabled.
static void test_parse_empty(void) {
    const uint8_t empty[1] = {0};
    kdf_do_t kdf;
    TEST_ASSERT_EQUAL(KDF_OK, kdf_do_parse(empty, 0, &kdf));
    TEST_ASSERT_EQUAL(KDF_ALGO_NONE, kdf.algo);
}

/// A full PBKDF2 config round-trips through build -> parse unchanged.
static void test_roundtrip_pbkdf2(void) {
    kdf_do_t in;
    kdf_do_clear(&in);
    in.algo = KDF_ALGO_PBKDF2;
    in.hash = KDF_HASH_SHA256;
    in.iter_count = 100000;
    in.has_pw1_salt = true;
    for (int i = 0; i < KDF_SALT_LEN; i++) in.pw1_salt[i] = (uint8_t)(0x10 + i);
    in.has_pw3_salt = true;
    for (int i = 0; i < KDF_SALT_LEN; i++) in.pw3_salt[i] = (uint8_t)(0x30 + i);
    in.has_pw1_initial = true;
    in.pw1_initial_len = 32;
    for (int i = 0; i < 32; i++) in.pw1_initial[i] = (uint8_t)(0x40 + i);

    uint8_t buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(KDF_OK, kdf_do_build(&in, buf, sizeof(buf), &len));
    TEST_ASSERT_TRUE(len > 0);

    kdf_do_t out;
    TEST_ASSERT_EQUAL(KDF_OK, kdf_do_parse(buf, len, &out));
    TEST_ASSERT_EQUAL(KDF_ALGO_PBKDF2, out.algo);
    TEST_ASSERT_EQUAL(KDF_HASH_SHA256, out.hash);
    TEST_ASSERT_EQUAL_UINT32(100000, out.iter_count);
    TEST_ASSERT_TRUE(out.has_pw1_salt);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.pw1_salt, out.pw1_salt, KDF_SALT_LEN);
    TEST_ASSERT_TRUE(out.has_pw3_salt);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.pw3_salt, out.pw3_salt, KDF_SALT_LEN);
    TEST_ASSERT_TRUE(out.has_pw1_initial);
    TEST_ASSERT_EQUAL_UINT8(32, out.pw1_initial_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.pw1_initial, out.pw1_initial, 32);
}

/// SHA-512 initial hash (64 bytes) round-trips.
static void test_roundtrip_sha512(void) {
    kdf_do_t in;
    kdf_do_clear(&in);
    in.algo = KDF_ALGO_PBKDF2;
    in.hash = KDF_HASH_SHA512;
    in.iter_count = 200000;
    in.has_pw3_initial = true;
    in.pw3_initial_len = 64;
    for (int i = 0; i < 64; i++) in.pw3_initial[i] = (uint8_t)i;

    uint8_t buf[160];
    size_t len = 0;
    TEST_ASSERT_EQUAL(KDF_OK, kdf_do_build(&in, buf, sizeof(buf), &len));
    kdf_do_t out;
    TEST_ASSERT_EQUAL(KDF_OK, kdf_do_parse(buf, len, &out));
    TEST_ASSERT_EQUAL(KDF_HASH_SHA512, out.hash);
    TEST_ASSERT_EQUAL_UINT8(64, out.pw3_initial_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.pw3_initial, out.pw3_initial, 64);
}

/// Unknown inner tags are rejected (no silent accept of malformed input).
static void test_reject_bad_tag(void) {
    const uint8_t in[] = {0x81, 0x01, 0x03, 0x8F, 0x01, 0x00};
    kdf_do_t kdf;
    TEST_ASSERT_EQUAL(KDF_ERR_BAD_TAG, kdf_do_parse(in, sizeof(in), &kdf));
}

/// A bad algorithm byte is rejected.
static void test_reject_bad_algo(void) {
    const uint8_t in[] = {0x81, 0x01, 0x07};
    kdf_do_t kdf;
    TEST_ASSERT_EQUAL(KDF_ERR_BAD_ALGO, kdf_do_parse(in, sizeof(in), &kdf));
}

/// An out-of-range salt length is rejected.
static void test_reject_bad_salt_len(void) {
    const uint8_t in[] = {0x81, 0x01, 0x03, 0x84, 0x04, 0x01, 0x02, 0x03, 0x04};
    kdf_do_t kdf;
    TEST_ASSERT_EQUAL(KDF_ERR_BAD_LENGTH, kdf_do_parse(in, sizeof(in), &kdf));
}

/// An initial-hash length that is neither 32 nor 64 is rejected.
static void test_reject_bad_initial_len(void) {
    uint8_t in[3 + 2 + 16];
    size_t p = 0;
    in[p++] = 0x81; in[p++] = 0x01; in[p++] = 0x03;
    in[p++] = 0x87; in[p++] = 16;
    for (int i = 0; i < 16; i++) in[p++] = 0;
    kdf_do_t kdf;
    TEST_ASSERT_EQUAL(KDF_ERR_BAD_HASH_LEN, kdf_do_parse(in, p, &kdf));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_build_disabled);
    RUN_TEST(test_parse_disabled);
    RUN_TEST(test_parse_empty);
    RUN_TEST(test_roundtrip_pbkdf2);
    RUN_TEST(test_roundtrip_sha512);
    RUN_TEST(test_reject_bad_tag);
    RUN_TEST(test_reject_bad_algo);
    RUN_TEST(test_reject_bad_salt_len);
    RUN_TEST(test_reject_bad_initial_len);
    return UNITY_END();
}
