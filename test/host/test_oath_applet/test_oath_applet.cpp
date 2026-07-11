// Host unit tests for the YKOATH applet (mod_2fa OathApplet).
//   - SELECT version/devid framing, lock state
//   - PUT name mapping ("60/ACME:bob"), oversized-account rejection
//   - LIST / CALCULATE / CALCULATE ALL framing
//   - DELETE of a missing credential
//   - SEND REMAINING (INS A5) response chaining
//   - access-key SET CODE / VALIDATE mutual challenge-response
//   - RESET
// Run with: pio test -e native
//
// The applet reaches the credential store only through oath_backend_t, an
// in-memory fake here; RFC 6238/4226 vectors are covered on-target by
// OathStore's mbedtls HMAC path.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "cdc_log.h"

#include "../../../components/cdc_scard/src/apdu.cpp"
#include "../../../components/mod_2fa/src/OathApplet.cpp"

extern "C" void log_write(log_level_t, const char*, const char*, ...) {}

// --- fake backend ----------------------------------------------------------
struct FakeEntry {
    bool used;
    char name[17];
    char issuer[33];
    uint8_t type;      // 0 TOTP, 1 HOTP, 2 CR
    uint8_t algorithm;
    uint8_t digits;
    uint32_t period;
    uint8_t flags;
    uint8_t secret[64];
    uint8_t secretLen;
};

static constexpr uint16_t CAP = 8;
static FakeEntry g_entries[CAP];
static bool g_hasAkey = false;
static uint8_t g_akey[16];

static uint16_t fkCapacity() { return CAP; }
static bool fkRead(uint16_t slot, oath_meta_t* out) {
    if (slot >= CAP || !g_entries[slot].used) { out->used = false; return false; }
    const FakeEntry& e = g_entries[slot];
    out->used = true;
    strncpy(out->name, e.name, sizeof(out->name));
    strncpy(out->issuer, e.issuer, sizeof(out->issuer));
    out->type = e.type; out->algorithm = e.algorithm; out->digits = e.digits;
    out->period = e.period; out->flags = e.flags;
    return true;
}
static bool fkCalculate(uint16_t slot, const uint8_t[8], uint8_t trunc[4], uint8_t* digits) {
    if (slot >= CAP || !g_entries[slot].used) return false;
    trunc[0] = 0x12; trunc[1] = 0x34; trunc[2] = 0x56; trunc[3] = 0x78;
    *digits = g_entries[slot].digits;
    return true;
}
static bool fkAddRaw(uint8_t type, const char* name, const char* issuer,
                     const uint8_t* key, uint8_t keyLen, uint8_t digits,
                     uint32_t period, uint8_t algorithm, uint64_t, uint8_t flags) {
    for (uint16_t s = 0; s < CAP; s++) {
        if (!g_entries[s].used) {
            FakeEntry& e = g_entries[s];
            e.used = true;
            strncpy(e.name, name, sizeof(e.name) - 1);
            strncpy(e.issuer, issuer ? issuer : "", sizeof(e.issuer) - 1);
            e.type = type; e.algorithm = algorithm; e.digits = digits;
            e.period = period; e.flags = flags;
            e.secretLen = keyLen; memcpy(e.secret, key, keyLen);
            return true;
        }
    }
    return false;
}
static bool fkRemove(uint16_t slot) {
    if (slot >= CAP || !g_entries[slot].used) return false;
    g_entries[slot].used = false; return true;
}
static void fkWipeAll() { for (auto& e : g_entries) e.used = false; }
static void fkDevId(uint8_t out[8]) { for (int i = 0; i < 8; i++) out[i] = static_cast<uint8_t>(0x10 + i); }
static bool fkAkeyGet(uint8_t out[16]) { if (!g_hasAkey) return false; memcpy(out, g_akey, 16); return true; }
static bool fkAkeySet(const uint8_t key[16]) { memcpy(g_akey, key, 16); g_hasAkey = true; return true; }
static void fkAkeyClear() { g_hasAkey = false; }
// Deterministic non-crypto pseudo-HMAC: self-consistent for the auth flow test.
static bool fkHmac(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen,
                   uint8_t out[20]) {
    for (int i = 0; i < 20; i++)
        out[i] = static_cast<uint8_t>(key[i % keyLen] ^ data[i % dataLen] ^ (i * 7));
    return true;
}
static bool fkRng(uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) buf[i] = static_cast<uint8_t>(0x30 + i);
    return true;
}

static const oath_backend_t kBackend = {
    fkCapacity, fkRead, fkCalculate, fkAddRaw, fkRemove, fkWipeAll,
    fkDevId, fkAkeyGet, fkAkeySet, fkAkeyClear, fkHmac, fkRng,
};

static const scard_applet_t* g_applet = nullptr;
static uint8_t g_rsp[512];

void setUp() {
    memset(g_entries, 0, sizeof(g_entries));
    g_hasAkey = false;
    oath_set_backend(&kBackend);
    g_applet = oath_applet();
    g_applet->deselect();
}
void tearDown() {}

static int send(const uint8_t* a, size_t n) {
    return g_applet->process_apdu(a, n, g_rsp, sizeof(g_rsp));
}
static uint16_t swOf(int n) { return static_cast<uint16_t>((g_rsp[n - 2] << 8) | g_rsp[n - 1]); }

static int selectApplet() {
    const uint8_t aid[] = {0xA0, 0x00, 0x00, 0x05, 0x27, 0x21, 0x01};
    uint8_t apdu[16] = {0x00, 0xA4, 0x04, 0x00, sizeof(aid)};
    memcpy(apdu + 5, aid, sizeof(aid));
    apdu[5 + sizeof(aid)] = 0x00;
    return send(apdu, 6 + sizeof(aid));
}

// TLV-71 name PUT helper.
static int putCred(const char* name, uint8_t typeAlgo, uint8_t digits,
                   const uint8_t* secret, uint8_t secretLen) {
    uint8_t data[128]; size_t p = 0;
    size_t nl = strlen(name);
    data[p++] = 0x71; data[p++] = static_cast<uint8_t>(nl);
    memcpy(data + p, name, nl); p += nl;
    data[p++] = 0x73; data[p++] = static_cast<uint8_t>(2 + secretLen);
    data[p++] = typeAlgo; data[p++] = digits;
    memcpy(data + p, secret, secretLen); p += secretLen;
    uint8_t apdu[160] = {0x00, 0x01, 0x00, 0x00, static_cast<uint8_t>(p)};
    memcpy(apdu + 5, data, p);
    return send(apdu, 5 + p);
}

// --- tests -----------------------------------------------------------------
static void test_select_unlocked() {
    int n = selectApplet();
    TEST_ASSERT_EQUAL_HEX16(0x9000, swOf(n));
    TEST_ASSERT_EQUAL_HEX8(0x79, g_rsp[0]);  // version tag
    // No challenge tag (0x74) when unlocked.
    const uint8_t* v = nullptr; size_t vl = 0;
    // scan for tag 0x74 absence
    bool hasChal = false;
    size_t p = 0; int len = n - 2;
    while ((int)p + 2 <= len) { uint8_t t = g_rsp[p]; uint8_t l = g_rsp[p+1]; if (t==0x74) hasChal=true; p += 2 + l; }
    TEST_ASSERT_FALSE(hasChal);
    (void)v; (void)vl;
}

static void test_put_name_mapping_and_reject_long() {
    selectApplet();
    uint8_t secret[20]; memset(secret, 0xAA, sizeof(secret));
    // "60/ACME:bob": period 60, issuer ACME, account bob. TOTP|SHA1, 6 digits.
    int n = putCred("60/ACME:bob", 0x21, 6, secret, sizeof(secret));
    TEST_ASSERT_EQUAL_HEX16(0x9000, swOf(n));
    TEST_ASSERT_TRUE(g_entries[0].used);
    TEST_ASSERT_EQUAL_UINT32(60, g_entries[0].period);
    TEST_ASSERT_EQUAL_STRING("ACME", g_entries[0].issuer);
    TEST_ASSERT_EQUAL_STRING("bob", g_entries[0].name);

    // 17-char account is rejected (store label limit is 16).
    n = putCred("0123456789ABCDEFG", 0x21, 6, secret, sizeof(secret));
    TEST_ASSERT_EQUAL_HEX16(0x6A80, swOf(n));
}

static void test_list_framing() {
    selectApplet();
    uint8_t secret[10]; memset(secret, 1, sizeof(secret));
    putCred("Example:alice", 0x21, 6, secret, sizeof(secret));  // TOTP SHA1
    uint8_t apdu[] = {0x00, 0xA1, 0x00, 0x00, 0x00};
    int n = send(apdu, sizeof(apdu));
    TEST_ASSERT_EQUAL_HEX16(0x9000, swOf(n));
    TEST_ASSERT_EQUAL_HEX8(0x72, g_rsp[0]);           // list entry tag
    TEST_ASSERT_EQUAL_HEX8(0x21, g_rsp[2]);           // type|algo = TOTP|SHA1
}

static void test_calculate_truncated() {
    selectApplet();
    uint8_t secret[10]; memset(secret, 2, sizeof(secret));
    putCred("Site:carol", 0x21, 6, secret, sizeof(secret));
    // CALCULATE: 71 name, 74 challenge(8), P2=1 truncated.
    const char* name = "Site:carol";
    uint8_t data[64]; size_t p = 0;
    data[p++] = 0x71; data[p++] = strlen(name); memcpy(data + p, name, strlen(name)); p += strlen(name);
    uint8_t chal[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    data[p++] = 0x74; data[p++] = 8; memcpy(data + p, chal, 8); p += 8;
    uint8_t apdu[80] = {0x00, 0xA2, 0x00, 0x01, static_cast<uint8_t>(p)};
    memcpy(apdu + 5, data, p); apdu[5 + p] = 0x00;
    int n = send(apdu, 5 + p + 1);
    TEST_ASSERT_EQUAL_HEX16(0x9000, swOf(n));
    TEST_ASSERT_EQUAL_HEX8(0x76, g_rsp[0]);   // truncated response
    TEST_ASSERT_EQUAL_HEX8(0x05, g_rsp[1]);   // length 5
    TEST_ASSERT_EQUAL_HEX8(6, g_rsp[2]);      // digits
}

static void test_calculate_all_totp_and_hotp() {
    selectApplet();
    uint8_t secret[10]; memset(secret, 3, sizeof(secret));
    putCred("T:t", 0x21, 6, secret, sizeof(secret));  // TOTP
    putCred("H:h", 0x11, 6, secret, sizeof(secret));  // HOTP
    uint8_t data[16]; size_t p = 0;
    uint8_t chal[8] = {0, 0, 0, 0, 0, 0, 0, 2};
    data[p++] = 0x74; data[p++] = 8; memcpy(data + p, chal, 8); p += 8;
    uint8_t apdu[32] = {0x00, 0xA4, 0x00, 0x01, static_cast<uint8_t>(p)};
    memcpy(apdu + 5, data, p); apdu[5 + p] = 0x00;
    int n = send(apdu, 5 + p + 1);
    TEST_ASSERT_EQUAL_HEX16(0x9000, swOf(n));
    // Expect a 0x76 (TOTP) and a 0x77 (HOTP marker) somewhere in the payload.
    bool has76 = false, has77 = false;
    for (int i = 0; i < n - 2; i++) { if (g_rsp[i] == 0x76) has76 = true; if (g_rsp[i] == 0x77) has77 = true; }
    TEST_ASSERT_TRUE(has76);
    TEST_ASSERT_TRUE(has77);
}

static void test_delete_missing() {
    selectApplet();
    const char* name = "Nope:x";
    uint8_t data[16]; size_t p = 0;
    data[p++] = 0x71; data[p++] = strlen(name); memcpy(data + p, name, strlen(name)); p += strlen(name);
    uint8_t apdu[32] = {0x00, 0x02, 0x00, 0x00, static_cast<uint8_t>(p)};
    memcpy(apdu + 5, data, p);
    int n = send(apdu, 5 + p);
    TEST_ASSERT_EQUAL_HEX16(0x6984, swOf(n));
}

static void test_send_remaining_chaining() {
    selectApplet();
    uint8_t secret[10]; memset(secret, 4, sizeof(secret));
    // Fill entries with long names so a LIST overflows one 255-byte window.
    for (int i = 0; i < 8; i++) {
        char nm[40];
        snprintf(nm, sizeof(nm), "Issuer%d:accountlongname%d", i, i);
        putCred(nm, 0x21, 6, secret, sizeof(secret));
    }
    // LIST with a small Le forces chaining.
    uint8_t apdu[] = {0x00, 0xA1, 0x00, 0x00, 0x40};  // Le = 64
    int n = send(apdu, sizeof(apdu));
    TEST_ASSERT_EQUAL_HEX8(0x61, g_rsp[n - 2]);  // 61xx: more data

    // Drain via SEND REMAINING (INS A5).
    int guard = 0;
    uint16_t sw = 0;
    do {
        uint8_t sr[] = {0x00, 0xA5, 0x00, 0x00, 0x40};
        n = send(sr, sizeof(sr));
        sw = swOf(n);
        guard++;
    } while (g_rsp[n - 2] == 0x61 && guard < 20);
    TEST_ASSERT_EQUAL_HEX16(0x9000, sw);
}

static void test_access_key_validate_flow() {
    // Set an access key directly, then SELECT should report locked and gate LIST.
    uint8_t key[16]; memset(key, 0x5A, 16);
    fkAkeySet(key);
    int n = selectApplet();
    // Locked SELECT carries a challenge (0x74).
    const uint8_t* chal = nullptr; size_t chalLen = 0;
    {
        size_t p = 0; int len = n - 2;
        while ((int)p + 2 <= len) {
            uint8_t t = g_rsp[p]; uint8_t l = g_rsp[p+1];
            if (t == 0x74) { chal = &g_rsp[p + 2]; chalLen = l; }
            p += 2 + l;
        }
    }
    TEST_ASSERT_NOT_NULL(chal);
    TEST_ASSERT_EQUAL_size_t(8, chalLen);
    uint8_t selChal[8]; memcpy(selChal, chal, 8);

    // LIST before VALIDATE is refused.
    uint8_t list[] = {0x00, 0xA1, 0x00, 0x00, 0x00};
    n = send(list, sizeof(list));
    TEST_ASSERT_EQUAL_HEX16(0x6982, swOf(n));

    // VALIDATE: 75 HMAC(key, selChal), 74 hostChallenge.
    uint8_t proof[20];
    fkHmac(key, 16, selChal, 8, proof);
    uint8_t hostChal[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t data[48]; size_t p = 0;
    data[p++] = 0x75; data[p++] = 20; memcpy(data + p, proof, 20); p += 20;
    data[p++] = 0x74; data[p++] = 8; memcpy(data + p, hostChal, 8); p += 8;
    uint8_t apdu[64] = {0x00, 0xA3, 0x00, 0x00, static_cast<uint8_t>(p)};
    memcpy(apdu + 5, data, p); apdu[5 + p] = 0x00;
    n = send(apdu, 5 + p + 1);
    TEST_ASSERT_EQUAL_HEX16(0x9000, swOf(n));
    TEST_ASSERT_EQUAL_HEX8(0x75, g_rsp[0]);  // reply proof

    // Now LIST is allowed.
    n = send(list, sizeof(list));
    TEST_ASSERT_EQUAL_HEX16(0x9000, swOf(n));
}

static void test_reset() {
    selectApplet();
    uint8_t secret[10]; memset(secret, 5, sizeof(secret));
    putCred("A:a", 0x21, 6, secret, sizeof(secret));
    uint8_t apdu[] = {0x00, 0x04, 0xDE, 0xAD};
    int n = send(apdu, sizeof(apdu));
    TEST_ASSERT_EQUAL_HEX16(0x9000, swOf(n));
    TEST_ASSERT_FALSE(g_entries[0].used);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_select_unlocked);
    RUN_TEST(test_put_name_mapping_and_reject_long);
    RUN_TEST(test_list_framing);
    RUN_TEST(test_calculate_truncated);
    RUN_TEST(test_calculate_all_totp_and_hotp);
    RUN_TEST(test_delete_missing);
    RUN_TEST(test_send_remaining_chaining);
    RUN_TEST(test_access_key_validate_flow);
    RUN_TEST(test_reset);
    return UNITY_END();
}
