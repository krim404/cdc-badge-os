/**
 * \file
 * \brief Host unit test for the firmware CP437 codec (cdc::core::cp437).
 *
 * Direct unit test: compiles the real firmware source
 * components/cdc_core/src/Cp437.cpp on the host (it depends only on the C++
 * stdlib). The neighbouring `cdc_core/Cp437.h` shim resolves the source's
 * quote-include to the real firmware header.
 *
 * Verifies the byte mappings documented in CLAUDE.md "Display Text Rendering":
 *   ae 0x84, oe 0x94, ue 0x81, AE 0x8E, OE 0x99, UE 0x9A, ss 0xE1, plus ASCII
 * passthrough and UTF-8 <-> CP437 round-trips.
 */

#include "../../../components/cdc_core/src/Cp437.cpp"

#include <string>
#include <unity.h>

using cdc::core::cp437::fromUnicode;
using cdc::core::cp437::fromUtf8;
using cdc::core::cp437::toUnicode;
using cdc::core::cp437::toUtf8;

void setUp(void) {}
void tearDown(void) {}

/// UTF-8 byte sequences for the German letters (canonical NFC encoding).
static constexpr const char* kUtf8_ae = "\xC3\xA4";  // U+00E4
static constexpr const char* kUtf8_oe = "\xC3\xB6";  // U+00F6
static constexpr const char* kUtf8_ue = "\xC3\xBC";  // U+00FC
static constexpr const char* kUtf8_AE = "\xC3\x84";  // U+00C4
static constexpr const char* kUtf8_OE = "\xC3\x96";  // U+00D6
static constexpr const char* kUtf8_UE = "\xC3\x9C";  // U+00DC
static constexpr const char* kUtf8_ss = "\xC3\x9F";  // U+00DF

/// Each German UTF-8 letter maps to exactly one CP437 byte.
static void test_german_utf8_to_cp437_bytes(void) {
    std::string ae = fromUtf8(kUtf8_ae);
    TEST_ASSERT_EQUAL_size_t(1u, ae.size());
    TEST_ASSERT_EQUAL_HEX8(0x84, static_cast<uint8_t>(ae[0]));

    std::string oe = fromUtf8(kUtf8_oe);
    TEST_ASSERT_EQUAL_size_t(1u, oe.size());
    TEST_ASSERT_EQUAL_HEX8(0x94, static_cast<uint8_t>(oe[0]));

    std::string ue = fromUtf8(kUtf8_ue);
    TEST_ASSERT_EQUAL_size_t(1u, ue.size());
    TEST_ASSERT_EQUAL_HEX8(0x81, static_cast<uint8_t>(ue[0]));

    std::string AE = fromUtf8(kUtf8_AE);
    TEST_ASSERT_EQUAL_size_t(1u, AE.size());
    TEST_ASSERT_EQUAL_HEX8(0x8E, static_cast<uint8_t>(AE[0]));

    std::string OE = fromUtf8(kUtf8_OE);
    TEST_ASSERT_EQUAL_size_t(1u, OE.size());
    TEST_ASSERT_EQUAL_HEX8(0x99, static_cast<uint8_t>(OE[0]));

    std::string UE = fromUtf8(kUtf8_UE);
    TEST_ASSERT_EQUAL_size_t(1u, UE.size());
    TEST_ASSERT_EQUAL_HEX8(0x9A, static_cast<uint8_t>(UE[0]));

    std::string ss = fromUtf8(kUtf8_ss);
    TEST_ASSERT_EQUAL_size_t(1u, ss.size());
    TEST_ASSERT_EQUAL_HEX8(0xE1, static_cast<uint8_t>(ss[0]));
}

/// The Unicode codepoint <-> CP437 byte direct map matches the same bytes.
static void test_fromUnicode_german(void) {
    TEST_ASSERT_EQUAL_HEX8(0x84, fromUnicode(0x00E4));  // ae
    TEST_ASSERT_EQUAL_HEX8(0x94, fromUnicode(0x00F6));  // oe
    TEST_ASSERT_EQUAL_HEX8(0x81, fromUnicode(0x00FC));  // ue
    TEST_ASSERT_EQUAL_HEX8(0x8E, fromUnicode(0x00C4));  // AE
    TEST_ASSERT_EQUAL_HEX8(0x99, fromUnicode(0x00D6));  // OE
    TEST_ASSERT_EQUAL_HEX8(0x9A, fromUnicode(0x00DC));  // UE
    TEST_ASSERT_EQUAL_HEX8(0xE1, fromUnicode(0x00DF));  // ss
}

/// ASCII passes through unchanged in both directions.
static void test_ascii_passthrough(void) {
    const char* ascii = "Hello, Badge! [Y/N] 0123456789";
    std::string enc = fromUtf8(ascii);
    TEST_ASSERT_EQUAL_STRING(ascii, enc.c_str());

    std::string dec = toUtf8(ascii);
    TEST_ASSERT_EQUAL_STRING(ascii, dec.c_str());

    for (int c = 0; c < 0x80; ++c) {
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(c),
                                 toUnicode(static_cast<uint8_t>(c)));
        TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(c),
                               fromUnicode(static_cast<uint32_t>(c)));
    }
}

/// A CP437 byte -> UTF-8 -> CP437 round-trip reproduces the original byte for
/// every byte 0x01..0xFF. Byte 0x00 is excluded: it is the C-string NUL
/// terminator, so the char* codec interface cannot carry it.
static void test_cp437_roundtrip_all_bytes(void) {
    for (int b = 0x01; b < 0x100; ++b) {
        char in[2] = {static_cast<char>(b), '\0'};
        std::string utf8 = toUtf8(in);
        std::string back = fromUtf8(utf8.c_str());
        TEST_ASSERT_EQUAL_size_t(1u, back.size());
        TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(b),
                               static_cast<uint8_t>(back[0]));
    }
}

/// A German word round-trips through UTF-8 with the documented byte layout.
static void test_word_roundtrip(void) {
    // "Gruesse" with real umlaut+ss: G r ue ss e. The string is split so the
    // \x9F hex escape does not absorb the following 'e' as a hex digit.
    const char* utf8 = "Gr\xC3\xBC\xC3\x9F" "e";
    std::string cp = fromUtf8(utf8);
    TEST_ASSERT_EQUAL_size_t(5u, cp.size());
    TEST_ASSERT_EQUAL_HEX8('G', static_cast<uint8_t>(cp[0]));
    TEST_ASSERT_EQUAL_HEX8('r', static_cast<uint8_t>(cp[1]));
    TEST_ASSERT_EQUAL_HEX8(0x81, static_cast<uint8_t>(cp[2]));  // ue
    TEST_ASSERT_EQUAL_HEX8(0xE1, static_cast<uint8_t>(cp[3]));  // ss
    TEST_ASSERT_EQUAL_HEX8('e', static_cast<uint8_t>(cp[4]));

    std::string back = toUtf8(cp.c_str());
    TEST_ASSERT_EQUAL_STRING(utf8, back.c_str());
}

/// Unmapped Unicode (no CP437 glyph) is dropped, not passed through.
static void test_unmapped_dropped(void) {
    // U+4E2D (CJK) has no CP437 mapping -> dropped; surrounding ASCII kept.
    const char* utf8 = "a\xE4\xB8\xAD" "b";
    std::string cp = fromUtf8(utf8);
    TEST_ASSERT_EQUAL_size_t(2u, cp.size());
    TEST_ASSERT_EQUAL_HEX8('a', static_cast<uint8_t>(cp[0]));
    TEST_ASSERT_EQUAL_HEX8('b', static_cast<uint8_t>(cp[1]));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_german_utf8_to_cp437_bytes);
    RUN_TEST(test_fromUnicode_german);
    RUN_TEST(test_ascii_passthrough);
    RUN_TEST(test_cp437_roundtrip_all_bytes);
    RUN_TEST(test_word_roundtrip);
    RUN_TEST(test_unmapped_dropped);
    return UNITY_END();
}
