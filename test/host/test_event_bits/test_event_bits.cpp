/**
 * \file
 * \brief Guards the plugin EventBus bit contract: every public EVENT_* flag in
 *        host_api.h must equal (1u << ordinal) of the matching core EventType,
 *        because on_bus_event matches subscriptions with `1u << evt.type`. The
 *        same invariant is enforced at compile time by static_asserts in
 *        host_api_event.cpp; this pins it as an explicit, readable unit test
 *        and additionally checks that the plugin-facing bits stay contiguous.
 *
 * Header-only: uses the constexpr EventBus::eventMask and the EventType enum,
 * so it links without the FreeRTOS-backed EventBus.cpp.
 */

#include "../../../components/cdc_core/include/cdc_core/EventBus.h"
#include "../../../components/plugin_manager/include/plugin_manager/host_api.h"

#include <unity.h>

using cdc::core::EventBus;
using cdc::core::EventType;

void setUp(void) {}
void tearDown(void) {}

static void test_public_bits_match_core_ordinals(void) {
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::KEY_PRESSED),            EVENT_KEY_PRESSED);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::KEY_RELEASED),           EVENT_KEY_RELEASED);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::KEY_LONG_PRESS),         EVENT_KEY_LONG_PRESS);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::POWER_USB_CONNECTED),    EVENT_POWER_USB_CONN);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::POWER_USB_DISCONNECTED), EVENT_POWER_USB_DISCONN);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::POWER_CHARGING),         EVENT_POWER_CHARGING);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::POWER_BATTERY_LOW),      EVENT_POWER_BATT_LOW);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::POWER_BATTERY_CRITICAL), EVENT_POWER_BATT_CRIT);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::SYSTEM_UNLOCK),          EVENT_SYSTEM_UNLOCK);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::SYSTEM_LOCK),            EVENT_SYSTEM_LOCK);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::SYSTEM_SLEEP),           EVENT_SYSTEM_SLEEP);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::SYSTEM_WAKE),            EVENT_SYSTEM_WAKE);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::BLE_CONNECTED),          EVENT_BLE_CONNECTED);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::BLE_DISCONNECTED),       EVENT_BLE_DISCONNECTED);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::TIMER_TICK),             EVENT_TIMER_TICK);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::MODULE_EVENT),           EVENT_MODULE_EVENT);
    TEST_ASSERT_EQUAL_UINT32(EventBus::eventMask(EventType::DISPLAY_REFRESH),        EVENT_DISPLAY_REFRESH);
}

static void test_public_bits_are_contiguous(void) {
    // Plugin-facing events occupy bits 0..16 with no gaps; DISPLAY_REFRESH is
    // the last of them.
    TEST_ASSERT_EQUAL_UINT32(1u << 0,  EVENT_KEY_PRESSED);
    TEST_ASSERT_EQUAL_UINT32(1u << 16, EVENT_DISPLAY_REFRESH);
    const uint32_t all = EVENT_KEY_PRESSED | EVENT_KEY_RELEASED | EVENT_KEY_LONG_PRESS |
        EVENT_POWER_USB_CONN | EVENT_POWER_USB_DISCONN | EVENT_POWER_CHARGING |
        EVENT_POWER_BATT_LOW | EVENT_POWER_BATT_CRIT | EVENT_SYSTEM_UNLOCK |
        EVENT_SYSTEM_LOCK | EVENT_SYSTEM_SLEEP | EVENT_SYSTEM_WAKE |
        EVENT_BLE_CONNECTED | EVENT_BLE_DISCONNECTED | EVENT_TIMER_TICK |
        EVENT_MODULE_EVENT | EVENT_DISPLAY_REFRESH;
    TEST_ASSERT_EQUAL_UINT32((1u << 17) - 1u, all);  // dense mask, no holes
}

static void test_event_count_fits_the_mask(void) {
    TEST_ASSERT_TRUE(static_cast<size_t>(EventType::EVENT_COUNT) < 32);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_public_bits_match_core_ordinals);
    RUN_TEST(test_public_bits_are_contiguous);
    RUN_TEST(test_event_count_fits_the_mask);
    return UNITY_END();
}
