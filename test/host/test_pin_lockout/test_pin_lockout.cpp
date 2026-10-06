/**
 * \file
 * \brief Host unit test for the badge PIN recovery-window schedule.
 *
 * Covers `PinManager::lockoutDurationMs`: the window doubles per consecutive
 * lockout and is capped at `LOCKOUT_BASE_MS << LOCKOUT_MAX_SHIFT`.
 * Run with: pio test -e native
 */

#include <unity.h>
#include "cdc_core/PinManager.h"

using cdc::core::PinManager;

void setUp(void) {}
void tearDown(void) {}

static void test_first_lockout_is_base_window(void) {
    TEST_ASSERT_EQUAL_UINT32(PinManager::LOCKOUT_BASE_MS, PinManager::lockoutDurationMs(0));
    TEST_ASSERT_EQUAL_UINT32(PinManager::LOCKOUT_BASE_MS, PinManager::lockoutDurationMs(1));
}

static void test_window_doubles_per_lockout(void) {
    TEST_ASSERT_EQUAL_UINT32(PinManager::LOCKOUT_BASE_MS * 2, PinManager::lockoutDurationMs(2));
    TEST_ASSERT_EQUAL_UINT32(PinManager::LOCKOUT_BASE_MS * 4, PinManager::lockoutDurationMs(3));
    TEST_ASSERT_EQUAL_UINT32(PinManager::LOCKOUT_BASE_MS * 8, PinManager::lockoutDurationMs(4));
}

static void test_window_is_capped(void) {
    const uint32_t cap = PinManager::LOCKOUT_BASE_MS << PinManager::LOCKOUT_MAX_SHIFT;
    TEST_ASSERT_EQUAL_UINT32(cap, PinManager::lockoutDurationMs(PinManager::LOCKOUT_MAX_SHIFT + 1));
    TEST_ASSERT_EQUAL_UINT32(cap, PinManager::lockoutDurationMs(PinManager::LOCKOUT_MAX_SHIFT + 2));
    TEST_ASSERT_EQUAL_UINT32(cap, PinManager::lockoutDurationMs(255));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_first_lockout_is_base_window);
    RUN_TEST(test_window_doubles_per_lockout);
    RUN_TEST(test_window_is_capped);
    return UNITY_END();
}
