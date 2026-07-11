/*
 * YKOATH applet: exposes the existing 2FA credential store (OathStore) over the
 * shared CCID interface so ykman / Yubico Authenticator can list and compute
 * codes. The credential store is the single source of truth; this applet holds
 * no secrets of its own beyond the CCID access key.
 *
 * The protocol state machine is free of secure-element / mbedtls / NVS
 * dependencies: every side effect goes through oath_backend_t, so the command
 * surface is host-testable with an in-memory fake.
 */

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "cdc_scard/applet.h"

#ifdef __cplusplus
extern "C" {
#endif

// YKOATH credential metadata for one stored account.
typedef struct {
    bool used;
    char name[17];      // account label (OathStore NAME_LEN + NUL)
    char issuer[33];    // issuer text (OathStore ISSUER_LEN + NUL)
    uint8_t type;       // 0 = TOTP, 1 = HOTP, 2 = CR
    uint8_t algorithm;  // 0 = SHA1, 1 = SHA256, 2 = SHA512
    uint8_t digits;
    uint32_t period;
    uint8_t flags;      // bit0 = touch required
} oath_meta_t;

// Store + crypto backend. All pointers must be set before registration.
typedef struct {
    // Enumeration.
    uint16_t (*capacity)(void);
    bool (*read)(uint16_t slot, oath_meta_t* out);

    // CALCULATE backend: dynamic-truncated response for an 8-byte challenge.
    bool (*calculate)(uint16_t slot, const uint8_t challenge[8],
                      uint8_t trunc[4], uint8_t* digits);

    // Mutations. addRaw overwrites an existing same-name entry.
    bool (*addRaw)(uint8_t type, const char* name, const char* issuer,
                   const uint8_t* key, uint8_t keyLen, uint8_t digits,
                   uint32_t period, uint8_t algorithm, uint64_t counter, uint8_t flags);
    bool (*remove)(uint16_t slot);
    void (*wipeAll)(void);

    // Access key (CCID password). akeyGet returns false when no key is set.
    void (*devId)(uint8_t out[8]);
    bool (*akeyGet)(uint8_t out[16]);
    bool (*akeySet)(const uint8_t key[16]);
    void (*akeyClear)(void);

    // HMAC-SHA1 for access-key challenge/response.
    bool (*hmacSha1)(const uint8_t* key, size_t keyLen,
                     const uint8_t* data, size_t dataLen, uint8_t out[20]);
    bool (*rng)(uint8_t* buf, size_t len);
} oath_backend_t;

void oath_set_backend(const oath_backend_t* backend);
const scard_applet_t* oath_applet(void);

#ifdef __cplusplus
}
#endif
