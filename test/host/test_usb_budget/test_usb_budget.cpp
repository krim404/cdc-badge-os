// Host unit tests for the USB endpoint budget helper:
//   - usb_ep_budget_fits: per-direction accounting against the ESP32-S3
//     silicon limits (4 non-control IN, 6 non-control OUT endpoints)
// Run with: pio test -e native

#include <unity.h>

#include "../../../components/usb_badge/include/usb_badge/usb_endpoint_budget.h"

// Per-interface endpoint costs as allocated by build_config_descriptor().
static constexpr usb_ep_usage_t kCdc = {USB_EP_BUDGET_CDC_IN, USB_EP_BUDGET_CDC_OUT};
static constexpr usb_ep_usage_t kHidInOut = {1, 1};   // FIDO2
static constexpr usb_ep_usage_t kHidInOnly = {1, 0};  // Keyboard
static constexpr usb_ep_usage_t kCcid = {1, 1};       // GPG smartcard
static constexpr usb_ep_usage_t kMsc = {USB_EP_BUDGET_MSC_IN, USB_EP_BUDGET_MSC_OUT};

static usb_ep_usage_t add(usb_ep_usage_t a, usb_ep_usage_t b) {
    return {static_cast<uint8_t>(a.in_eps + b.in_eps),
            static_cast<uint8_t>(a.out_eps + b.out_eps)};
}

void setUp() {}
void tearDown() {}

// --- baseline sets ---

void test_cdc_alone_fits() {
    TEST_ASSERT_TRUE(usb_ep_budget_fits({0, 0}, kCdc));
}

void test_full_badge_set_fits_exactly() {
    // CDC + FIDO2 + CCID = 4 IN / 3 OUT: exactly at the IN limit.
    usb_ep_usage_t used = add(add(kCdc, kHidInOut), kCcid);
    TEST_ASSERT_EQUAL_UINT8(USB_EP_BUDGET_MAX_IN, used.in_eps);
    TEST_ASSERT_TRUE(usb_ep_budget_fits(used, {0, 0}));
}

// --- IN direction is the bottleneck ---

void test_keyboard_on_full_set_rejected() {
    // A 4th IN consumer must be rejected even though OUT still has slack.
    usb_ep_usage_t used = add(add(kCdc, kHidInOut), kCcid);
    TEST_ASSERT_FALSE(usb_ep_budget_fits(used, kHidInOnly));
}

void test_msc_on_full_set_rejected() {
    usb_ep_usage_t used = add(add(kCdc, kHidInOut), kCcid);
    TEST_ASSERT_FALSE(usb_ep_budget_fits(used, kMsc));
}

void test_keyboard_fits_after_ccid_suspended() {
    // Swap scenario: CDC + FIDO2 only, keyboard takes the freed IN endpoint.
    usb_ep_usage_t used = add(kCdc, kHidInOut);
    TEST_ASSERT_TRUE(usb_ep_budget_fits(used, kHidInOnly));
}

void test_keyboard_plus_msc_fits_without_ccid_and_fido() {
    // CDC + keyboard + MSC = 4 IN / 2 OUT.
    usb_ep_usage_t used = add(kCdc, kHidInOnly);
    TEST_ASSERT_TRUE(usb_ep_budget_fits(used, kMsc));
}

// --- CDC disabled frees its endpoints (usb_hid_set_cdc / setCdcEnabled) ---

void test_full_hid_set_fits_without_cdc() {
    // FIDO2 + keyboard + CCID + MSC = 4 IN / 3 OUT: fits once CDC is off.
    usb_ep_usage_t used = add(add(kHidInOut, kHidInOnly), kCcid);
    TEST_ASSERT_TRUE(usb_ep_budget_fits(used, kMsc));
}

void test_full_hid_set_rejected_with_cdc() {
    // The same set can never coexist with CDC (6 IN > 4).
    usb_ep_usage_t used = add(add(add(kCdc, kHidInOut), kHidInOnly), kCcid);
    TEST_ASSERT_FALSE(usb_ep_budget_fits(used, kMsc));
}

void test_cdc_reenable_rejected_when_endpoints_taken() {
    // FIDO2 + keyboard + CCID + MSC leaves only 0 IN: CDC (2 IN) must fail.
    usb_ep_usage_t used = add(add(add(kHidInOut, kHidInOnly), kCcid), kMsc);
    TEST_ASSERT_FALSE(usb_ep_budget_fits(used, kCdc));
}

void test_cdc_reenable_fits_after_freeing_two_in() {
    // Dropping keyboard and MSC (2 IN) makes room for CDC again.
    usb_ep_usage_t used = add(kHidInOut, kCcid);
    TEST_ASSERT_TRUE(usb_ep_budget_fits(used, kCdc));
}

// --- OUT direction bound ---

void test_out_limit_enforced() {
    TEST_ASSERT_FALSE(usb_ep_budget_fits({0, USB_EP_BUDGET_MAX_OUT}, {0, 1}));
    TEST_ASSERT_TRUE(usb_ep_budget_fits({0, USB_EP_BUDGET_MAX_OUT - 1}, {0, 1}));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_cdc_alone_fits);
    RUN_TEST(test_full_badge_set_fits_exactly);
    RUN_TEST(test_keyboard_on_full_set_rejected);
    RUN_TEST(test_msc_on_full_set_rejected);
    RUN_TEST(test_keyboard_fits_after_ccid_suspended);
    RUN_TEST(test_keyboard_plus_msc_fits_without_ccid_and_fido);
    RUN_TEST(test_full_hid_set_fits_without_cdc);
    RUN_TEST(test_full_hid_set_rejected_with_cdc);
    RUN_TEST(test_cdc_reenable_rejected_when_endpoints_taken);
    RUN_TEST(test_cdc_reenable_fits_after_freeing_two_in);
    RUN_TEST(test_out_limit_enforced);
    return UNITY_END();
}
