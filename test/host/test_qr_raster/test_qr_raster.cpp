/**
 * \file
 * \brief Host unit test for the QR raster bit-packing (cdc::ui::qr::packModule).
 *
 * Direct unit test: includes the real header from components/cdc_views. The
 * packing helper is header-only and dependency-free; the esp_qrcode encode
 * path itself is hardware/vendor code and stays out of scope here.
 */

#include "../../../components/cdc_views/include/cdc_views/QrRaster.h"

#include <cstring>
#include <unity.h>

using cdc::ui::qr::packModule;

void setUp(void) {}
void tearDown(void) {}

static bool pixel(const uint8_t* out, uint16_t stride, int x, int y) {
    return (out[static_cast<size_t>(y) * stride + (x >> 3)] & (0x80u >> (x & 7))) != 0;
}

static void test_single_module_scale1_no_quiet(void) {
    uint8_t out[8] = {};
    packModule(out, 1, 1, 0, 3, 2);
    TEST_ASSERT_TRUE(pixel(out, 1, 3, 2));
    TEST_ASSERT_EQUAL_UINT8(0x10, out[2]);  // bit 3 from MSB
    int set = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (pixel(out, 1, x, y)) set++;
    TEST_ASSERT_EQUAL_INT(1, set);
}

static void test_msb_first_packing(void) {
    uint8_t out[1] = {};
    packModule(out, 1, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(0x80, out[0]);
}

static void test_scale_blocks(void) {
    // 2 modules + scale 3 -> 6 px wide, stride 1.
    uint8_t out[6] = {};
    packModule(out, 1, 3, 0, 1, 1);
    for (int y = 0; y < 6; y++) {
        for (int x = 0; x < 6; x++) {
            const bool expect = (x >= 3 && y >= 3);
            TEST_ASSERT_EQUAL(expect, pixel(out, 1, x, y));
        }
    }
}

static void test_quiet_zone_offset(void) {
    // quiet 2, scale 2: module (0,0) starts at pixel (4,4); side = (1+4)*2 = 10 px.
    const uint16_t stride = 2;
    uint8_t out[2 * 10] = {};
    packModule(out, stride, 2, 2, 0, 0);
    for (int y = 0; y < 10; y++) {
        for (int x = 0; x < 10; x++) {
            const bool expect = (x >= 4 && x < 6 && y >= 4 && y < 6);
            TEST_ASSERT_EQUAL(expect, pixel(out, stride, x, y));
        }
    }
}

static void test_row_crossing_byte_boundary(void) {
    // Module at px 6..9 spans the byte boundary within a row.
    const uint16_t stride = 2;
    uint8_t out[2 * 16] = {};
    packModule(out, stride, 4, 0, 1, 0);  // px0 = 4, block 4..7... scale 4 -> px 4..7
    packModule(out, stride, 4, 0, 2, 0);  // px 8..11 in second byte
    TEST_ASSERT_EQUAL_UINT8(0x0F, out[0]);          // px 4..7
    TEST_ASSERT_EQUAL_UINT8(0xF0, out[1]);          // px 8..11
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_single_module_scale1_no_quiet);
    RUN_TEST(test_msb_first_packing);
    RUN_TEST(test_scale_blocks);
    RUN_TEST(test_quiet_zone_offset);
    RUN_TEST(test_row_crossing_byte_boundary);
    return UNITY_END();
}
