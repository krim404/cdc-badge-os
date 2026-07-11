// Host unit tests for the refcounted shared CCID USB ownership (scard_usb):
//   - first acquire registers the interface once, later acquires only count
//   - last release unregisters exactly once, extra releases are ignored
//   - failed registration does not leak a refcount
// Run with: pio test -e native
//
// Uses the UsbManager shim in shim/cdc_core/UsbManager.h (call counters).

#include <unity.h>

#include "../../../components/cdc_scard/src/scard_usb.cpp"

// Stubs for declarations pulled in by scard_usb.cpp.
extern "C" void log_write(log_level_t, const char*, const char*, ...) {}
extern "C" bool ccid_init(void) { return true; }

static cdc::core::UsbManager& mgr() { return cdc::core::UsbManager::instance(); }

void setUp() {
    // Drain any refcount left over from a previous test and reopen the gate.
    for (int i = 0; i < 8; i++) scard_usb_release();
    scard_usb_set_enabled(true);
    mgr().reset();
}

void tearDown() {}

static void test_single_acquire_registers_once() {
    TEST_ASSERT_TRUE(scard_usb_acquire());
    TEST_ASSERT_EQUAL_INT(1, mgr().registerCalls);
    TEST_ASSERT_EQUAL_INT(0, mgr().unregisterCalls);

    scard_usb_release();
    TEST_ASSERT_EQUAL_INT(1, mgr().unregisterCalls);
}

static void test_nested_acquires_share_one_registration() {
    TEST_ASSERT_TRUE(scard_usb_acquire());
    TEST_ASSERT_TRUE(scard_usb_acquire());
    TEST_ASSERT_TRUE(scard_usb_acquire());
    TEST_ASSERT_EQUAL_INT(1, mgr().registerCalls);

    scard_usb_release();
    scard_usb_release();
    TEST_ASSERT_EQUAL_INT(0, mgr().unregisterCalls);

    scard_usb_release();
    TEST_ASSERT_EQUAL_INT(1, mgr().unregisterCalls);
}

static void test_extra_releases_are_ignored() {
    scard_usb_release();
    scard_usb_release();
    TEST_ASSERT_EQUAL_INT(0, mgr().unregisterCalls);

    TEST_ASSERT_TRUE(scard_usb_acquire());
    TEST_ASSERT_EQUAL_INT(1, mgr().registerCalls);
    scard_usb_release();
    scard_usb_release();
    TEST_ASSERT_EQUAL_INT(1, mgr().unregisterCalls);
}

static void test_failed_registration_keeps_refcount_zero() {
    mgr().registerResult = false;
    TEST_ASSERT_FALSE(scard_usb_acquire());
    TEST_ASSERT_EQUAL_INT(1, mgr().registerCalls);

    // No reference was handed out, so a release must not unregister.
    scard_usb_release();
    TEST_ASSERT_EQUAL_INT(0, mgr().unregisterCalls);

    // Recovery: a later acquire works again.
    mgr().registerResult = true;
    TEST_ASSERT_TRUE(scard_usb_acquire());
    TEST_ASSERT_EQUAL_INT(2, mgr().registerCalls);
    scard_usb_release();
    TEST_ASSERT_EQUAL_INT(1, mgr().unregisterCalls);
}

static void test_reacquire_after_full_release_reregisters() {
    TEST_ASSERT_TRUE(scard_usb_acquire());
    scard_usb_release();
    TEST_ASSERT_TRUE(scard_usb_acquire());
    TEST_ASSERT_EQUAL_INT(2, mgr().registerCalls);
    TEST_ASSERT_EQUAL_INT(1, mgr().unregisterCalls);
    scard_usb_release();
    TEST_ASSERT_EQUAL_INT(2, mgr().unregisterCalls);
}

// --- service gate (scard_usb_set_enabled) ---

static void test_acquire_under_closed_gate_counts_without_registering() {
    TEST_ASSERT_TRUE(scard_usb_set_enabled(false));
    TEST_ASSERT_TRUE(scard_usb_acquire());
    TEST_ASSERT_TRUE(scard_usb_in_use());
    TEST_ASSERT_EQUAL_INT(0, mgr().registerCalls);

    // Release under a closed gate must not unregister anything either.
    scard_usb_release();
    TEST_ASSERT_EQUAL_INT(0, mgr().unregisterCalls);
}

static void test_gate_open_with_refs_registers_exactly_once() {
    TEST_ASSERT_TRUE(scard_usb_set_enabled(false));
    TEST_ASSERT_TRUE(scard_usb_acquire());
    TEST_ASSERT_TRUE(scard_usb_acquire());

    TEST_ASSERT_TRUE(scard_usb_set_enabled(true));
    TEST_ASSERT_EQUAL_INT(1, mgr().registerCalls);

    // Idempotent: enabling again does not re-register.
    TEST_ASSERT_TRUE(scard_usb_set_enabled(true));
    TEST_ASSERT_EQUAL_INT(1, mgr().registerCalls);

    scard_usb_release();
    scard_usb_release();
    TEST_ASSERT_EQUAL_INT(1, mgr().unregisterCalls);
}

static void test_gate_close_unregisters_but_keeps_refs() {
    TEST_ASSERT_TRUE(scard_usb_acquire());
    TEST_ASSERT_EQUAL_INT(1, mgr().registerCalls);

    TEST_ASSERT_TRUE(scard_usb_set_enabled(false));
    TEST_ASSERT_EQUAL_INT(1, mgr().unregisterCalls);
    TEST_ASSERT_TRUE(scard_usb_in_use());

    // Reopening restores the interface for the still-held reference.
    TEST_ASSERT_TRUE(scard_usb_set_enabled(true));
    TEST_ASSERT_EQUAL_INT(2, mgr().registerCalls);

    scard_usb_release();
    TEST_ASSERT_EQUAL_INT(2, mgr().unregisterCalls);
}

static void test_gate_open_fails_when_budget_refuses() {
    TEST_ASSERT_TRUE(scard_usb_set_enabled(false));
    TEST_ASSERT_TRUE(scard_usb_acquire());

    mgr().registerResult = false;
    TEST_ASSERT_FALSE(scard_usb_set_enabled(true));
    TEST_ASSERT_FALSE(scard_usb_enabled());

    // Recovery once the budget frees up again.
    mgr().registerResult = true;
    TEST_ASSERT_TRUE(scard_usb_set_enabled(true));
    TEST_ASSERT_TRUE(scard_usb_enabled());
    scard_usb_release();
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_single_acquire_registers_once);
    RUN_TEST(test_nested_acquires_share_one_registration);
    RUN_TEST(test_extra_releases_are_ignored);
    RUN_TEST(test_failed_registration_keeps_refcount_zero);
    RUN_TEST(test_reacquire_after_full_release_reregisters);
    RUN_TEST(test_acquire_under_closed_gate_counts_without_registering);
    RUN_TEST(test_gate_open_with_refs_registers_exactly_once);
    RUN_TEST(test_gate_close_unregisters_but_keeps_refs);
    RUN_TEST(test_gate_open_fails_when_budget_refuses);
    return UNITY_END();
}
