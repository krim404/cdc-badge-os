/*
 * PIV data-object builders: SELECT response (APT), CHUID, CCC, Discovery.
 * Pure functions (host-testable); the raw GUID / CCC id come from piv_state.
 */

#pragma once
#include <stdint.h>
#include <stddef.h>

#include "piv_defs.h"

namespace cdc::mod_piv {

// 5FC1xx data-object tags (full BER tags).
constexpr uint32_t PIV_OBJ_CHUID     = 0x5FC102;
constexpr uint32_t PIV_OBJ_CCC       = 0x5FC107;
constexpr uint32_t PIV_OBJ_CERT_9A   = 0x5FC105;
constexpr uint32_t PIV_OBJ_CERT_9E   = 0x5FC101;
constexpr uint32_t PIV_OBJ_CERT_9C   = 0x5FC10A;
constexpr uint32_t PIV_OBJ_CERT_9D   = 0x5FC10B;
constexpr uint32_t PIV_OBJ_DISCOVERY = 0x7E;

/**
 * \brief Maps a certificate object tag to its PIV key reference.
 * \return Key ref (0x9A/0x9C/0x9D/0x9E) or 0 if the tag is not a cert object.
 */
uint8_t certObjectToKeyRef(uint32_t objectTag);

/**
 * \brief Builds the SELECT Application Property Template (tag 0x61).
 * \return APT length, or 0 on overflow.
 */
size_t buildApt(uint8_t* out, size_t outCap);

/**
 * \brief Builds the CHUID object body (content inside tag 0x53).
 * \param guid 16-byte Card UUID (tag 0x34).
 * \return Body length, or 0 on overflow.
 */
size_t buildChuid(const uint8_t guid[16], uint8_t* out, size_t outCap);

/**
 * \brief Builds the CCC object body (content inside tag 0x53).
 * \param cardId 14-byte card identifier embedded in the F0 field.
 * \return Body length, or 0 on overflow.
 */
size_t buildCcc(const uint8_t cardId[14], uint8_t* out, size_t outCap);

/**
 * \brief Builds the Discovery Object (bare tag 0x7E, not wrapped in 0x53).
 * \return Object length including the 0x7E tag, or 0 on overflow.
 */
size_t buildDiscovery(uint8_t* out, size_t outCap);

} // namespace cdc::mod_piv
