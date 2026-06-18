// Host unit tests for the image engine core: format detection + Floyd-Steinberg
// dither. Pure logic, no hardware. Run with: pio test -e native

#include <unity.h>

#include <cstdint>
#include <cstring>

#include "../../../components/cdc_image/src/ImageFormat.cpp"
#include "../../../components/cdc_image/src/Dither.cpp"

using namespace cdc::image;

void setUp(void) {}
void tearDown(void) {}

// --- format detection -------------------------------------------------------

void test_detect_png(void) {
    const uint8_t png[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    TEST_ASSERT_EQUAL(static_cast<int>(ImageFormat::Png),
                      static_cast<int>(detectImageFormat(png, sizeof(png))));
}

void test_detect_jpeg(void) {
    const uint8_t jpg[3] = {0xFF, 0xD8, 0xFF};
    TEST_ASSERT_EQUAL(static_cast<int>(ImageFormat::Jpeg),
                      static_cast<int>(detectImageFormat(jpg, sizeof(jpg))));
}

void test_detect_unknown(void) {
    const uint8_t junk[4] = {0x00, 0x01, 0x02, 0x03};
    const uint8_t gif[6] = {'G', 'I', 'F', '8', '9', 'a'};  // GIF unsupported -> Unknown
    TEST_ASSERT_EQUAL(static_cast<int>(ImageFormat::Unknown),
                      static_cast<int>(detectImageFormat(junk, sizeof(junk))));
    TEST_ASSERT_EQUAL(static_cast<int>(ImageFormat::Unknown),
                      static_cast<int>(detectImageFormat(gif, sizeof(gif))));
    TEST_ASSERT_EQUAL(static_cast<int>(ImageFormat::Unknown),
                      static_cast<int>(detectImageFormat(nullptr, 0)));
    TEST_ASSERT_EQUAL(static_cast<int>(ImageFormat::Unknown),
                      static_cast<int>(detectImageFormat(junk, 2)));
}

// --- dither -----------------------------------------------------------------

static size_t countBlack(const uint8_t* packed, uint16_t w) {
    size_t c = 0;
    for (uint16_t x = 0; x < w; ++x) {
        if (packed[x >> 3] & (0x80u >> (x & 7))) ++c;
    }
    return c;
}

void test_dither_all_black(void) {
    constexpr uint16_t W = 64;
    int16_t scratch[Ditherer::scratchCells(W)];
    uint8_t gray[W];
    uint8_t out[(W + 7) / 8];
    std::memset(gray, 0, sizeof(gray));
    Ditherer d(W, scratch);
    d.ditherRow(gray, out);
    TEST_ASSERT_EQUAL_UINT32(W, countBlack(out, W));
}

void test_dither_all_white(void) {
    constexpr uint16_t W = 64;
    int16_t scratch[Ditherer::scratchCells(W)];
    uint8_t gray[W];
    uint8_t out[(W + 7) / 8];
    std::memset(gray, 255, sizeof(gray));
    Ditherer d(W, scratch);
    d.ditherRow(gray, out);
    TEST_ASSERT_EQUAL_UINT32(0, countBlack(out, W));
}

void test_dither_monotonic(void) {
    constexpr uint16_t W = 128;
    int16_t scratch[Ditherer::scratchCells(W)];
    uint8_t gray[W];
    uint8_t out[(W + 7) / 8];
    auto blackForLevel = [&](uint8_t level) -> size_t {
        std::memset(gray, level, sizeof(gray));
        Ditherer d(W, scratch);
        size_t last = 0;
        for (int row = 0; row < 8; ++row) {
            d.ditherRow(gray, out);
            last = countBlack(out, W);
        }
        return last;
    };
    const size_t dark = blackForLevel(64);
    const size_t light = blackForLevel(200);
    TEST_ASSERT_TRUE(dark > light);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_detect_png);
    RUN_TEST(test_detect_jpeg);
    RUN_TEST(test_detect_unknown);
    RUN_TEST(test_dither_all_black);
    RUN_TEST(test_dither_all_white);
    RUN_TEST(test_dither_monotonic);
    return UNITY_END();
}
