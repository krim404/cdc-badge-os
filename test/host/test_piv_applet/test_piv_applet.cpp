// Host unit tests for the PIV applet state machine (mod_piv).
//   - SELECT APT framing
//   - VERIFY: retry query / wrong-PIN 63Cx / success / logout
//   - GENERAL AUTHENTICATE sign gating + 7C{82 DER} framing
//   - AES management-key mutual authentication (identity cipher)
//   - GENERATE gating on management auth
//   - GET DATA CHUID / Discovery object framing
//   - command-chaining reassembly for PUT DATA
//   - DER ECDSA signature encoding edge cases
// Run with: pio test -e native
//
// The applet reaches hardware only through piv_backend_t, injected here as an
// in-memory fake, so the whole command surface runs on the host.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "cdc_log.h"

#include "../../../components/cdc_scard/src/apdu.cpp"
#include "../../../components/mod_piv/src/piv_tlv.cpp"
#include "../../../components/mod_piv/src/piv_objects.cpp"
#include "../../../components/mod_piv/src/piv_applet.cpp"

extern "C" void log_write(log_level_t, const char*, const char*, ...) {}

using namespace cdc::mod_piv;

// --- fake backend ----------------------------------------------------------
struct Fake {
    bool keyPresent[4] = {false, false, false, false};  // 9A,9C,9D,9E
    const char* pin = "123456";
    uint8_t retries = 3;
    bool blocked = false;
    uint8_t witness[16];
    bool rngReturns = true;
    piv_state_t state;
};
static Fake g_fake;

static int keyIdx(uint8_t ref) {
    switch (ref) {
        case PIV_KEY_9A: return 0;
        case PIV_KEY_9C: return 1;
        case PIV_KEY_9D: return 2;
        case PIV_KEY_9E: return 3;
        default: return -1;
    }
}

static bool fkPresent(uint8_t ref) { int i = keyIdx(ref); return i >= 0 && g_fake.keyPresent[i]; }
static bool fkSign(uint8_t ref, const uint8_t[32], uint8_t rs[64]) {
    if (!fkPresent(ref)) return false;
    // Deterministic signature: r has a leading zero + high bit, s is plain.
    memset(rs, 0, 64);
    rs[0] = 0x00; rs[1] = 0x80; rs[31] = 0x11;   // r
    rs[32] = 0x22; rs[63] = 0x33;                // s
    return true;
}
static bool fkGenerate(uint8_t ref, uint8_t pub[65]) {
    int i = keyIdx(ref); if (i < 0) return false;
    g_fake.keyPresent[i] = true;
    pub[0] = 0x04; memset(pub + 1, 0xAB, 64);
    return true;
}
static bool fkPubkey(uint8_t ref, uint8_t pub[65]) {
    if (!fkPresent(ref)) return false;
    pub[0] = 0x04; memset(pub + 1, 0xCD, 64); return true;
}
static bool fkEcdh(const uint8_t[65], uint8_t sharedX[32]) { memset(sharedX, 0x5A, 32); return true; }
static bool fkPinVerify(const char* pin) { return strcmp(pin, g_fake.pin) == 0; }
static uint8_t fkPinRetries() { return g_fake.retries; }
static bool fkPinBlocked() { return g_fake.blocked; }
static bool fkMgmtEncrypt(const uint8_t in[16], uint8_t out[16]) { memcpy(out, in, 16); return true; }
static bool fkMgmtSetKey(uint8_t, const uint8_t*, uint8_t) { return true; }
static bool fkStateLoad(piv_state_t* out) { *out = g_fake.state; return true; }
static int fkObjectLoad(uint32_t, uint8_t*, size_t) { return -1; }
static bool fkObjectStore(uint32_t, const uint8_t*, size_t) { return true; }
static bool fkRng(uint8_t* buf, size_t len) {
    if (!g_fake.rngReturns) return false;
    for (size_t i = 0; i < len; i++) buf[i] = static_cast<uint8_t>(0x40 + i);
    if (len == 16) memcpy(g_fake.witness, buf, 16);
    return true;
}

static const piv_backend_t kBackend = {
    fkPresent, fkSign, fkGenerate, fkPubkey, fkEcdh,
    fkPinVerify, fkPinRetries, fkPinBlocked,
    fkMgmtEncrypt, fkMgmtSetKey,
    fkStateLoad, fkObjectLoad, fkObjectStore, fkRng,
};

static const scard_applet_t* g_applet = nullptr;
static uint8_t g_resp[512];

void setUp() {
    g_fake = Fake{};
    memset(&g_fake.state, 0, sizeof(g_fake.state));
    g_fake.state.version = 1;
    g_fake.state.mgmt_alg = PIV_ALG_AES192;
    g_fake.state.mgmt_is_default = 1;
    for (int i = 0; i < 16; i++) g_fake.state.guid[i] = static_cast<uint8_t>(i + 1);
    piv_set_backend(&kBackend);
    g_applet = piv_applet();
    g_applet->deselect();
}
void tearDown() {}

static int send(const uint8_t* apdu, size_t len) {
    return g_applet->process_apdu(apdu, len, g_resp, sizeof(g_resp));
}
static uint16_t swOf(int n) {
    return static_cast<uint16_t>((g_resp[n - 2] << 8) | g_resp[n - 1]);
}

// --- tests -----------------------------------------------------------------
static void test_select_returns_apt() {
    const uint8_t aid[] = {0xA0, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x10, 0x00};
    uint8_t apdu[32] = {0x00, 0xA4, 0x04, 0x00, sizeof(aid)};
    memcpy(apdu + 5, aid, sizeof(aid));
    int n = send(apdu, 5 + sizeof(aid));
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_OK, swOf(n));
    TEST_ASSERT_EQUAL_HEX8(PIV_TAG_APT, g_resp[0]);
}

static void test_verify_retry_query_and_wrong_pin() {
    // Empty-body VERIFY = retry query -> 63C3 (3 retries).
    uint8_t query[] = {0x00, 0x20, 0x00, 0x80};
    int n = send(query, sizeof(query));
    TEST_ASSERT_EQUAL_HEX16(0x63C3, swOf(n));

    // Wrong PIN "999999" padded with FF -> 63Cx.
    g_fake.retries = 2;
    uint8_t bad[] = {0x00, 0x20, 0x00, 0x80, 0x08,
                     '9', '9', '9', '9', '9', '9', 0xFF, 0xFF};
    n = send(bad, sizeof(bad));
    TEST_ASSERT_EQUAL_HEX16(0x63C2, swOf(n));
}

static void test_verify_success_and_logout() {
    uint8_t ok[] = {0x00, 0x20, 0x00, 0x80, 0x08,
                    '1', '2', '3', '4', '5', '6', 0xFF, 0xFF};
    int n = send(ok, sizeof(ok));
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_OK, swOf(n));

    // After verify, retry query reports 9000.
    uint8_t query[] = {0x00, 0x20, 0x00, 0x80};
    n = send(query, sizeof(query));
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_OK, swOf(n));

    // Logout (P1=FF) then query reports the counter again.
    uint8_t logout[] = {0x00, 0x20, 0xFF, 0x80};
    n = send(logout, sizeof(logout));
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_OK, swOf(n));
    n = send(query, sizeof(query));
    TEST_ASSERT_EQUAL_HEX16(0x63C3, swOf(n));
}

static void verifyPin() {
    uint8_t ok[] = {0x00, 0x20, 0x00, 0x80, 0x08,
                    '1', '2', '3', '4', '5', '6', 0xFF, 0xFF};
    send(ok, sizeof(ok));
}

static void test_general_auth_sign_requires_pin() {
    g_fake.keyPresent[0] = true;  // 9A present
    // 7C { 82 00, 81 20 <32-byte digest> }
    uint8_t inner[40];
    size_t ip = 0;
    tlvWrite(inner, sizeof(inner), &ip, PIV_TAG_RESPONSE, nullptr, 0);
    uint8_t digest[32]; memset(digest, 0x5A, 32);
    tlvWrite(inner, sizeof(inner), &ip, PIV_TAG_CHALLENGE, digest, 32);
    uint8_t data[64]; size_t dp = 0;
    tlvWrite(data, sizeof(data), &dp, PIV_TAG_DYN_AUTH, inner, ip);

    uint8_t apdu[80] = {0x00, 0x87, PIV_ALG_ECC_P256, PIV_KEY_9A, static_cast<uint8_t>(dp)};
    memcpy(apdu + 5, data, dp);
    apdu[5 + dp] = 0x00;  // Le

    int n = send(apdu, 5 + dp + 1);
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_SECURITY_STATUS, swOf(n));

    verifyPin();
    n = send(apdu, 5 + dp + 1);
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_OK, swOf(n));
    // Response wrapped in 7C, inner tag 82, inner DER SEQUENCE.
    TEST_ASSERT_EQUAL_HEX8(PIV_TAG_DYN_AUTH, g_resp[0]);
    const uint8_t* v = nullptr; size_t vl = 0;
    TEST_ASSERT_TRUE(tlvFind(g_resp, n - 2, PIV_TAG_DYN_AUTH, &v, &vl));
    const uint8_t* sig = nullptr; size_t sl = 0;
    TEST_ASSERT_TRUE(tlvFind(v, vl, PIV_TAG_RESPONSE, &sig, &sl));
    TEST_ASSERT_EQUAL_HEX8(0x30, sig[0]);  // DER SEQUENCE
}

static void test_generate_requires_mgmt_auth() {
    // AC { 80 01 11 }
    uint8_t inner[8]; size_t ip = 0;
    uint8_t alg = PIV_ALG_ECC_P256;
    tlvWrite(inner, sizeof(inner), &ip, 0x80, &alg, 1);
    uint8_t data[16]; size_t dp = 0;
    tlvWrite(data, sizeof(data), &dp, PIV_TAG_ALG_ID, inner, ip);
    uint8_t apdu[32] = {0x00, 0x47, 0x00, PIV_KEY_9A, static_cast<uint8_t>(dp)};
    memcpy(apdu + 5, data, dp);
    apdu[5 + dp] = 0x00;

    int n = send(apdu, 5 + dp + 1);
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_SECURITY_STATUS, swOf(n));
}

static void doMgmtAuth() {
    // Step 1: request witness (7C { 80 00 }).
    uint8_t inner1[4]; size_t ip1 = 0;
    tlvWrite(inner1, sizeof(inner1), &ip1, PIV_TAG_WITNESS, nullptr, 0);
    uint8_t d1[16]; size_t dp1 = 0;
    tlvWrite(d1, sizeof(d1), &dp1, PIV_TAG_DYN_AUTH, inner1, ip1);
    uint8_t a1[32] = {0x00, 0x87, PIV_ALG_AES192, PIV_KEY_9B, static_cast<uint8_t>(dp1)};
    memcpy(a1 + 5, d1, dp1);
    a1[5 + dp1] = 0x00;
    int n = send(a1, 5 + dp1 + 1);
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_OK, swOf(n));

    // Recover the (identity-encrypted) witness from the response.
    const uint8_t* tmpl = nullptr; size_t tl = 0;
    tlvFind(g_resp, n - 2, PIV_TAG_DYN_AUTH, &tmpl, &tl);
    const uint8_t* wit = nullptr; size_t wl = 0;
    tlvFind(tmpl, tl, PIV_TAG_WITNESS, &wit, &wl);
    uint8_t witness[16]; memcpy(witness, wit, 16);

    // Step 2: return witness + our challenge.
    uint8_t chal[16]; memset(chal, 0x77, 16);
    uint8_t inner2[48]; size_t ip2 = 0;
    tlvWrite(inner2, sizeof(inner2), &ip2, PIV_TAG_WITNESS, witness, 16);
    tlvWrite(inner2, sizeof(inner2), &ip2, PIV_TAG_CHALLENGE, chal, 16);
    uint8_t d2[64]; size_t dp2 = 0;
    tlvWrite(d2, sizeof(d2), &dp2, PIV_TAG_DYN_AUTH, inner2, ip2);
    uint8_t a2[80] = {0x00, 0x87, PIV_ALG_AES192, PIV_KEY_9B, static_cast<uint8_t>(dp2)};
    memcpy(a2 + 5, d2, dp2);
    a2[5 + dp2] = 0x00;
    n = send(a2, 5 + dp2 + 1);
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_OK, swOf(n));
}

static void test_mgmt_auth_then_generate() {
    doMgmtAuth();
    // AC { 80 01 11 }
    uint8_t inner[8]; size_t ip = 0;
    uint8_t alg = PIV_ALG_ECC_P256;
    tlvWrite(inner, sizeof(inner), &ip, 0x80, &alg, 1);
    uint8_t data[16]; size_t dp = 0;
    tlvWrite(data, sizeof(data), &dp, PIV_TAG_ALG_ID, inner, ip);
    uint8_t apdu[32] = {0x00, 0x47, 0x00, PIV_KEY_9A, static_cast<uint8_t>(dp)};
    memcpy(apdu + 5, data, dp);
    apdu[5 + dp] = 0x00;
    int n = send(apdu, 5 + dp + 1);
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_OK, swOf(n));
    TEST_ASSERT_EQUAL_HEX8(0x7F, g_resp[0]);  // 7F49 public-key template
    TEST_ASSERT_EQUAL_HEX8(0x49, g_resp[1]);
}

static void test_get_data_chuid_and_discovery() {
    // CHUID: 5C 03 5F C1 02
    uint8_t chuid[] = {0x00, 0xCB, 0x3F, 0xFF, 0x05, 0x5C, 0x03, 0x5F, 0xC1, 0x02, 0x00};
    int n = send(chuid, sizeof(chuid));
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_OK, swOf(n));
    TEST_ASSERT_EQUAL_HEX8(PIV_TAG_DATA_OBJECT, g_resp[0]);  // wrapped in 0x53

    // Discovery: 5C 01 7E -> bare 7E, not wrapped in 53.
    uint8_t disc[] = {0x00, 0xCB, 0x3F, 0xFF, 0x03, 0x5C, 0x01, 0x7E, 0x00};
    n = send(disc, sizeof(disc));
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_OK, swOf(n));
    TEST_ASSERT_EQUAL_HEX8(PIV_TAG_DISCOVERY, g_resp[0]);
}

static void test_command_chaining_put_data() {
    doMgmtAuth();
    // Build a full PUT DATA body (5C 03 5FC10A || 53 <cert>) and split it across
    // two chained blocks.
    uint8_t body[300];
    size_t bp = 0;
    uint8_t tag[] = {0x5F, 0xC1, 0x0A};
    tlvWrite(body, sizeof(body), &bp, PIV_TAG_TAG_LIST, tag, sizeof(tag));
    uint8_t cert[200];
    memset(cert, 0xEE, sizeof(cert));
    tlvWrite(body, sizeof(body), &bp, PIV_TAG_DATA_OBJECT, cert, sizeof(cert));

    size_t half = bp / 2;
    // Block 1: CLA 0x10 (chaining).
    uint8_t b1[160] = {0x10, 0xDB, 0x3F, 0xFF, static_cast<uint8_t>(half)};
    memcpy(b1 + 5, body, half);
    int n = send(b1, 5 + half);
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_OK, swOf(n));

    // Block 2: final (CLA 0x00).
    size_t rest = bp - half;
    uint8_t b2[220] = {0x00, 0xDB, 0x3F, 0xFF, static_cast<uint8_t>(rest)};
    memcpy(b2 + 5, body + half, rest);
    n = send(b2, 5 + rest);
    TEST_ASSERT_EQUAL_HEX16(PIV_SW_OK, swOf(n));
}

static void test_der_encoding_edge_cases() {
    // r with high bit set requires a 0x00 pad; s with a leading zero drops it.
    uint8_t rs[64];
    memset(rs, 0, 64);
    rs[0] = 0x80; rs[31] = 0x01;   // r: 0x80..01 -> pad
    rs[32] = 0x00; rs[33] = 0x7F;  // s: leading zero stripped, 0x7F no pad
    uint8_t out[80];
    size_t n = derEncodeEcdsaSig(rs, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_HEX8(0x30, out[0]);      // SEQUENCE
    TEST_ASSERT_EQUAL_HEX8(0x02, out[2]);      // INTEGER r
    TEST_ASSERT_EQUAL_HEX8(0x21, out[3]);      // r length 33 (padded)
    TEST_ASSERT_EQUAL_HEX8(0x00, out[4]);      // pad byte
    TEST_ASSERT_EQUAL_HEX8(0x80, out[5]);      // r MSB
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_select_returns_apt);
    RUN_TEST(test_verify_retry_query_and_wrong_pin);
    RUN_TEST(test_verify_success_and_logout);
    RUN_TEST(test_general_auth_sign_requires_pin);
    RUN_TEST(test_generate_requires_mgmt_auth);
    RUN_TEST(test_mgmt_auth_then_generate);
    RUN_TEST(test_get_data_chuid_and_discovery);
    RUN_TEST(test_command_chaining_put_data);
    RUN_TEST(test_der_encoding_edge_cases);
    return UNITY_END();
}
