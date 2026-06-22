#pragma once
// Minimal no-op mbedtls SHA-256 shim for the native host test. Only present so
// fingerprint.cpp links on the host; buildEcdhPubkeyBody (the unit under test)
// never calls into it.
#include <cstddef>

typedef struct { int unused; } mbedtls_sha256_context;

static inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
static inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
static inline int  mbedtls_sha256_starts(mbedtls_sha256_context*, int) { return 0; }
static inline int  mbedtls_sha256_update(mbedtls_sha256_context*, const unsigned char*, size_t) { return 0; }
static inline int  mbedtls_sha256_finish(mbedtls_sha256_context*, unsigned char out[32]) {
    for (int i = 0; i < 32; ++i) out[i] = 0;
    return 0;
}
static inline int  mbedtls_sha256(const unsigned char*, size_t, unsigned char out[32], int) {
    for (int i = 0; i < 32; ++i) out[i] = 0;
    return 0;
}
