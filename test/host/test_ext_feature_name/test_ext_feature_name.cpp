/**
 * \file
 * \brief Host unit test for the external-feature name validator.
 *
 * Direct unit test: includes the real header-only validator from
 * components/plugin_manager (no ESP-IDF dependencies).
 */

#include "../../../components/plugin_manager/include/plugin_manager/ExtFeatureName.h"

#include <string>
#include <unity.h>

using cdc::plugin_manager::isValidExtFeatureName;
using cdc::plugin_manager::EXT_FEATURE_NAME_MAX;

void setUp(void) {}
void tearDown(void) {}

static void test_valid_names(void) {
    TEST_ASSERT_TRUE(isValidExtFeatureName("thermo_print"));
    TEST_ASSERT_TRUE(isValidExtFeatureName("a"));
    TEST_ASSERT_TRUE(isValidExtFeatureName("a1"));
    TEST_ASSERT_TRUE(isValidExtFeatureName("print_v2"));
    TEST_ASSERT_TRUE(isValidExtFeatureName("x_______y"));
}

static void test_invalid_first_char(void) {
    TEST_ASSERT_FALSE(isValidExtFeatureName("1print"));
    TEST_ASSERT_FALSE(isValidExtFeatureName("_print"));
    TEST_ASSERT_FALSE(isValidExtFeatureName("Print"));
    TEST_ASSERT_FALSE(isValidExtFeatureName(" print"));
}

static void test_invalid_chars(void) {
    TEST_ASSERT_FALSE(isValidExtFeatureName("thermo-print"));
    TEST_ASSERT_FALSE(isValidExtFeatureName("thermo print"));
    TEST_ASSERT_FALSE(isValidExtFeatureName("thermo.print"));
    TEST_ASSERT_FALSE(isValidExtFeatureName("thermoPrint"));
    TEST_ASSERT_FALSE(isValidExtFeatureName("umlaut\xc3\xa4"));
}

static void test_empty_and_null(void) {
    TEST_ASSERT_FALSE(isValidExtFeatureName(""));
    TEST_ASSERT_FALSE(isValidExtFeatureName(nullptr));
}

static void test_length_bounds(void) {
    const std::string max_ok(EXT_FEATURE_NAME_MAX - 1, 'a');
    TEST_ASSERT_TRUE(isValidExtFeatureName(max_ok.c_str()));
    const std::string too_long(EXT_FEATURE_NAME_MAX, 'a');
    TEST_ASSERT_FALSE(isValidExtFeatureName(too_long.c_str()));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_names);
    RUN_TEST(test_invalid_first_char);
    RUN_TEST(test_invalid_chars);
    RUN_TEST(test_empty_and_null);
    RUN_TEST(test_length_bounds);
    return UNITY_END();
}
