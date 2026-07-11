/*
 * Minimal BER-TLV reader/writer and ECDSA DER encoder for the PIV applet.
 * Pure functions, no hardware dependency (host-testable).
 */

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace cdc::mod_piv {

/**
 * \brief Locates a top-level BER-TLV element by tag within a buffer.
 *
 * Handles 1- and 2-byte tags (first tag byte with low 5 bits set continues
 * into a second byte) and 1/2/3-byte length encodings. Only the first match
 * is returned.
 *
 * \param buf Input TLV buffer.
 * \param len Buffer length.
 * \param tag Tag to search for (up to 2 bytes, e.g. 0x5C or 0x7F49).
 * \param valueOut Receives a pointer to the value bytes on success.
 * \param valueLenOut Receives the value length on success.
 * \return true if the tag was found.
 */
bool tlvFind(const uint8_t* buf, size_t len, uint32_t tag,
             const uint8_t** valueOut, size_t* valueLenOut);

/**
 * \brief Writes a TLV tag into a buffer.
 * \return Number of bytes written, or 0 on overflow.
 */
size_t tlvWriteTag(uint8_t* buf, size_t cap, uint32_t tag);

/**
 * \brief Writes a BER length field into a buffer.
 * \return Number of bytes written, or 0 on overflow.
 */
size_t tlvWriteLen(uint8_t* buf, size_t cap, size_t len);

/**
 * \brief Writes a complete tag-length-value element.
 * \param pos In/out write cursor; advanced past the written element.
 * \return true on success, false on overflow (cursor unchanged on failure).
 */
bool tlvWrite(uint8_t* buf, size_t cap, size_t* pos, uint32_t tag,
              const uint8_t* value, size_t valueLen);

/**
 * \brief Encodes a raw 64-byte ECDSA R||S signature as a DER SEQUENCE of two
 *        INTEGERs (r, s), minimally encoded with the sign-bit rule.
 * \param rs 64-byte input (R||S).
 * \param out Output DER buffer.
 * \param outCap Output capacity.
 * \return DER length, or 0 on overflow.
 */
size_t derEncodeEcdsaSig(const uint8_t rs[64], uint8_t* out, size_t outCap);

} // namespace cdc::mod_piv
