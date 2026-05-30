/**
 * \file VfatFs.h
 * \brief File operations confined to the plugins FAT partition.
 *
 * Shared by the serial shell and the GUI explorer. All paths are relative to
 * the partition root (PluginStorage::basePath(), e.g. "/plugins") and may not
 * escape it: any ".." component or absolute path is rejected.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace cdc::mod_vfat {

/// One directory entry returned by \ref fs::list.
struct FsEntry {
    std::string name;
    bool        is_dir = false;
    uint32_t    size   = 0;  // bytes (files only)
};

namespace fs {

/// Cap on entries returned by a single list() call.
constexpr size_t MAX_LIST_ENTRIES = 256;

/// \brief Partition root mount path (e.g. "/plugins").
const char* root();

/**
 * \brief Resolve a root-relative path to an absolute VFS path.
 * \param relPath Path relative to root; '/'-separated, no ".." or leading '/'.
 * \param absOut Receives the absolute path on success.
 * \return false if the path is unsafe (escapes root).
 */
bool resolve(const std::string& relPath, std::string& absOut);

/**
 * \brief List a directory: directories first, then files.
 * \param relDir Directory relative to root ("" = root).
 * \param out Receives the entries (capped at MAX_LIST_ENTRIES).
 * \param truncated Set true if more entries existed than the cap.
 * \return false if the directory could not be opened.
 */
bool list(const std::string& relDir, std::vector<FsEntry>& out, bool& truncated);

/// \brief Read a file into `out` (up to maxBytes). \return false if not found.
bool readText(const std::string& relFile, std::string& out, size_t maxBytes);

/// \brief Create/overwrite `relFile` with `len` bytes. \return false on error.
bool writeText(const std::string& relFile, const char* data, size_t len);

/// \brief Delete a file. \return false if not found.
bool removeFile(const std::string& relFile);

/// \brief Create a directory. \return false on error.
bool makeDir(const std::string& relDir);

/// \brief Remove an (empty) directory. \return false on error / not empty.
bool removeDir(const std::string& relDir);

/// \brief True if `relPath` exists and is a directory.
bool isDir(const std::string& relPath);

/// \brief True if `relPath` exists (file or directory).
bool exists(const std::string& relPath);

/// \brief Partition totals in KiB. \return false if stats are unavailable.
bool stats(uint32_t& totalKB, uint32_t& freeKB);

}  // namespace fs
}  // namespace cdc::mod_vfat
