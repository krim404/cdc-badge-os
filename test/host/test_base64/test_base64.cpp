/**
 * \file
 * \brief Base64 encode/decode known-answer + round-trip tests (RFC 4648).
 *
 * The firmware base64 (cdc_os_ui/BackupManager.cpp b64Encode/b64Decode,
 * mod_gpg armorBase64) delegates to mbedtls (mbedtls_base64_encode/decode).
 * Linking mbedtls standalone on the native env is non-trivial, so this is a
 * spec-conformance test: a self-contained RFC 4648 codec pinned against the
 * canonical RFC 4648 section 10 test vectors, which is exactly the standard
 * mbedtls implements. A direct unit test of the firmware wrappers needs an
 * mbedtls host link (RF-03), deferred.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static const char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Standard RFC 4648 base64 encode (with '=' padding).
static std::string b64encode(const uint8_t* data, size_t len) {
    std::string out;
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     static_cast<uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kAlphabet[n & 0x3F]);
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

/// Maps a base64 character to its 6-bit value, or -1 if not in the alphabet.
static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/// Standard RFC 4648 base64 decode. Returns false on malformed input.
static bool b64decode(const std::string& in, std::string& out) {
    out.clear();
    if (in.size() % 4 != 0) return false;
    for (size_t i = 0; i < in.size(); i += 4) {
        int pad = 0;
        int v[4];
        for (int k = 0; k < 4; ++k) {
            char c = in[i + k];
            if (c == '=') {
                // Padding is only valid in the final group, trailing positions.
                if (i + 4 != in.size() || k < 2) return false;
                v[k] = 0;
                pad++;
            } else {
                if (pad) return false;  // data after padding
                v[k] = b64val(c);
                if (v[k] < 0) return false;
            }
        }
        uint32_t n = (static_cast<uint32_t>(v[0]) << 18) |
                     (static_cast<uint32_t>(v[1]) << 12) |
                     (static_cast<uint32_t>(v[2]) << 6) |
                     static_cast<uint32_t>(v[3]);
        out.push_back(static_cast<char>((n >> 16) & 0xFF));
        if (pad < 2) out.push_back(static_cast<char>((n >> 8) & 0xFF));
        if (pad < 1) out.push_back(static_cast<char>(n & 0xFF));
    }
    return true;
}

static std::string enc(const char* s) {
    return b64encode(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
}

/// RFC 4648 section 10 canonical test vectors.
static void test_rfc4648_vectors(void) {
    TEST_ASSERT_EQUAL_STRING("", enc("").c_str());
    TEST_ASSERT_EQUAL_STRING("Zg==", enc("f").c_str());
    TEST_ASSERT_EQUAL_STRING("Zm8=", enc("fo").c_str());
    TEST_ASSERT_EQUAL_STRING("Zm9v", enc("foo").c_str());
    TEST_ASSERT_EQUAL_STRING("Zm9vYg==", enc("foob").c_str());
    TEST_ASSERT_EQUAL_STRING("Zm9vYmE=", enc("fooba").c_str());
    TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", enc("foobar").c_str());
}

/// Decode the same canonical vectors back to the original.
static void test_decode_vectors(void) {
    struct { const char* b64; const char* plain; } cases[] = {
        {"", ""},        {"Zg==", "f"},      {"Zm8=", "fo"},
        {"Zm9v", "foo"}, {"Zm9vYg==", "foob"}, {"Zm9vYmE=", "fooba"},
        {"Zm9vYmFy", "foobar"},
    };
    for (const auto& c : cases) {
        std::string out;
        TEST_ASSERT_TRUE(b64decode(c.b64, out));
        TEST_ASSERT_EQUAL_STRING(c.plain, out.c_str());
    }
}

/// Round-trip every byte length 0..255 with all-distinct bytes.
static void test_roundtrip_binary(void) {
    for (size_t len = 0; len <= 255; ++len) {
        std::string blob(len, '\0');
        for (size_t i = 0; i < len; ++i) {
            blob[i] = static_cast<char>((i * 7 + 3) & 0xFF);
        }
        std::string e = b64encode(reinterpret_cast<const uint8_t*>(blob.data()), len);
        TEST_ASSERT_EQUAL_size_t(0u, e.size() % 4);  // always padded to 4-char groups
        std::string d;
        TEST_ASSERT_TRUE(b64decode(e, d));
        TEST_ASSERT_EQUAL_size_t(len, d.size());
        TEST_ASSERT_EQUAL_INT(0, std::memcmp(blob.data(), d.data(), len));
    }
}

/// Malformed input is rejected (non-multiple-of-4, bad char, misplaced pad).
static void test_decode_rejects_malformed(void) {
    std::string out;
    TEST_ASSERT_FALSE(b64decode("Zg=", out));     // wrong length
    TEST_ASSERT_FALSE(b64decode("Zg=A", out));     // data after pad
    TEST_ASSERT_FALSE(b64decode("Z!==", out));     // illegal char
    TEST_ASSERT_FALSE(b64decode("====", out));     // pad in data positions
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_rfc4648_vectors);
    RUN_TEST(test_decode_vectors);
    RUN_TEST(test_roundtrip_binary);
    RUN_TEST(test_decode_rejects_malformed);
    return UNITY_END();
}
