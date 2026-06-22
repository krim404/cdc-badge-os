/**
 * \file
 * \brief Host unit test for the OpenPGP ECDH (encryption-subkey) public-key body.
 *
 * Compiles the real fingerprint.cpp against a no-op mbedtls SHA shim (shim/),
 * so buildEcdhPubkeyBody is exercised verbatim. The hash functions are never
 * called by the unit under test; the shim only satisfies the linker.
 *
 * Locks the RFC 6637 ECDH (algorithm 18) public-key body byte layout, in
 * particular the canonical 515-bit P-256 point MPI that gpg expects when it
 * recomputes a subkey fingerprint.
 */

#include "../../../components/mod_gpg/src/openpgp/fingerprint.cpp"

#include <cstring>
#include <unity.h>

using namespace cdc::mod_gpg;

void setUp() {}
void tearDown() {}

// A recognisable 64-byte X||Y point (0x01..0x40).
static void makePoint(uint8_t point[64]) {
    for (int i = 0; i < 64; ++i) point[i] = static_cast<uint8_t>(i + 1);
}

// The full body for a known point and creation time has the exact OpenPGP
// RFC 6637 layout: version, creation time, algo 18, P-256 OID, point MPI, KDF.
static void test_ecdh_body_layout() {
    uint8_t point[64];
    makePoint(point);

    uint8_t out[128];
    const size_t n = buildEcdhPubkeyBody(point, 0x12345678u, out, sizeof(out));

    // 1 (version) + 4 (time) + 1 (algo) + 9 (OID) + 67 (MPI) + 4 (KDF).
    TEST_ASSERT_EQUAL_size_t(86u, n);

    TEST_ASSERT_EQUAL_HEX8(0x04, out[0]);                 // version 4
    TEST_ASSERT_EQUAL_HEX8(0x12, out[1]);                 // created_at, big-endian
    TEST_ASSERT_EQUAL_HEX8(0x34, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0x56, out[3]);
    TEST_ASSERT_EQUAL_HEX8(0x78, out[4]);
    TEST_ASSERT_EQUAL_HEX8(18, out[5]);                   // public-key algo ECDH

    const uint8_t oid[9] = {0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(oid, &out[6], 9);

    TEST_ASSERT_EQUAL_HEX8(0x04, out[17]);                // uncompressed point prefix
    TEST_ASSERT_EQUAL_HEX8_ARRAY(point, &out[18], 64);    // X||Y verbatim

    const uint8_t kdf[4] = {0x03, 0x01, 0x08, 0x07};      // size, reserved, SHA-256, AES-128
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kdf, &out[82], 4);
}

// The point MPI must declare 515 bits (0x0203), the canonical length gpg uses
// for an uncompressed nistp256 point. 520 (0x0208) breaks subkey verification.
static void test_ecdh_mpi_is_515_bits() {
    uint8_t point[64];
    makePoint(point);

    uint8_t out[128];
    buildEcdhPubkeyBody(point, 0u, out, sizeof(out));

    TEST_ASSERT_EQUAL_HEX8(0x02, out[15]);
    TEST_ASSERT_EQUAL_HEX8(0x03, out[16]);
}

// An undersized output buffer is rejected, not overrun.
static void test_ecdh_body_rejects_small_buffer() {
    uint8_t point[64];
    makePoint(point);

    uint8_t out[85];
    TEST_ASSERT_EQUAL_size_t(0u, buildEcdhPubkeyBody(point, 0u, out, sizeof(out)));
}

// The DEC subkey fingerprint is deterministic for a fixed point and time.
static void test_ecdh_fingerprint_deterministic() {
    uint8_t point[64];
    makePoint(point);

    uint8_t fp1[20];
    uint8_t fp2[20];
    TEST_ASSERT_TRUE(calculateFingerprintV4Ecdh(point, 0xCAFEBABEu, fp1));
    TEST_ASSERT_TRUE(calculateFingerprintV4Ecdh(point, 0xCAFEBABEu, fp2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fp1, fp2, 20);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ecdh_body_layout);
    RUN_TEST(test_ecdh_mpi_is_515_bits);
    RUN_TEST(test_ecdh_body_rejects_small_buffer);
    RUN_TEST(test_ecdh_fingerprint_deterministic);
    return UNITY_END();
}
