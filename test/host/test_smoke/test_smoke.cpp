/**
 * \file
 * \brief Smoke test proving the native (host) test environment runs.
 *
 * This is the placeholder that validates `pio test -e native` end to end. Real
 * host unit tests (PIN KDFs, CRCs, base64, CBOR, framing, capability policy,
 * CP437) are added under sibling `test/host/<name>/` folders.
 */

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/** \brief Trivial assertion confirming the harness links and runs. */
static void test_native_harness_runs(void) {
    TEST_ASSERT_EQUAL_INT(2, 1 + 1);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_native_harness_runs);
    return UNITY_END();
}
