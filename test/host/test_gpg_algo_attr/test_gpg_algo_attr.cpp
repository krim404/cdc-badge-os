/**
 * \file
 * \brief Host test for the OpenPGP algorithm-attribute codec, focused on the
 *        RSA paths added alongside the ECC support. Compiles the real
 *        firmware source (algo_attr.cpp) on the host.
 */

#include "../../../components/mod_gpg/src/openpgp/algo_attr.cpp"

#include <cstdint>
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/// Ed25519 SIG attribute parses to EdDSA + curve Ed25519.
static void test_parse_ed25519(void) {
    const uint8_t in[] = {0x16, 0x2B, 0x06, 0x01, 0x04, 0x01, 0xDA, 0x47, 0x0F, 0x01};
    algo_attr_t a;
    TEST_ASSERT_EQUAL(ALGO_ATTR_OK, algo_attr_parse(in, sizeof(in), &a));
    TEST_ASSERT_FALSE(a.is_rsa);
    TEST_ASSERT_EQUAL(ALGO_ATTR_ID_EDDSA, a.algo_id);
    TEST_ASSERT_EQUAL(ALGO_ATTR_CURVE_ED25519, a.curve);
    TEST_ASSERT_EQUAL(ALGO_ATTR_OK, algo_attr_validate_role(&a, ALGO_ATTR_ROLE_SIG));
    TEST_ASSERT_EQUAL(ALGO_ATTR_OK, algo_attr_validate_capability(&a, false));
}

/// P-256 ECDH is valid for DEC but not for SIG.
static void test_parse_p256_ecdh_role(void) {
    const uint8_t in[] = {0x12, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
    algo_attr_t a;
    TEST_ASSERT_EQUAL(ALGO_ATTR_OK, algo_attr_parse(in, sizeof(in), &a));
    TEST_ASSERT_EQUAL(ALGO_ATTR_CURVE_P256, a.curve);
    TEST_ASSERT_EQUAL(ALGO_ATTR_OK, algo_attr_validate_role(&a, ALGO_ATTR_ROLE_DEC));
    TEST_ASSERT_NOT_EQUAL(ALGO_ATTR_OK, algo_attr_validate_role(&a, ALGO_ATTR_ROLE_SIG));
}

/// RSA-2048 attribute parses; valid for any role; gated by rsa_supported.
static void test_parse_rsa2048(void) {
    const uint8_t in[] = {0x01, 0x08, 0x00, 0x00, 0x20, 0x00};
    algo_attr_t a;
    TEST_ASSERT_EQUAL(ALGO_ATTR_OK, algo_attr_parse(in, sizeof(in), &a));
    TEST_ASSERT_TRUE(a.is_rsa);
    TEST_ASSERT_EQUAL_UINT16(2048, a.rsa_n_bits);
    TEST_ASSERT_EQUAL_UINT16(32, a.rsa_e_bits);
    TEST_ASSERT_EQUAL_UINT8(0, a.rsa_import_fmt);
    TEST_ASSERT_EQUAL(ALGO_ATTR_OK, algo_attr_validate_role(&a, ALGO_ATTR_ROLE_SIG));
    TEST_ASSERT_EQUAL(ALGO_ATTR_OK, algo_attr_validate_role(&a, ALGO_ATTR_ROLE_DEC));
    TEST_ASSERT_EQUAL(ALGO_ATTR_OK, algo_attr_validate_capability(&a, true));
    TEST_ASSERT_EQUAL(ALGO_ATTR_ERR_BAD_RSA, algo_attr_validate_capability(&a, false));
}

/// RSA-4096 parses; an unsupported modulus size is rejected.
static void test_rsa_modulus_sizes(void) {
    const uint8_t ok4096[] = {0x01, 0x10, 0x00, 0x00, 0x11, 0x00};
    algo_attr_t a;
    TEST_ASSERT_EQUAL(ALGO_ATTR_OK, algo_attr_parse(ok4096, sizeof(ok4096), &a));
    TEST_ASSERT_EQUAL_UINT16(4096, a.rsa_n_bits);

    const uint8_t bad1024[] = {0x01, 0x04, 0x00, 0x00, 0x20, 0x00};
    TEST_ASSERT_EQUAL(ALGO_ATTR_ERR_BAD_RSA, algo_attr_parse(bad1024, sizeof(bad1024), &a));
}

/// RSA build -> parse round-trips the modulus / exponent sizes and format.
static void test_rsa_build_roundtrip(void) {
    algo_attr_t in;
    memset(&in, 0, sizeof(in));
    in.algo_id = ALGO_ATTR_ID_RSA;
    in.is_rsa = true;
    in.rsa_n_bits = 3072;
    in.rsa_e_bits = 17;
    in.rsa_import_fmt = 0x03;

    uint8_t buf[16];
    size_t len = 0;
    TEST_ASSERT_EQUAL(ALGO_ATTR_OK, algo_attr_build(&in, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_size_t(6, len);

    algo_attr_t out;
    TEST_ASSERT_EQUAL(ALGO_ATTR_OK, algo_attr_parse(buf, len, &out));
    TEST_ASSERT_TRUE(out.is_rsa);
    TEST_ASSERT_EQUAL_UINT16(3072, out.rsa_n_bits);
    TEST_ASSERT_EQUAL_UINT16(17, out.rsa_e_bits);
    TEST_ASSERT_EQUAL_UINT8(0x03, out.rsa_import_fmt);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_ed25519);
    RUN_TEST(test_parse_p256_ecdh_role);
    RUN_TEST(test_parse_rsa2048);
    RUN_TEST(test_rsa_modulus_sizes);
    RUN_TEST(test_rsa_build_roundtrip);
    return UNITY_END();
}
