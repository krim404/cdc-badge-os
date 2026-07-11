/*
 * YKOATH protocol state machine. See OathApplet.h for the backend seam.
 * Reference: https://developers.yubico.com/OATH/YKOATH_Protocol.html
 */

#include "OathApplet.h"
#include "cdc_scard/apdu.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__has_include)
#  if __has_include(<esp_attr.h>)
#    include <esp_attr.h>
#  endif
#endif
#ifndef EXT_RAM_BSS_ATTR
#  define EXT_RAM_BSS_ATTR
#endif

namespace {

// --- YKOATH constants ------------------------------------------------------
constexpr uint8_t INS_PUT = 0x01;
constexpr uint8_t INS_DELETE = 0x02;
constexpr uint8_t INS_SET_CODE = 0x03;
constexpr uint8_t INS_RESET = 0x04;
constexpr uint8_t INS_LIST = 0xA1;
constexpr uint8_t INS_CALCULATE = 0xA2;
constexpr uint8_t INS_VALIDATE = 0xA3;
constexpr uint8_t INS_CALCULATE_ALL = 0xA4;
constexpr uint8_t INS_SEND_REMAINING = 0xA5;
// ISO SELECT is INS 0xA4 (same byte as CALCULATE ALL); disambiguated by P1=0x04.
constexpr uint8_t INS_ISO_SELECT = 0xA4;

constexpr uint8_t TAG_NAME = 0x71;
constexpr uint8_t TAG_NAME_LIST = 0x72;
constexpr uint8_t TAG_KEY = 0x73;
constexpr uint8_t TAG_CHALLENGE = 0x74;
constexpr uint8_t TAG_FULL_RESP = 0x75;
constexpr uint8_t TAG_TRUNC_RESP = 0x76;
constexpr uint8_t TAG_HOTP = 0x77;
constexpr uint8_t TAG_PROPERTY = 0x78;
constexpr uint8_t TAG_VERSION = 0x79;
constexpr uint8_t TAG_IMF = 0x7A;
constexpr uint8_t TAG_ALGO = 0x7B;
constexpr uint8_t TAG_TOUCH = 0x7C;

constexpr uint8_t TYPE_HOTP = 0x10;
constexpr uint8_t TYPE_TOTP = 0x20;
constexpr uint8_t TYPE_MASK = 0xF0;
constexpr uint8_t ALGO_MASK = 0x0F;

constexpr uint8_t PROP_TOUCH = 0x02;

constexpr uint16_t SW_OK = 0x9000;
constexpr uint16_t SW_MORE = 0x6100;
constexpr uint16_t SW_NO_SPACE = 0x6A84;
constexpr uint16_t SW_AUTH_REQUIRED = 0x6982;
constexpr uint16_t SW_WRONG_SYNTAX = 0x6A80;
constexpr uint16_t SW_NO_SUCH_OBJECT = 0x6984;
constexpr uint16_t SW_NOT_SATISFIED = 0x6985;
constexpr uint16_t SW_WRONG_LENGTH = 0x6700;
constexpr uint16_t SW_INS_NOT_SUPPORTED = 0x6D00;

// OathType / OathAlgorithm mirror mod_2fa/OathStore.h.
constexpr uint8_t OTYPE_TOTP = 0;
constexpr uint8_t OTYPE_HOTP = 1;
constexpr uint8_t OTYPE_CR = 2;
constexpr uint8_t OFLAG_TOUCH = 0x01;

const oath_backend_t* g_be = nullptr;

// Session state.
bool g_validated = false;
uint8_t g_sel_challenge[8];
bool g_challenge_valid = false;

EXT_RAM_BSS_ATTR uint8_t g_build[4096];
EXT_RAM_BSS_ATTR uint8_t g_resp[4096];
size_t g_resp_pos = 0;
size_t g_resp_remaining = 0;

int sw(uint8_t* resp, uint16_t s) {
    resp[0] = static_cast<uint8_t>(s >> 8);
    resp[1] = static_cast<uint8_t>(s);
    return 2;
}

// TLV write with 1-byte length (YKOATH lengths never exceed one byte per item).
bool put_tlv(uint8_t* buf, size_t cap, size_t* pos, uint8_t tag,
             const uint8_t* val, size_t len) {
    if (*pos + 2 + len > cap || len > 0xFF) return false;
    buf[(*pos)++] = tag;
    buf[(*pos)++] = static_cast<uint8_t>(len);
    if (len && val) memcpy(buf + *pos, val, len);
    *pos += len;
    return true;
}

bool find_tlv(const uint8_t* buf, size_t len, uint8_t tag,
              const uint8_t** valOut, size_t* lenOut) {
    size_t p = 0;
    while (p + 2 <= len) {
        uint8_t t = buf[p++];
        size_t l = buf[p++];
        if (p + l > len) return false;
        if (t == tag) {
            if (valOut) *valOut = buf + p;
            if (lenOut) *lenOut = l;
            return true;
        }
        p += l;
    }
    return false;
}

int emit(const uint8_t* payload, size_t len, uint32_t le, uint8_t* resp, size_t resp_max) {
    size_t window = (le == 0) ? 256 : le;
    if (window > resp_max - 2) window = resp_max - 2;
    if (len <= window) {
        memcpy(resp, payload, len);
        return sw(resp + len, SW_OK) + static_cast<int>(len);
    }
    memcpy(resp, payload, window);
    size_t remainder = len - window;
    if (remainder > sizeof(g_resp)) remainder = sizeof(g_resp);
    memcpy(g_resp, payload + window, remainder);
    g_resp_pos = 0;
    g_resp_remaining = remainder;
    uint8_t sw2 = (remainder > 0xFF) ? 0x00 : static_cast<uint8_t>(remainder);
    return sw(resp + window, static_cast<uint16_t>(SW_MORE | sw2)) + static_cast<int>(window);
}

int cmd_send_remaining(const apdu_t* a, uint8_t* resp, size_t resp_max) {
    if (g_resp_remaining == 0) return sw(resp, SW_NOT_SATISFIED);
    size_t want = (a->le == 0) ? 256 : a->le;
    if (want > g_resp_remaining) want = g_resp_remaining;
    if (want + 2 > resp_max) want = resp_max - 2;
    memcpy(resp, g_resp + g_resp_pos, want);
    g_resp_pos += want;
    g_resp_remaining -= want;
    uint16_t s = SW_OK;
    if (g_resp_remaining > 0) {
        uint8_t sw2 = (g_resp_remaining > 0xFF) ? 0x00 : static_cast<uint8_t>(g_resp_remaining);
        s = static_cast<uint16_t>(SW_MORE | sw2);
    } else {
        g_resp_pos = 0;
    }
    return sw(resp + want, s) + static_cast<int>(want);
}

// --- name mapping ----------------------------------------------------------
// Builds the YKOATH credential name "[period/]issuer:account" from stored
// fields (period prefix only when it differs from the default 30 seconds).
size_t build_yk_name(const oath_meta_t* m, char* out, size_t cap) {
    size_t p = 0;
    if (m->type == OTYPE_TOTP && m->period != 0 && m->period != 30) {
        int n = snprintf(out + p, cap - p, "%u/", static_cast<unsigned>(m->period));
        if (n > 0) p += static_cast<size_t>(n);
    }
    if (m->issuer[0]) {
        int n = snprintf(out + p, cap - p, "%s:", m->issuer);
        if (n > 0) p += static_cast<size_t>(n);
    }
    int n = snprintf(out + p, cap - p, "%s", m->name);
    if (n > 0) p += static_cast<size_t>(n);
    return p;
}

// Parses a YKOATH name into period / issuer / account. Returns false when the
// account or issuer exceed the store's field sizes.
bool parse_yk_name(const uint8_t* name, size_t len, uint32_t* period,
                   char* issuer, size_t issuerCap, char* account, size_t accountCap) {
    char buf[128];
    if (len >= sizeof(buf)) return false;
    memcpy(buf, name, len);
    buf[len] = '\0';

    char* cur = buf;
    *period = 30;
    // Optional "<digits>/" period prefix.
    char* slash = strchr(cur, '/');
    if (slash) {
        bool allDigits = slash > cur;
        for (char* c = cur; c < slash; c++) {
            if (*c < '0' || *c > '9') { allDigits = false; break; }
        }
        if (allDigits) {
            *slash = '\0';
            long v = strtol(cur, nullptr, 10);
            if (v > 0) *period = static_cast<uint32_t>(v);
            cur = slash + 1;
        }
    }

    issuer[0] = '\0';
    char* colon = strchr(cur, ':');
    if (colon) {
        size_t ilen = static_cast<size_t>(colon - cur);
        if (ilen >= issuerCap) return false;
        memcpy(issuer, cur, ilen);
        issuer[ilen] = '\0';
        cur = colon + 1;
    }
    size_t alen = strlen(cur);
    if (alen == 0 || alen >= accountCap) return false;
    memcpy(account, cur, alen + 1);
    return true;
}

uint8_t type_algo_byte(const oath_meta_t* m) {
    uint8_t hi = (m->type == OTYPE_HOTP) ? TYPE_HOTP : TYPE_TOTP;
    uint8_t lo = static_cast<uint8_t>((m->algorithm & 0x0F) + 1);  // SHA1=1..SHA512=3
    return hi | lo;
}

// Finds the store slot whose reconstructed YKOATH name matches the request.
bool find_by_yk_name(const uint8_t* name, size_t len, uint16_t* slotOut) {
    char target[128];
    if (len >= sizeof(target)) return false;
    memcpy(target, name, len);
    target[len] = '\0';

    uint16_t cap = g_be->capacity();
    for (uint16_t s = 0; s < cap; s++) {
        oath_meta_t m;
        if (!g_be->read(s, &m) || !m.used || m.type == OTYPE_CR) continue;
        char yk[128];
        size_t n = build_yk_name(&m, yk, sizeof(yk));
        if (n == len && memcmp(yk, target, len) == 0) {
            *slotOut = s;
            return true;
        }
    }
    return false;
}

// --- commands --------------------------------------------------------------
int cmd_select(uint8_t* resp, size_t resp_max, uint32_t le) {
    uint8_t devid[8];
    g_be->devId(devid);
    uint8_t akey[16];
    bool locked = g_be->akeyGet(akey);
    g_validated = !locked;

    size_t p = 0;
    static const uint8_t kVersion[3] = {0x05, 0x04, 0x03};
    put_tlv(g_build, sizeof(g_build), &p, TAG_VERSION, kVersion, 3);
    put_tlv(g_build, sizeof(g_build), &p, TAG_NAME, devid, 8);
    if (locked) {
        g_be->rng(g_sel_challenge, 8);
        g_challenge_valid = true;
        put_tlv(g_build, sizeof(g_build), &p, TAG_CHALLENGE, g_sel_challenge, 8);
        uint8_t algo = 0x21;  // HMAC-SHA1 marker
        put_tlv(g_build, sizeof(g_build), &p, TAG_ALGO, &algo, 1);
    }
    return emit(g_build, p, le, resp, resp_max);
}

int cmd_put(const uint8_t* data, size_t lc, uint8_t* resp) {
    const uint8_t* name = nullptr; size_t nameLen = 0;
    const uint8_t* key = nullptr; size_t keyLen = 0;
    if (!find_tlv(data, lc, TAG_NAME, &name, &nameLen) ||
        !find_tlv(data, lc, TAG_KEY, &key, &keyLen) || keyLen < 3)
        return sw(resp, SW_WRONG_SYNTAX);

    uint8_t typeAlgo = key[0];
    uint8_t digits = key[1];
    const uint8_t* secret = key + 2;
    size_t secretLen = keyLen - 2;
    if (secretLen > 64) return sw(resp, SW_WRONG_SYNTAX);

    uint8_t type = ((typeAlgo & TYPE_MASK) == TYPE_HOTP) ? OTYPE_HOTP : OTYPE_TOTP;
    uint8_t algoNib = typeAlgo & ALGO_MASK;
    if (algoNib < 1 || algoNib > 3) return sw(resp, SW_WRONG_SYNTAX);
    uint8_t algorithm = static_cast<uint8_t>(algoNib - 1);

    uint32_t period = 30;
    char issuer[33]; char account[17];
    if (!parse_yk_name(name, nameLen, &period, issuer, sizeof(issuer), account, sizeof(account)))
        return sw(resp, SW_WRONG_SYNTAX);

    uint8_t flags = 0;
    const uint8_t* prop = nullptr; size_t propLen = 0;
    if (find_tlv(data, lc, TAG_PROPERTY, &prop, &propLen) && propLen >= 1 &&
        (prop[0] & PROP_TOUCH)) {
        flags |= OFLAG_TOUCH;
    }

    uint64_t counter = 0;
    const uint8_t* imf = nullptr; size_t imfLen = 0;
    if (find_tlv(data, lc, TAG_IMF, &imf, &imfLen) && imfLen == 4) {
        counter = (static_cast<uint64_t>(imf[0]) << 24) | (imf[1] << 16) |
                  (imf[2] << 8) | imf[3];
    }

    if (!g_be->addRaw(type, account, issuer, secret, static_cast<uint8_t>(secretLen),
                      digits, period, algorithm, counter, flags))
        return sw(resp, SW_NO_SPACE);
    return sw(resp, SW_OK);
}

int cmd_delete(const uint8_t* data, size_t lc, uint8_t* resp) {
    const uint8_t* name = nullptr; size_t nameLen = 0;
    if (!find_tlv(data, lc, TAG_NAME, &name, &nameLen)) return sw(resp, SW_WRONG_SYNTAX);
    uint16_t slot = 0;
    if (!find_by_yk_name(name, nameLen, &slot)) return sw(resp, SW_NO_SUCH_OBJECT);
    if (!g_be->remove(slot)) return sw(resp, SW_NO_SUCH_OBJECT);
    return sw(resp, SW_OK);
}

int cmd_list(uint8_t* resp, size_t resp_max, uint32_t le) {
    size_t p = 0;
    uint16_t cap = g_be->capacity();
    for (uint16_t s = 0; s < cap; s++) {
        oath_meta_t m;
        if (!g_be->read(s, &m) || !m.used || m.type == OTYPE_CR) continue;
        char yk[128];
        size_t n = build_yk_name(&m, yk, sizeof(yk));
        uint8_t entry[1 + 128];
        entry[0] = type_algo_byte(&m);
        memcpy(entry + 1, yk, n);
        put_tlv(g_build, sizeof(g_build), &p, TAG_NAME_LIST, entry, 1 + n);
    }
    return emit(g_build, p, le, resp, resp_max);
}

int cmd_calculate(const apdu_t* a, const uint8_t* data, size_t lc,
                  uint8_t* resp, size_t resp_max) {
    const uint8_t* name = nullptr; size_t nameLen = 0;
    const uint8_t* chal = nullptr; size_t chalLen = 0;
    if (!find_tlv(data, lc, TAG_NAME, &name, &nameLen) ||
        !find_tlv(data, lc, TAG_CHALLENGE, &chal, &chalLen) || chalLen != 8)
        return sw(resp, SW_WRONG_SYNTAX);
    uint16_t slot = 0;
    if (!find_by_yk_name(name, nameLen, &slot)) return sw(resp, SW_NO_SUCH_OBJECT);

    uint8_t trunc[4]; uint8_t digits = 6;
    if (!g_be->calculate(slot, chal, trunc, &digits)) return sw(resp, SW_NO_SUCH_OBJECT);

    uint8_t val[5] = {digits, trunc[0], trunc[1], trunc[2], trunc[3]};
    size_t p = 0;
    put_tlv(g_build, sizeof(g_build), &p, TAG_TRUNC_RESP, val, 5);
    return emit(g_build, p, a->le, resp, resp_max);
}

int cmd_calculate_all(const uint8_t* data, size_t lc, uint8_t* resp, size_t resp_max, uint32_t le) {
    const uint8_t* chal = nullptr; size_t chalLen = 0;
    if (!find_tlv(data, lc, TAG_CHALLENGE, &chal, &chalLen) || chalLen != 8)
        return sw(resp, SW_WRONG_SYNTAX);

    size_t p = 0;
    uint16_t cap = g_be->capacity();
    for (uint16_t s = 0; s < cap; s++) {
        oath_meta_t m;
        if (!g_be->read(s, &m) || !m.used || m.type == OTYPE_CR) continue;
        char yk[128];
        size_t n = build_yk_name(&m, yk, sizeof(yk));
        put_tlv(g_build, sizeof(g_build), &p, TAG_NAME, reinterpret_cast<const uint8_t*>(yk), n);

        if (m.type == OTYPE_HOTP) {
            put_tlv(g_build, sizeof(g_build), &p, TAG_HOTP, nullptr, 0);
        } else if (m.flags & OFLAG_TOUCH) {
            put_tlv(g_build, sizeof(g_build), &p, TAG_TOUCH, nullptr, 0);
        } else {
            uint8_t trunc[4]; uint8_t digits = 6;
            if (g_be->calculate(s, chal, trunc, &digits)) {
                uint8_t val[5] = {digits, trunc[0], trunc[1], trunc[2], trunc[3]};
                put_tlv(g_build, sizeof(g_build), &p, TAG_TRUNC_RESP, val, 5);
            } else {
                put_tlv(g_build, sizeof(g_build), &p, TAG_TOUCH, nullptr, 0);
            }
        }
    }
    return emit(g_build, p, le, resp, resp_max);
}

int cmd_set_code(const uint8_t* data, size_t lc, uint8_t* resp) {
    const uint8_t* key = nullptr; size_t keyLen = 0;
    if (!find_tlv(data, lc, TAG_KEY, &key, &keyLen)) return sw(resp, SW_WRONG_SYNTAX);
    if (keyLen == 0) {  // clear the access key
        g_be->akeyClear();
        g_validated = true;
        return sw(resp, SW_OK);
    }
    // key = [algo][16-byte key]; verify the host's proof over its challenge.
    if (keyLen != 17) return sw(resp, SW_WRONG_SYNTAX);
    const uint8_t* chal = nullptr; size_t chalLen = 0;
    const uint8_t* resp75 = nullptr; size_t respLen = 0;
    if (!find_tlv(data, lc, TAG_CHALLENGE, &chal, &chalLen) || chalLen != 8 ||
        !find_tlv(data, lc, TAG_FULL_RESP, &resp75, &respLen) || respLen != 20)
        return sw(resp, SW_WRONG_SYNTAX);
    uint8_t expect[20];
    if (!g_be->hmacSha1(key + 1, 16, chal, 8, expect)) return sw(resp, SW_NOT_SATISFIED);
    if (memcmp(expect, resp75, 20) != 0) return sw(resp, SW_NOT_SATISFIED);
    if (!g_be->akeySet(key + 1)) return sw(resp, SW_NOT_SATISFIED);
    return sw(resp, SW_OK);
}

int cmd_validate(const uint8_t* data, size_t lc, uint8_t* resp, size_t resp_max, uint32_t le) {
    uint8_t akey[16];
    if (!g_be->akeyGet(akey)) return sw(resp, SW_OK);  // no key: nothing to validate
    if (!g_challenge_valid) return sw(resp, SW_NOT_SATISFIED);

    const uint8_t* resp75 = nullptr; size_t respLen = 0;
    const uint8_t* hostChal = nullptr; size_t hostLen = 0;
    if (!find_tlv(data, lc, TAG_FULL_RESP, &resp75, &respLen) || respLen != 20 ||
        !find_tlv(data, lc, TAG_CHALLENGE, &hostChal, &hostLen) || hostLen != 8)
        return sw(resp, SW_WRONG_SYNTAX);

    uint8_t expect[20];
    if (!g_be->hmacSha1(akey, 16, g_sel_challenge, 8, expect)) return sw(resp, SW_NOT_SATISFIED);
    if (memcmp(expect, resp75, 20) != 0) return sw(resp, SW_WRONG_SYNTAX);

    uint8_t reply[20];
    if (!g_be->hmacSha1(akey, 16, hostChal, 8, reply)) return sw(resp, SW_NOT_SATISFIED);
    g_validated = true;
    g_challenge_valid = false;

    size_t p = 0;
    put_tlv(g_build, sizeof(g_build), &p, TAG_FULL_RESP, reply, 20);
    return emit(g_build, p, le, resp, resp_max);
}

int cmd_reset(const apdu_t* a, uint8_t* resp) {
    if (a->p1 != 0xDE || a->p2 != 0xAD) return sw(resp, SW_WRONG_SYNTAX);
    g_be->wipeAll();
    g_be->akeyClear();
    g_validated = true;
    g_challenge_valid = false;
    return sw(resp, SW_OK);
}

int process_apdu(const uint8_t* cmd, size_t cmd_len, uint8_t* resp, size_t resp_max) {
    if (!g_be || resp_max < 2) return sw(resp, SW_INS_NOT_SUPPORTED);
    apdu_t a;
    if (!apdu_parse(cmd, cmd_len, &a)) return sw(resp, SW_WRONG_LENGTH);

    // SELECT-by-AID (routed here by the dispatcher) uses ISO INS 0xA4 with
    // P1=0x04; CALCULATE ALL is also 0xA4 but with P1=0x00.
    if (a.ins == INS_ISO_SELECT && a.p1 == 0x04) {
        return cmd_select(resp, resp_max, a.le);
    }

    switch (a.ins) {
        case INS_SEND_REMAINING:
            return cmd_send_remaining(&a, resp, resp_max);
        case INS_RESET:
            return cmd_reset(&a, resp);
        case INS_VALIDATE:
            return cmd_validate(a.data, a.lc, resp, resp_max, a.le);
        default:
            break;
    }

    // Everything else needs a validated session when an access key is set.
    if (!g_validated) return sw(resp, SW_AUTH_REQUIRED);

    switch (a.ins) {
        case INS_PUT:            return cmd_put(a.data, a.lc, resp);
        case INS_DELETE:         return cmd_delete(a.data, a.lc, resp);
        case INS_SET_CODE:       return cmd_set_code(a.data, a.lc, resp);
        case INS_LIST:           return cmd_list(resp, resp_max, a.le);
        case INS_CALCULATE:      return cmd_calculate(&a, a.data, a.lc, resp, resp_max);
        case INS_CALCULATE_ALL:  return cmd_calculate_all(a.data, a.lc, resp, resp_max, a.le);
        default:                 return sw(resp, SW_INS_NOT_SUPPORTED);
    }
}

void deselect() {
    g_validated = false;
    g_challenge_valid = false;
    memset(g_sel_challenge, 0, sizeof(g_sel_challenge));
    g_resp_pos = 0;
    g_resp_remaining = 0;
}

} // namespace

extern "C" void oath_set_backend(const oath_backend_t* backend) {
    g_be = backend;
}

extern "C" const scard_applet_t* oath_applet(void) {
    static const uint8_t kAid[7] = {0xA0, 0x00, 0x00, 0x05, 0x27, 0x21, 0x01};
    static const scard_applet_t applet = {
        "oath", kAid, sizeof(kAid), process_apdu, deselect,
    };
    return &applet;
}
