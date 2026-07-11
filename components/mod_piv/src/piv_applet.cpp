/*
 * PIV applet APDU state machine (NIST SP 800-73-4).
 *
 * Hardware- and crypto-free: every side effect goes through piv_backend_t so
 * the whole command surface is host-testable. Response chaining, command
 * chaining and PIN/management session gating are handled here; key material,
 * AES, ECDSA, ECDH, PIN checks and persistence live behind the backend.
 */

#include "piv_defs.h"
#include "piv_tlv.h"
#include "piv_objects.h"

#include "cdc_scard/apdu.h"

#include <string.h>

#if defined(__has_include)
#  if __has_include(<esp_attr.h>)
#    include <esp_attr.h>
#  endif
#endif
#ifndef EXT_RAM_BSS_ATTR
#  define EXT_RAM_BSS_ATTR
#endif

namespace cdc::mod_piv {
namespace {

const piv_backend_t* g_backend = nullptr;

// --- Session state (wiped on deselect / card reset) ------------------------
bool g_pin_verified = false;
bool g_mgmt_authenticated = false;
bool g_witness_valid = false;
uint8_t g_witness[PIV_MGMT_BLOCK];

// Command-chaining accumulator (largest input is a ~2 KB certificate PUT DATA).
EXT_RAM_BSS_ATTR uint8_t g_chain[2600];
size_t g_chain_len = 0;
uint8_t g_chain_ins = 0, g_chain_p1 = 0, g_chain_p2 = 0;

// Full-response scratch + GET RESPONSE drain buffer.
EXT_RAM_BSS_ATTR uint8_t g_build[2600];
EXT_RAM_BSS_ATTR uint8_t g_resp_buf[2600];
size_t g_resp_pos = 0;
size_t g_resp_remaining = 0;

void resetChain() {
    g_chain_len = 0;
    g_chain_ins = g_chain_p1 = g_chain_p2 = 0;
}

int sw(uint8_t* resp, uint16_t s) {
    resp[0] = static_cast<uint8_t>(s >> 8);
    resp[1] = static_cast<uint8_t>(s);
    return 2;
}

// Emits a payload with ISO 7816 response chaining: hands out the head that
// fits the host's Le window (and the buffer), stashing the rest for
// GET RESPONSE (61xx).
int emit(const uint8_t* payload, size_t len, uint32_t le, uint8_t* resp, size_t resp_max) {
    size_t window = (le == 0) ? 256 : le;
    if (window > resp_max - 2) window = resp_max - 2;
    if (len <= window) {
        memcpy(resp, payload, len);
        return sw(resp + len, PIV_SW_OK) + static_cast<int>(len);
    }
    memcpy(resp, payload, window);
    size_t remainder = len - window;
    if (remainder > sizeof(g_resp_buf)) remainder = sizeof(g_resp_buf);
    memcpy(g_resp_buf, payload + window, remainder);
    g_resp_pos = 0;
    g_resp_remaining = remainder;
    uint8_t sw2 = (remainder > 0xFF) ? 0x00 : static_cast<uint8_t>(remainder);
    return sw(resp + window, static_cast<uint16_t>(PIV_SW_MORE_DATA | sw2)) +
           static_cast<int>(window);
}

int cmdGetResponse(const apdu_t* a, uint8_t* resp, size_t resp_max) {
    if (a->p1 != 0x00 || a->p2 != 0x00) return sw(resp, PIV_SW_INCORRECT_P1P2);
    if (g_resp_remaining == 0) return sw(resp, PIV_SW_SECURITY_STATUS);
    size_t want = (a->le == 0) ? 256 : a->le;
    if (want > g_resp_remaining) want = g_resp_remaining;
    if (want + 2 > resp_max) want = resp_max - 2;
    memcpy(resp, g_resp_buf + g_resp_pos, want);
    g_resp_pos += want;
    g_resp_remaining -= want;
    uint16_t s = PIV_SW_OK;
    if (g_resp_remaining > 0) {
        uint8_t sw2 = (g_resp_remaining > 0xFF) ? 0x00 : static_cast<uint8_t>(g_resp_remaining);
        s = static_cast<uint16_t>(PIV_SW_MORE_DATA | sw2);
    } else {
        g_resp_pos = 0;
    }
    return sw(resp + want, s) + static_cast<int>(want);
}

// --- SELECT ----------------------------------------------------------------
int cmdSelect(uint8_t* resp, size_t resp_max, uint32_t le) {
    g_pin_verified = false;
    g_mgmt_authenticated = false;
    g_witness_valid = false;
    size_t n = buildApt(g_build, sizeof(g_build));
    if (n == 0) return sw(resp, PIV_SW_DATA_INVALID);
    return emit(g_build, n, le, resp, resp_max);
}

// --- GET DATA --------------------------------------------------------------
int cmdGetData(const uint8_t* data, size_t lc, uint8_t* resp, size_t resp_max, uint32_t le) {
    const uint8_t* tagVal = nullptr;
    size_t tagLen = 0;
    if (!tlvFind(data, lc, PIV_TAG_TAG_LIST, &tagVal, &tagLen) || tagLen == 0 || tagLen > 3)
        return sw(resp, PIV_SW_DATA_INVALID);
    uint32_t obj = 0;
    for (size_t i = 0; i < tagLen; i++) obj = (obj << 8) | tagVal[i];

    if (obj == PIV_OBJ_DISCOVERY) {
        size_t n = buildDiscovery(g_build, sizeof(g_build));
        if (n == 0) return sw(resp, PIV_SW_FILE_NOT_FOUND);
        return emit(g_build, n, le, resp, resp_max);
    }

    piv_state_t st;
    if (!g_backend->state_load(&st)) return sw(resp, PIV_SW_FILE_NOT_FOUND);

    // Body inside tag 0x53.
    uint8_t body[2200];
    size_t bodyLen = 0;
    if (obj == PIV_OBJ_CHUID) {
        bodyLen = buildChuid(st.guid, body, sizeof(body));
    } else if (obj == PIV_OBJ_CCC) {
        bodyLen = buildCcc(st.ccc_id, body, sizeof(body));
    } else if (certObjectToKeyRef(obj) != 0) {
        int n = g_backend->object_load(obj, body, sizeof(body));
        if (n <= 0) return sw(resp, PIV_SW_FILE_NOT_FOUND);
        bodyLen = static_cast<size_t>(n);
    } else {
        return sw(resp, PIV_SW_FILE_NOT_FOUND);
    }
    if (bodyLen == 0) return sw(resp, PIV_SW_FILE_NOT_FOUND);

    size_t p = 0;
    if (!tlvWrite(g_build, sizeof(g_build), &p, PIV_TAG_DATA_OBJECT, body, bodyLen))
        return sw(resp, PIV_SW_FILE_NOT_FOUND);
    return emit(g_build, p, le, resp, resp_max);
}

// --- VERIFY ----------------------------------------------------------------
int cmdVerify(const apdu_t* a, const uint8_t* data, size_t lc, uint8_t* resp) {
    if (a->p2 != PIV_KEY_PIN) return sw(resp, PIV_SW_REF_NOT_FOUND);

    if (a->p1 == 0xFF) {  // logout / reset security status
        g_pin_verified = false;
        return sw(resp, PIV_SW_OK);
    }
    if (lc == 0) {  // retry-counter query
        if (g_pin_verified) return sw(resp, PIV_SW_OK);
        if (g_backend->pin_blocked()) return sw(resp, PIV_SW_AUTH_BLOCKED);
        return sw(resp, static_cast<uint16_t>(PIV_SW_PIN_RETRIES | (g_backend->pin_retries() & 0x0F)));
    }
    if (lc != 8) return sw(resp, PIV_SW_DATA_INVALID);
    if (g_backend->pin_blocked()) return sw(resp, PIV_SW_AUTH_BLOCKED);

    // Strip the 0xFF padding; the remainder must be ASCII digits.
    char pin[9] = {0};
    size_t n = 0;
    for (size_t i = 0; i < 8; i++) {
        if (data[i] == 0xFF) break;
        if (data[i] < '0' || data[i] > '9') return sw(resp, PIV_SW_DATA_INVALID);
        pin[n++] = static_cast<char>(data[i]);
    }
    if (n == 0) return sw(resp, PIV_SW_DATA_INVALID);

    if (g_backend->pin_verify(pin)) {
        g_pin_verified = true;
        return sw(resp, PIV_SW_OK);
    }
    if (g_backend->pin_blocked()) return sw(resp, PIV_SW_AUTH_BLOCKED);
    return sw(resp, static_cast<uint16_t>(PIV_SW_PIN_RETRIES | (g_backend->pin_retries() & 0x0F)));
}

// --- GENERAL AUTHENTICATE --------------------------------------------------
bool pinGateForSign(uint8_t keyRef) {
    switch (keyRef) {
        case PIV_KEY_9E: return true;                 // card auth: no PIN
        case PIV_KEY_9A:
        case PIV_KEY_9C:
        case PIV_KEY_9D: return g_pin_verified;
        default: return false;
    }
}

int mgmtAuth(const uint8_t* inner, size_t innerLen, uint8_t* resp) {
    const uint8_t* wit = nullptr; size_t witLen = 0;
    const uint8_t* chal = nullptr; size_t chalLen = 0;
    bool hasWit = tlvFind(inner, innerLen, PIV_TAG_WITNESS, &wit, &witLen);
    bool hasChal = tlvFind(inner, innerLen, PIV_TAG_CHALLENGE, &chal, &chalLen);

    // Step 1: host requests the encrypted witness (empty tag 80).
    if (hasWit && witLen == 0 && !hasChal) {
        if (!g_backend->rng(g_witness, PIV_MGMT_BLOCK)) return sw(resp, PIV_SW_SECURITY_STATUS);
        uint8_t enc[PIV_MGMT_BLOCK];
        if (!g_backend->mgmt_encrypt(g_witness, enc)) return sw(resp, PIV_SW_SECURITY_STATUS);
        g_witness_valid = true;
        uint8_t tmpl[32];
        size_t p = 0;
        if (!tlvWrite(tmpl, sizeof(tmpl), &p, PIV_TAG_WITNESS, enc, PIV_MGMT_BLOCK))
            return sw(resp, PIV_SW_SECURITY_STATUS);
        size_t op = 0;
        if (!tlvWrite(resp, 256, &op, PIV_TAG_DYN_AUTH, tmpl, p))
            return sw(resp, PIV_SW_SECURITY_STATUS);
        return sw(resp + op, PIV_SW_OK) + static_cast<int>(op);
    }

    // Step 2: host returns the decrypted witness plus its own challenge.
    if (hasWit && witLen == PIV_MGMT_BLOCK && hasChal && chalLen == PIV_MGMT_BLOCK) {
        if (!g_witness_valid || memcmp(wit, g_witness, PIV_MGMT_BLOCK) != 0) {
            g_witness_valid = false;
            return sw(resp, PIV_SW_SECURITY_STATUS);
        }
        uint8_t enc[PIV_MGMT_BLOCK];
        if (!g_backend->mgmt_encrypt(chal, enc)) return sw(resp, PIV_SW_SECURITY_STATUS);
        g_mgmt_authenticated = true;
        g_witness_valid = false;
        uint8_t tmpl[32];
        size_t p = 0;
        if (!tlvWrite(tmpl, sizeof(tmpl), &p, PIV_TAG_RESPONSE, enc, PIV_MGMT_BLOCK))
            return sw(resp, PIV_SW_SECURITY_STATUS);
        size_t op = 0;
        if (!tlvWrite(resp, 256, &op, PIV_TAG_DYN_AUTH, tmpl, p))
            return sw(resp, PIV_SW_SECURITY_STATUS);
        return sw(resp + op, PIV_SW_OK) + static_cast<int>(op);
    }
    return sw(resp, PIV_SW_DATA_INVALID);
}

int cmdGeneralAuth(const apdu_t* a, const uint8_t* data, size_t lc,
                   uint8_t* resp, size_t resp_max, uint32_t le) {
    uint8_t alg = a->p1;
    uint8_t keyRef = a->p2;

    const uint8_t* inner = nullptr; size_t innerLen = 0;
    if (!tlvFind(data, lc, PIV_TAG_DYN_AUTH, &inner, &innerLen))
        return sw(resp, PIV_SW_DATA_INVALID);

    if (keyRef == PIV_KEY_9B) {
        return mgmtAuth(inner, innerLen, resp);
    }
    if (alg != PIV_ALG_ECC_P256) return sw(resp, PIV_SW_DATA_INVALID);

    // ECDH on 9D: peer public point in tag 0x85.
    const uint8_t* point = nullptr; size_t pointLen = 0;
    if (keyRef == PIV_KEY_9D && tlvFind(inner, innerLen, PIV_TAG_EXP, &point, &pointLen)) {
        if (!g_pin_verified) return sw(resp, PIV_SW_SECURITY_STATUS);
        if (pointLen != 65 || point[0] != 0x04) return sw(resp, PIV_SW_DATA_INVALID);
        uint8_t shared[32];
        if (!g_backend->key_ecdh(point, shared)) return sw(resp, PIV_SW_SECURITY_STATUS);
        uint8_t tmpl[40];
        size_t p = 0;
        if (!tlvWrite(tmpl, sizeof(tmpl), &p, PIV_TAG_RESPONSE, shared, 32))
            return sw(resp, PIV_SW_SECURITY_STATUS);
        size_t op = 0;
        if (!tlvWrite(g_build, sizeof(g_build), &op, PIV_TAG_DYN_AUTH, tmpl, p))
            return sw(resp, PIV_SW_SECURITY_STATUS);
        return emit(g_build, op, le, resp, resp_max);
    }

    // Signature: challenge digest in tag 0x81.
    const uint8_t* chal = nullptr; size_t chalLen = 0;
    if (!tlvFind(inner, innerLen, PIV_TAG_CHALLENGE, &chal, &chalLen) || chalLen == 0)
        return sw(resp, PIV_SW_DATA_INVALID);
    if (!pinGateForSign(keyRef)) return sw(resp, PIV_SW_SECURITY_STATUS);
    if (!g_backend->key_present(keyRef)) return sw(resp, PIV_SW_REF_NOT_FOUND);

    // Normalize the host digest to 32 bytes (leftmost bytes, left-zero-padded).
    uint8_t digest[32] = {0};
    if (chalLen >= 32) {
        memcpy(digest, chal, 32);
    } else {
        memcpy(digest + (32 - chalLen), chal, chalLen);
    }
    uint8_t rs[64];
    if (!g_backend->key_sign(keyRef, digest, rs)) return sw(resp, PIV_SW_SECURITY_STATUS);

    // 9C is PIN-Always: force re-VERIFY before the next signature.
    if (keyRef == PIV_KEY_9C) g_pin_verified = false;

    uint8_t der[80];
    size_t derLen = derEncodeEcdsaSig(rs, der, sizeof(der));
    if (derLen == 0) return sw(resp, PIV_SW_SECURITY_STATUS);
    uint8_t tmpl[96];
    size_t p = 0;
    if (!tlvWrite(tmpl, sizeof(tmpl), &p, PIV_TAG_RESPONSE, der, derLen))
        return sw(resp, PIV_SW_SECURITY_STATUS);
    size_t op = 0;
    if (!tlvWrite(g_build, sizeof(g_build), &op, PIV_TAG_DYN_AUTH, tmpl, p))
        return sw(resp, PIV_SW_SECURITY_STATUS);
    return emit(g_build, op, le, resp, resp_max);
}

// --- GENERATE ASYMMETRIC KEY PAIR ------------------------------------------
int cmdGenerate(const apdu_t* a, const uint8_t* data, size_t lc,
                uint8_t* resp, size_t resp_max, uint32_t le) {
    if (!g_mgmt_authenticated) return sw(resp, PIV_SW_SECURITY_STATUS);
    uint8_t keyRef = a->p2;
    if (keyRef != PIV_KEY_9A && keyRef != PIV_KEY_9C &&
        keyRef != PIV_KEY_9D && keyRef != PIV_KEY_9E)
        return sw(resp, PIV_SW_REF_NOT_FOUND);

    const uint8_t* alg = nullptr; size_t algLen = 0;
    if (!tlvFind(data, lc, PIV_TAG_ALG_ID, &alg, &algLen))
        return sw(resp, PIV_SW_DATA_INVALID);
    const uint8_t* algId = nullptr; size_t algIdLen = 0;
    if (!tlvFind(alg, algLen, 0x80, &algId, &algIdLen) || algIdLen != 1 ||
        algId[0] != PIV_ALG_ECC_P256)
        return sw(resp, PIV_SW_DATA_INVALID);

    uint8_t pub[65];
    if (!g_backend->key_generate(keyRef, pub)) return sw(resp, PIV_SW_SECURITY_STATUS);

    uint8_t inner[80];
    size_t ip = 0;
    if (!tlvWrite(inner, sizeof(inner), &ip, PIV_TAG_PUBKEY_POINT, pub, 65))
        return sw(resp, PIV_SW_SECURITY_STATUS);
    size_t op = 0;
    if (!tlvWrite(g_build, sizeof(g_build), &op, PIV_TAG_PUBKEY, inner, ip))
        return sw(resp, PIV_SW_SECURITY_STATUS);
    return emit(g_build, op, le, resp, resp_max);
}

// --- PUT DATA --------------------------------------------------------------
int cmdPutData(const uint8_t* data, size_t lc, uint8_t* resp) {
    if (!g_mgmt_authenticated) return sw(resp, PIV_SW_SECURITY_STATUS);
    const uint8_t* tagVal = nullptr; size_t tagLen = 0;
    if (!tlvFind(data, lc, PIV_TAG_TAG_LIST, &tagVal, &tagLen) || tagLen == 0 || tagLen > 3)
        return sw(resp, PIV_SW_DATA_INVALID);
    uint32_t obj = 0;
    for (size_t i = 0; i < tagLen; i++) obj = (obj << 8) | tagVal[i];

    const uint8_t* body = nullptr; size_t bodyLen = 0;
    if (!tlvFind(data, lc, PIV_TAG_DATA_OBJECT, &body, &bodyLen))
        return sw(resp, PIV_SW_DATA_INVALID);

    if (certObjectToKeyRef(obj) != 0) {
        if (!g_backend->object_store(obj, body, bodyLen)) return sw(resp, PIV_SW_SECURITY_STATUS);
        return sw(resp, PIV_SW_OK);
    }
    if (obj == PIV_OBJ_CHUID || obj == PIV_OBJ_CCC) {
        // GUID / card id are managed on-card; accept the write so ykman's
        // object round-trip succeeds, but keep our generated identifiers.
        return sw(resp, PIV_SW_OK);
    }
    return sw(resp, PIV_SW_DATA_INVALID);
}

// --- Yubico GET METADATA (INS F7) ------------------------------------------
int cmdGetMetadata(const apdu_t* a, uint8_t* resp, size_t resp_max, uint32_t le) {
    piv_state_t st;
    if (!g_backend->state_load(&st)) return sw(resp, PIV_SW_FILE_NOT_FOUND);
    uint8_t keyRef = a->p2;

    size_t p = 0;
    if (keyRef == PIV_KEY_9B) {
        // 01: algorithm, 05: is-default flag.
        uint8_t alg[1] = {st.mgmt_alg};
        tlvWrite(g_build, sizeof(g_build), &p, 0x01, alg, 1);
        uint8_t def[1] = {st.mgmt_is_default};
        tlvWrite(g_build, sizeof(g_build), &p, 0x05, def, 1);
        return emit(g_build, p, le, resp, resp_max);
    }
    if (keyRef == PIV_KEY_9A || keyRef == PIV_KEY_9C ||
        keyRef == PIV_KEY_9D || keyRef == PIV_KEY_9E) {
        if (!g_backend->key_present(keyRef)) return sw(resp, PIV_SW_REF_NOT_FOUND);
        uint8_t alg[1] = {PIV_ALG_ECC_P256};
        tlvWrite(g_build, sizeof(g_build), &p, 0x01, alg, 1);
        uint8_t policy[2] = {0x01, 0x01};  // PIN once, touch never (default)
        tlvWrite(g_build, sizeof(g_build), &p, 0x02, policy, 2);
        uint8_t origin[1] = {0x01};        // generated on card
        tlvWrite(g_build, sizeof(g_build), &p, 0x03, origin, 1);
        uint8_t pub[65];
        if (g_backend->key_pubkey(keyRef, pub)) {
            // Public key wrapped as 7F49 { 86 point }, per Yubico metadata.
            uint8_t inner[80];
            size_t ip = 0;
            tlvWrite(inner, sizeof(inner), &ip, PIV_TAG_PUBKEY_POINT, pub, 65);
            tlvWrite(g_build, sizeof(g_build), &p, 0x04, inner, ip);
        }
        return emit(g_build, p, le, resp, resp_max);
    }
    return sw(resp, PIV_SW_REF_NOT_FOUND);
}

// --- Yubico SET MANAGEMENT KEY (INS FF) ------------------------------------
int cmdSetMgmtKey(const apdu_t* a, const uint8_t* data, size_t lc, uint8_t* resp) {
    if (!g_mgmt_authenticated) return sw(resp, PIV_SW_SECURITY_STATUS);
    // Data: <alg><keyref 0x9B><len><key>. P1 is touch policy (ignored).
    (void)a;
    if (lc < 3) return sw(resp, PIV_SW_DATA_INVALID);
    uint8_t alg = data[0];
    if (data[1] != PIV_KEY_9B) return sw(resp, PIV_SW_DATA_INVALID);
    uint8_t keyLen = data[2];
    if (static_cast<size_t>(3 + keyLen) > lc) return sw(resp, PIV_SW_DATA_INVALID);
    if (alg != PIV_ALG_AES128 && alg != PIV_ALG_AES192 && alg != PIV_ALG_AES256)
        return sw(resp, PIV_SW_DATA_INVALID);
    if (!g_backend->mgmt_set_key(alg, data + 3, keyLen)) return sw(resp, PIV_SW_SECURITY_STATUS);
    return sw(resp, PIV_SW_OK);
}

// --- Dispatch --------------------------------------------------------------
int dispatch(const apdu_t* a, const uint8_t* data, size_t lc,
             uint8_t* resp, size_t resp_max) {
    switch (a->ins) {
        case PIV_INS_SELECT:
            return cmdSelect(resp, resp_max, a->le);
        case PIV_INS_GET_DATA:
            return cmdGetData(data, lc, resp, resp_max, a->le);
        case PIV_INS_VERIFY:
            return cmdVerify(a, data, lc, resp);
        case PIV_INS_GENERAL_AUTH:
            return cmdGeneralAuth(a, data, lc, resp, resp_max, a->le);
        case PIV_INS_GENERATE_KEYPAIR:
            return cmdGenerate(a, data, lc, resp, resp_max, a->le);
        case PIV_INS_PUT_DATA:
            return cmdPutData(data, lc, resp);
        case PIV_INS_GET_METADATA:
            return cmdGetMetadata(a, resp, resp_max, a->le);
        case PIV_INS_SET_MGMT_KEY:
            return cmdSetMgmtKey(a, data, lc, resp);
        case PIV_INS_CHANGE_REFERENCE:
            // Application PIN is the badge PIN, changed only from the device UI.
            return sw(resp, PIV_SW_SECURITY_STATUS);
        default:
            return sw(resp, PIV_SW_INS_NOT_SUPPORTED);
    }
}

int processApdu(const uint8_t* cmd, size_t cmd_len, uint8_t* resp, size_t resp_max) {
    if (!g_backend || resp_max < 2) return sw(resp, PIV_SW_INS_NOT_SUPPORTED);
    apdu_t a;
    if (!apdu_parse(cmd, cmd_len, &a)) return sw(resp, PIV_SW_WRONG_LENGTH);

    // GET RESPONSE is answered directly and never disturbs the chain state.
    if (a.ins == PIV_INS_GET_RESPONSE && (a.cla & ~CLA_CHAIN) == CLA_ISO7816)
        return cmdGetResponse(&a, resp, resp_max);

    // Command chaining: accumulate the data field of non-final blocks.
    if (a.cla & CLA_CHAIN) {
        if (g_chain_len == 0) {
            g_chain_ins = a.ins; g_chain_p1 = a.p1; g_chain_p2 = a.p2;
        } else if (a.ins != g_chain_ins || a.p1 != g_chain_p1 || a.p2 != g_chain_p2) {
            resetChain();
            return sw(resp, PIV_SW_DATA_INVALID);
        }
        if (g_chain_len + a.lc > sizeof(g_chain)) {
            resetChain();
            return sw(resp, PIV_SW_WRONG_LENGTH);
        }
        memcpy(g_chain + g_chain_len, a.data, a.lc);
        g_chain_len += a.lc;
        return sw(resp, PIV_SW_OK);
    }

    // Final block: splice any accumulated chain in front of this data.
    const uint8_t* data = a.data;
    size_t lc = a.lc;
    if (g_chain_len > 0) {
        if (a.ins != g_chain_ins || a.p1 != g_chain_p1 || a.p2 != g_chain_p2) {
            resetChain();
            return sw(resp, PIV_SW_DATA_INVALID);
        }
        if (g_chain_len + a.lc > sizeof(g_chain)) {
            resetChain();
            return sw(resp, PIV_SW_WRONG_LENGTH);
        }
        memcpy(g_chain + g_chain_len, a.data, a.lc);
        g_chain_len += a.lc;
        data = g_chain;
        lc = g_chain_len;
    }

    int r = dispatch(&a, data, lc, resp, resp_max);
    resetChain();
    return r;
}

void deselect() {
    g_pin_verified = false;
    g_mgmt_authenticated = false;
    g_witness_valid = false;
    memset(g_witness, 0, sizeof(g_witness));
    resetChain();
    g_resp_pos = 0;
    g_resp_remaining = 0;
    memset(g_resp_buf, 0, sizeof(g_resp_buf));
}

} // namespace
} // namespace cdc::mod_piv

// --- C surface -------------------------------------------------------------
using namespace cdc::mod_piv;

extern "C" void piv_set_backend(const piv_backend_t* backend) {
    g_backend = backend;
}

extern "C" const scard_applet_t* piv_applet(void) {
    static const uint8_t kAid[PIV_AID_PREFIX_LEN] = {
        0xA0, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x10, 0x00
    };
    static const scard_applet_t applet = {
        "piv",
        kAid,
        PIV_AID_PREFIX_LEN,
        processApdu,
        deselect,
    };
    return &applet;
}
