// Host unit tests for the smartcard applet dispatcher (cdc_scard):
//   - SELECT-by-AID routing with prefix matching (longest prefix wins)
//   - pre-SELECT traffic going to the default applet
//   - deselect callbacks on applet switch and card reset
//   - legacy pass-through of unknown AIDs and chain blocks
// Run with: pio test -e native

#include <unity.h>

#include <cstdint>
#include <cstring>

#include "../../../components/cdc_scard/src/apdu.cpp"
#include "../../../components/cdc_scard/src/scard.cpp"

// Stub for the cdc_log declaration pulled in by scard.cpp.
extern "C" void log_write(log_level_t, const char*, const char*, ...) {}

// --- stub applets -----------------------------------------------------------

struct StubState {
    int apdu_calls;
    int deselect_calls;
    uint8_t last_ins;
};

static StubState g_alpha;
static StubState g_beta;
static StubState g_gamma;

// Each stub answers with a distinctive SW1 so tests can tell who responded.
static int stub_process(StubState* s, uint8_t sw1, const uint8_t* cmd, size_t cmd_len,
                        uint8_t* resp, size_t resp_max) {
    (void)resp_max;
    s->apdu_calls++;
    s->last_ins = cmd_len >= 2 ? cmd[1] : 0;
    resp[0] = sw1;
    resp[1] = 0x00;
    return 2;
}

static int alpha_process(const uint8_t* cmd, size_t cmd_len, uint8_t* resp, size_t resp_max) {
    return stub_process(&g_alpha, 0xA1, cmd, cmd_len, resp, resp_max);
}
static int beta_process(const uint8_t* cmd, size_t cmd_len, uint8_t* resp, size_t resp_max) {
    return stub_process(&g_beta, 0xB1, cmd, cmd_len, resp, resp_max);
}
static int gamma_process(const uint8_t* cmd, size_t cmd_len, uint8_t* resp, size_t resp_max) {
    return stub_process(&g_gamma, 0xC1, cmd, cmd_len, resp, resp_max);
}

static void alpha_deselect() { g_alpha.deselect_calls++; }
static void beta_deselect() { g_beta.deselect_calls++; }
static void gamma_deselect() { g_gamma.deselect_calls++; }

// OpenPGP-like 6-byte RID and PIV-like 5-byte RID.
static const uint8_t kAidAlpha[] = {0xD2, 0x76, 0x00, 0x01, 0x24, 0x01};
static const uint8_t kAidBeta[]  = {0xA0, 0x00, 0x00, 0x03, 0x08};
// Proper prefix of kAidAlpha, for longest-prefix disambiguation.
static const uint8_t kAidGamma[] = {0xD2, 0x76, 0x00, 0x01};

static const scard_applet_t kAlpha = {"alpha", kAidAlpha, sizeof(kAidAlpha),
                                      alpha_process, alpha_deselect};
static const scard_applet_t kBeta = {"beta", kAidBeta, sizeof(kAidBeta),
                                     beta_process, beta_deselect};
static const scard_applet_t kGamma = {"gamma", kAidGamma, sizeof(kAidGamma),
                                      gamma_process, gamma_deselect};

// --- helpers ----------------------------------------------------------------

static uint8_t g_resp[64];

static int send_select(const uint8_t* aid, uint8_t aid_len, uint8_t cla = 0x00) {
    uint8_t apdu[32] = {cla, 0xA4, 0x04, 0x00, aid_len};
    memcpy(apdu + 5, aid, aid_len);
    return scard_dispatch_apdu(apdu, 5u + aid_len, g_resp, sizeof(g_resp));
}

static int send_get_data() {
    static const uint8_t apdu[] = {0x00, 0xCA, 0x00, 0x4F, 0x00};
    return scard_dispatch_apdu(apdu, sizeof(apdu), g_resp, sizeof(g_resp));
}

void setUp() {
    // Reset dispatcher internals directly (same translation unit).
    s_count = 0;
    s_current = -1;
    s_default = -1;
    g_alpha = StubState{};
    g_beta = StubState{};
    g_gamma = StubState{};
}

void tearDown() {}

// --- registration -----------------------------------------------------------

void test_register_rejects_duplicates_and_invalid() {
    TEST_ASSERT_TRUE(scard_register_applet(&kAlpha, true));
    TEST_ASSERT_FALSE(scard_register_applet(&kAlpha, false));

    scard_applet_t broken = kBeta;
    broken.process_apdu = nullptr;
    TEST_ASSERT_FALSE(scard_register_applet(&broken, false));
}

void test_register_rejects_when_table_full() {
    scard_applet_t clones[SCARD_MAX_APPLETS + 1];
    static const char* names[] = {"c0", "c1", "c2", "c3", "c4"};
    for (int i = 0; i <= SCARD_MAX_APPLETS; i++) {
        clones[i] = kAlpha;
        clones[i].name = names[i];
        clones[i].aid = kAidBeta;  // distinct from alpha, irrelevant here
        clones[i].aid_len = sizeof(kAidBeta);
    }
    for (int i = 0; i < SCARD_MAX_APPLETS; i++) {
        TEST_ASSERT_TRUE(scard_register_applet(&clones[i], false));
    }
    TEST_ASSERT_FALSE(scard_register_applet(&clones[SCARD_MAX_APPLETS], false));
}

// --- routing ----------------------------------------------------------------

void test_non_select_before_select_goes_to_default() {
    scard_register_applet(&kAlpha, true);
    scard_register_applet(&kBeta, false);

    int len = send_get_data();

    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_HEX8(0xA1, g_resp[0]);
    TEST_ASSERT_EQUAL_INT(1, g_alpha.apdu_calls);
    TEST_ASSERT_EQUAL_INT(0, g_beta.apdu_calls);
}

void test_non_select_without_default_returns_6a82() {
    scard_register_applet(&kBeta, false);

    int len = send_get_data();

    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_HEX8(0x6A, g_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0x82, g_resp[1]);
    TEST_ASSERT_EQUAL_INT(0, g_beta.apdu_calls);
}

void test_select_switches_applet_and_forwards_select() {
    scard_register_applet(&kAlpha, true);
    scard_register_applet(&kBeta, false);

    int len = send_select(kAidBeta, sizeof(kAidBeta));

    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_HEX8(0xB1, g_resp[0]);
    TEST_ASSERT_EQUAL_INT(1, g_beta.apdu_calls);
    TEST_ASSERT_EQUAL_HEX8(0xA4, g_beta.last_ins);

    send_get_data();
    TEST_ASSERT_EQUAL_INT(2, g_beta.apdu_calls);
    TEST_ASSERT_EQUAL_INT(0, g_alpha.apdu_calls);
}

void test_select_full_aid_matches_prefix() {
    scard_register_applet(&kAlpha, true);

    // Full 16-byte OpenPGP AID: RID + version + manufacturer + serial + RFU.
    uint8_t full_aid[16] = {0xD2, 0x76, 0x00, 0x01, 0x24, 0x01, 0x03, 0x04,
                            0x00, 0x2A, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00};
    int len = send_select(full_aid, sizeof(full_aid));

    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_HEX8(0xA1, g_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA4, g_alpha.last_ins);
}

void test_longest_prefix_wins() {
    scard_register_applet(&kGamma, false);  // D2 76 00 01 (4 bytes)
    scard_register_applet(&kAlpha, true);   // D2 76 00 01 24 01 (6 bytes)

    int len = send_select(kAidAlpha, sizeof(kAidAlpha));

    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_HEX8(0xA1, g_resp[0]);
    TEST_ASSERT_EQUAL_INT(0, g_gamma.apdu_calls);

    // A 4-byte SELECT only reaches the shorter registration.
    int len2 = send_select(kAidGamma, sizeof(kAidGamma));
    TEST_ASSERT_EQUAL_INT(2, len2);
    TEST_ASSERT_EQUAL_HEX8(0xC1, g_resp[0]);
}

void test_unknown_aid_forwarded_to_current_applet() {
    scard_register_applet(&kAlpha, true);
    send_select(kAidAlpha, sizeof(kAidAlpha));

    static const uint8_t unknown[] = {0xF0, 0x11, 0x22, 0x33, 0x44};
    int len = send_select(unknown, sizeof(unknown));

    // The selected applet answers itself (legacy 6A82 path) and stays selected.
    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_HEX8(0xA1, g_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA4, g_alpha.last_ins);
    TEST_ASSERT_EQUAL_INT(0, g_alpha.deselect_calls);
}

void test_unknown_aid_without_selection_returns_6a82() {
    scard_register_applet(&kAlpha, true);

    static const uint8_t unknown[] = {0xF0, 0x11, 0x22, 0x33, 0x44};
    int len = send_select(unknown, sizeof(unknown));

    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_HEX8(0x6A, g_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0x82, g_resp[1]);
    TEST_ASSERT_EQUAL_INT(0, g_alpha.apdu_calls);
}

void test_chain_block_select_not_intercepted() {
    scard_register_applet(&kAlpha, true);
    scard_register_applet(&kBeta, false);
    send_select(kAidAlpha, sizeof(kAidAlpha));

    // CLA 0x10 (chaining) with SELECT-shaped body must pass through to the
    // current applet, not switch to beta.
    send_select(kAidBeta, sizeof(kAidBeta), 0x10);

    TEST_ASSERT_EQUAL_INT(2, g_alpha.apdu_calls);
    TEST_ASSERT_EQUAL_INT(0, g_beta.apdu_calls);
}

// --- deselect / reset -------------------------------------------------------

void test_deselect_fired_on_switch_not_on_reselect() {
    scard_register_applet(&kAlpha, true);
    scard_register_applet(&kBeta, false);

    send_select(kAidAlpha, sizeof(kAidAlpha));
    send_select(kAidAlpha, sizeof(kAidAlpha));  // re-SELECT, forwarded
    TEST_ASSERT_EQUAL_INT(0, g_alpha.deselect_calls);
    TEST_ASSERT_EQUAL_INT(2, g_alpha.apdu_calls);

    send_select(kAidBeta, sizeof(kAidBeta));
    TEST_ASSERT_EQUAL_INT(1, g_alpha.deselect_calls);
    TEST_ASSERT_EQUAL_INT(0, g_beta.deselect_calls);
}

void test_reset_deselects_and_restores_default_routing() {
    scard_register_applet(&kAlpha, true);
    scard_register_applet(&kBeta, false);
    send_select(kAidBeta, sizeof(kAidBeta));

    scard_reset();
    TEST_ASSERT_EQUAL_INT(1, g_beta.deselect_calls);

    send_get_data();
    TEST_ASSERT_EQUAL_INT(1, g_alpha.apdu_calls);  // back to default
}

void test_unregister_current_applet_deselects_it() {
    scard_register_applet(&kAlpha, true);
    scard_register_applet(&kBeta, false);
    send_select(kAidBeta, sizeof(kAidBeta));

    scard_unregister_applet("beta");
    TEST_ASSERT_EQUAL_INT(1, g_beta.deselect_calls);

    send_get_data();
    TEST_ASSERT_EQUAL_INT(1, g_alpha.apdu_calls);  // default still valid
}

void test_dispatch_with_empty_registry_returns_6a82() {
    int len = send_get_data();

    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_HEX8(0x6A, g_resp[0]);
    TEST_ASSERT_EQUAL_HEX8(0x82, g_resp[1]);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_register_rejects_duplicates_and_invalid);
    RUN_TEST(test_register_rejects_when_table_full);
    RUN_TEST(test_non_select_before_select_goes_to_default);
    RUN_TEST(test_non_select_without_default_returns_6a82);
    RUN_TEST(test_select_switches_applet_and_forwards_select);
    RUN_TEST(test_select_full_aid_matches_prefix);
    RUN_TEST(test_longest_prefix_wins);
    RUN_TEST(test_unknown_aid_forwarded_to_current_applet);
    RUN_TEST(test_unknown_aid_without_selection_returns_6a82);
    RUN_TEST(test_chain_block_select_not_intercepted);
    RUN_TEST(test_deselect_fired_on_switch_not_on_reselect);
    RUN_TEST(test_reset_deselects_and_restores_default_routing);
    RUN_TEST(test_unregister_current_applet_deselects_it);
    RUN_TEST(test_dispatch_with_empty_registry_returns_6a82);
    return UNITY_END();
}
