// Host unit tests for the USB Mass Storage block helpers:
//   - usb_msc_range_ok: read/write bounds and write sector-alignment
//   - usb_msc_should_remount: host-active edge that schedules a remount
// Run with: pio test -e native

#include <unity.h>

#include "../../../components/usb_badge/include/usb_badge/usb_msc_bounds.h"

// A 2 MiB volume with 4096-byte sectors (matches the vfat geometry).
static constexpr uint64_t kTotal = 2u * 1024u * 1024u;
static constexpr uint16_t kBs    = 4096;
static constexpr uint32_t kLast  = (uint32_t)(kTotal / kBs) - 1;  // last valid LBA

void setUp() {}
void tearDown() {}

// --- usb_msc_range_ok: reads ---

void test_read_in_range() {
    TEST_ASSERT_TRUE(usb_msc_range_ok(kTotal, kBs, 0, 0, kBs, false));
    TEST_ASSERT_TRUE(usb_msc_range_ok(kTotal, kBs, kLast, 0, kBs, false));
    TEST_ASSERT_TRUE(usb_msc_range_ok(kTotal, kBs, 1, 100, 512, false));  // arbitrary offset/len ok for reads
}

void test_read_out_of_range() {
    TEST_ASSERT_FALSE(usb_msc_range_ok(kTotal, kBs, kLast + 1, 0, kBs, false));  // past end
    TEST_ASSERT_FALSE(usb_msc_range_ok(kTotal, kBs, kLast, 1, kBs, false));      // last block + 1 byte
}

void test_read_exact_fit() {
    // Reading the final byte of the volume must succeed.
    TEST_ASSERT_TRUE(usb_msc_range_ok(kTotal, kBs, kLast, kBs - 1, 1, false));
}

// --- usb_msc_range_ok: writes (sector-aligned only) ---

void test_write_aligned_ok() {
    TEST_ASSERT_TRUE(usb_msc_range_ok(kTotal, kBs, 0, 0, kBs, true));
    TEST_ASSERT_TRUE(usb_msc_range_ok(kTotal, kBs, 5, 0, kBs * 2, true));  // multi-block write
    TEST_ASSERT_TRUE(usb_msc_range_ok(kTotal, kBs, kLast, 0, kBs, true));
}

void test_write_misaligned_rejected() {
    TEST_ASSERT_FALSE(usb_msc_range_ok(kTotal, kBs, 0, 1, kBs, true));       // non-zero offset
    TEST_ASSERT_FALSE(usb_msc_range_ok(kTotal, kBs, 0, 0, kBs / 2, true));   // partial block
    TEST_ASSERT_FALSE(usb_msc_range_ok(kTotal, kBs, 0, 0, kBs + 1, true));   // not a whole block
}

void test_write_out_of_range() {
    TEST_ASSERT_FALSE(usb_msc_range_ok(kTotal, kBs, kLast + 1, 0, kBs, true));
    TEST_ASSERT_FALSE(usb_msc_range_ok(kTotal, kBs, kLast, 0, kBs * 2, true));  // straddles the end
}

void test_zero_block_size_rejected() {
    TEST_ASSERT_FALSE(usb_msc_range_ok(kTotal, 0, 0, 0, 0, false));
    TEST_ASSERT_FALSE(usb_msc_range_ok(kTotal, 0, 0, 0, 0, true));
}

// --- usb_msc_should_remount: host-active edge ---

void test_remount_only_on_detach_edge() {
    TEST_ASSERT_TRUE(usb_msc_should_remount(true, false));    // host detached -> remount
    TEST_ASSERT_FALSE(usb_msc_should_remount(false, true));   // host attached -> no remount
    TEST_ASSERT_FALSE(usb_msc_should_remount(true, true));    // no change
    TEST_ASSERT_FALSE(usb_msc_should_remount(false, false));  // no change
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_read_in_range);
    RUN_TEST(test_read_out_of_range);
    RUN_TEST(test_read_exact_fit);
    RUN_TEST(test_write_aligned_ok);
    RUN_TEST(test_write_misaligned_rejected);
    RUN_TEST(test_write_out_of_range);
    RUN_TEST(test_zero_block_size_rejected);
    RUN_TEST(test_remount_only_on_detach_edge);
    return UNITY_END();
}
