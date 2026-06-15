/**
 * \file cdc_log.h (host stub)
 * \brief No-op logging macros so firmware cbor_helpers.cpp links on host.
 *
 * cbor_helpers.cpp uses only LOG_E for overflow diagnostics. The native env
 * has no cdc_log library; this stub satisfies the include so the REAL firmware
 * CBOR encoder/reader can be compiled and unit-tested directly on host.
 */
#pragma once
#define LOG_E(tag, ...) ((void)0)
#define LOG_W(tag, ...) ((void)0)
#define LOG_I(tag, ...) ((void)0)
#define LOG_D(tag, ...) ((void)0)
