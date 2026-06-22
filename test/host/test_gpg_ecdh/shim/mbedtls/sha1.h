#pragma once
// Minimal no-op mbedtls SHA-1 shim for the native host test. Only present so
// fingerprint.cpp links on the host; buildEcdhPubkeyBody (the unit under test)
// never calls into it.
#include <cstddef>

typedef struct { int unused; } mbedtls_sha1_context;

static inline void mbedtls_sha1_init(mbedtls_sha1_context*) {}
static inline void mbedtls_sha1_free(mbedtls_sha1_context*) {}
static inline int  mbedtls_sha1_starts(mbedtls_sha1_context*) { return 0; }
static inline int  mbedtls_sha1_update(mbedtls_sha1_context*, const unsigned char*, size_t) { return 0; }
static inline int  mbedtls_sha1_finish(mbedtls_sha1_context*, unsigned char out[20]) {
    for (int i = 0; i < 20; ++i) out[i] = 0;
    return 0;
}
