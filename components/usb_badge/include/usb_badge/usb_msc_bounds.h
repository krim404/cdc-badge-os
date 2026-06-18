// Pure bounds/alignment validation for a USB MSC block access.
//
// Kept dependency-free (stdint/stdbool only) so it is shared by the firmware
// block layer and host unit tests.

#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * \brief Validates an MSC block access against the volume geometry.
 *
 * Reads may use any offset/length within bounds. Writes must be sector-aligned
 * (offset 0 and length a whole number of blocks), matching the erase-then-write
 * wear-levelling semantics.
 *
 * \param total_bytes Total accessible volume size.
 * \param block_size Logical block (sector) size.
 * \param lba Logical block address.
 * \param offset Byte offset inside the block.
 * \param len Access length in bytes.
 * \param is_write True for a write access (stricter alignment).
 * \return true when the access is in range and (for writes) aligned.
 */
static inline bool usb_msc_range_ok(uint64_t total_bytes, uint16_t block_size,
                                    uint32_t lba, uint32_t offset, uint32_t len,
                                    bool is_write) {
    if (block_size == 0) return false;
    if (is_write && (offset != 0 || (len % block_size) != 0)) return false;
    uint64_t addr = (uint64_t)lba * block_size + offset;
    if (addr + len > total_bytes) return false;
    return true;
}

/**
 * \brief Whether a host-active transition should trigger a badge remount.
 *
 * Only the active->inactive edge (host detached) refreshes the FATFS view so
 * host-written files become visible.
 * \param prev Previous host-active state.
 * \param cur New host-active state.
 * \return true when the volume should be remounted.
 */
static inline bool usb_msc_should_remount(bool prev, bool cur) {
    return prev && !cur;
}
