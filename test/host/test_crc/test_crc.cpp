/**
 * \file
 * \brief Spec-conformance known-answer-vector tests for the three CRC
 *        variants used in the firmware.
 *
 * These are spec-conformance vectors that mirror the firmware algorithms; they
 * are not direct unit tests of the firmware symbols because each firmware CRC
 * lives in a .cpp that pulls in non-host-portable headers:
 *   - CRC16 (ISO13239): cdc::mod_otphid::OtpHidCr::crc16 in OtpHidCr.cpp, which
 *     includes esp_attr.h / freertos / cdc_log. Direct testing needs firmware
 *     refactoring (RF-02), deferred.
 *   - CRC32: cdc::msg crc32() delegates to esp_rom_crc32_le (ESP-IDF ROM),
 *     unavailable on host. Pinned here as the standard zlib/IEEE CRC-32.
 *   - CRC24 (RFC 4880): crc24() in mod_gpg/src/openpgp/xsig.cpp, which includes
 *     mbedtls and module headers. Pinned here from the RFC.
 *
 * The reimplementations below are byte-for-byte copies of the firmware loops.
 * The check vectors are cross-validated against the published standard check
 * values (CRC-16/X-25 = 0x906E inverted form, CRC-32 "123456789" = 0xCBF43926,
 * CRC-24/OPENPGP "123456789" = 0x21CF02), so any drift in the firmware
 * algorithm constants would diverge from these pins.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// --- Algorithm reimplementations (mirror the firmware loops verbatim) ---

/// Mirror of cdc::mod_otphid::OtpHidCr::crc16 (poly 0x8408, init 0xFFFF,
/// reflected; ISO13239 / CRC-16-X.25 form).
static uint16_t crc16_iso13239(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            int lsb = crc & 1;
            crc >>= 1;
            if (lsb) crc ^= 0x8408;
        }
    }
    return crc;
}

/// Standard zlib/IEEE CRC-32 (poly 0xEDB88320 reflected, init/xorout
/// 0xFFFFFFFF). Equivalent to esp_rom_crc32_le(0, ...) used by cdc::msg.
static uint32_t crc32_ieee(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(crc & 1));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/// Mirror of crc24() in xsig.cpp (RFC 4880 section 6.1).
static uint32_t crc24_rfc4880(const uint8_t* data, size_t len) {
    constexpr uint32_t kInit = 0x00B704CEu;
    constexpr uint32_t kPoly = 0x01864CFBu;
    uint32_t crc = kInit;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 16;
        for (int b = 0; b < 8; ++b) {
            crc <<= 1;
            if (crc & 0x01000000u) crc ^= kPoly;
        }
    }
    return crc & 0x00FFFFFFu;
}

static const uint8_t kCheck[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};

// --- CRC16-ISO13239 ---

static void test_crc16_known_answers(void) {
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, crc16_iso13239(nullptr, 0));
    // CRC-16/X-25 standard check value over "123456789".
    TEST_ASSERT_EQUAL_HEX16(0x6F91, crc16_iso13239(kCheck, sizeof(kCheck)));
    TEST_ASSERT_EQUAL_HEX16(0x5C0A, crc16_iso13239(reinterpret_cast<const uint8_t*>("A"), 1));
}

/// The ISO13239 residual property the firmware relies on: appending the
/// complemented CRC little-endian yields the magic residual 0xF0B8.
static void test_crc16_residual_property(void) {
    uint16_t crc = static_cast<uint16_t>(~crc16_iso13239(kCheck, sizeof(kCheck)));
    uint8_t framed[sizeof(kCheck) + 2];
    std::memcpy(framed, kCheck, sizeof(kCheck));
    framed[sizeof(kCheck)] = static_cast<uint8_t>(crc & 0xFF);
    framed[sizeof(kCheck) + 1] = static_cast<uint8_t>(crc >> 8);
    TEST_ASSERT_EQUAL_HEX16(0xF0B8, crc16_iso13239(framed, sizeof(framed)));
}

// --- CRC32 (zlib/IEEE) ---

static void test_crc32_known_answers(void) {
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, crc32_ieee(nullptr, 0));
    // Canonical CRC-32 check value over "123456789".
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32_ieee(kCheck, sizeof(kCheck)));
    TEST_ASSERT_EQUAL_HEX32(0xD3D99E8Bu, crc32_ieee(reinterpret_cast<const uint8_t*>("A"), 1));
    const char* fox = "The quick brown fox jumps over the lazy dog";
    TEST_ASSERT_EQUAL_HEX32(0x414FA339u,
                            crc32_ieee(reinterpret_cast<const uint8_t*>(fox), std::strlen(fox)));
}

// --- CRC24 (RFC 4880 / OpenPGP) ---

static void test_crc24_known_answers(void) {
    TEST_ASSERT_EQUAL_HEX32(0x00B704CEu, crc24_rfc4880(nullptr, 0));
    // Canonical CRC-24/OPENPGP check value over "123456789".
    TEST_ASSERT_EQUAL_HEX32(0x0021CF02u, crc24_rfc4880(kCheck, sizeof(kCheck)));
    TEST_ASSERT_EQUAL_HEX32(0x00FE86FAu, crc24_rfc4880(reinterpret_cast<const uint8_t*>("A"), 1));
    const char* s = "The quick brown fox";
    TEST_ASSERT_EQUAL_HEX32(0x00F82D42u,
                            crc24_rfc4880(reinterpret_cast<const uint8_t*>(s), std::strlen(s)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_crc16_known_answers);
    RUN_TEST(test_crc16_residual_property);
    RUN_TEST(test_crc32_known_answers);
    RUN_TEST(test_crc24_known_answers);
    return UNITY_END();
}
