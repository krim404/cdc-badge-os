/**
 * \file PluginSerialCommands.h
 * \brief Serial console PLUGIN command bundle. Call once from main.cpp
 *        after PluginManager::init() to expose LIST/INFO/UPLOAD/DELETE/
 *        START/STOP via USB-CDC.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace cdc::plugin_manager {

void registerPluginSerialCommands();

/**
 * \brief Receive a file over USB-CDC into the plugins partition.
 *
 * Arms the same binary byte-stream receiver as the PLUGIN UPLOAD commands:
 * replies "READY", then expects exactly `size` raw bytes whose CRC32 must
 * equal `crc`, writes them to `abs_path` and replies "OK <size>". No
 * post-finalize action is taken (pure file write); callers that need a reload
 * issue it separately (e.g. LANG RELOAD). The caller resolves and validates
 * `abs_path` (absolute, inside the partition); the VFAT serial shell resolves
 * it against its working directory.
 *
 * \param abs_path Absolute destination path, e.g. "/vfat/data/notes.txt".
 * \param size Total payload size in bytes (must be > 0).
 * \param crc Expected CRC32 of the whole payload.
 * \return true if the receiver was armed (READY sent), false on error.
 */
bool beginFileReceive(const char* abs_path, size_t size, uint32_t crc);

}  // namespace cdc::plugin_manager
