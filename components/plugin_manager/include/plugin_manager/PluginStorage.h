/**
 * \file PluginStorage.h
 * \brief Mounts the FAT-FS partition that holds plugin .wasm + .meta files.
 *
 * Reads /plugins via the standard ESP-IDF VFS so PluginManager can list and
 * load installed plugins. Auto-formats the partition on first boot if it has
 * not been initialised yet.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace cdc::plugin_manager {

class PluginStorage {
public:
    /**
     * \brief Mount the plugins partition. Auto-formats if empty.
     * \return true on success.
     */
    static bool mount();

    /**
     * \brief Unmount the plugins partition (rarely used; mostly tests).
     */
    static void unmount();

    /**
     * \brief Returns the VFS path prefix, e.g. "/vfat".
     */
    static const char* basePath();

    /**
     * \brief Discover all installed plugin ids. A plugin is recognised by
     *        the presence of both `<id>.wasm` and `<id>.meta` files.
     */
    static std::vector<std::string> listPluginIds();

    /**
     * \brief Returns the path that should be loaded for `<id>`: `<id>.aot` if
     *        it exists on disk, otherwise `<id>.wasm`.
     */
    static std::string binaryPath(const std::string& id);

    /**
     * \brief Returns the full VFS path of `<id>.wasm`.
     */
    static std::string wasmPath(const std::string& id);

    /**
     * \brief Returns the full VFS path of `<id>.aot`.
     */
    static std::string aotPath(const std::string& id);

    /**
     * \brief Returns the full VFS path of `<id>.meta`.
     */
    static std::string metaPath(const std::string& id);

    /**
     * \brief Returns the full VFS path of `<id>.lang` (translation overlay).
     */
    static std::string langPath(const std::string& id);

    /**
     * \brief Returns the full VFS path of `<id>.disabled`.
     */
    static std::string disabledPath(const std::string& id);

    /**
     * \brief True when the plugin has a persistent disabled marker.
     */
    static bool isDisabled(const std::string& id);

    /**
     * \brief Create or remove the persistent disabled marker for a plugin.
     */
    static bool setDisabled(const std::string& id, bool disabled);

    /**
     * \brief Returns the free and total bytes on the plugins partition.
     */
    static bool stats(uint64_t& free_bytes, uint64_t& total_bytes);

    // --- USB Mass Storage block access (vfat wear-levelling device) ---

    /**
     * \brief Logical sector size of the vfat volume in bytes (MSC block size).
     * \return Sector size, or 0 when not mounted.
     */
    static uint16_t blockSize();

    /**
     * \brief Total accessible size of the vfat volume in bytes.
     * \return Volume size, or 0 when not mounted.
     */
    static uint64_t blockTotalBytes();

    /**
     * \brief Reads raw bytes from the wear-levelling logical space.
     * \param lba Logical block address (in blockSize() units).
     * \param offset Byte offset added inside the block.
     * \param buf Destination buffer.
     * \param len Number of bytes to read.
     * \return true on success (bounds-checked against the volume size).
     */
    static bool blockRead(uint32_t lba, uint32_t offset, void* buf, uint32_t len);

    /**
     * \brief Erases and writes raw bytes to the wear-levelling logical space.
     *
     * \p offset must be 0 and \p len a multiple of blockSize(); the addressed
     * range is erased then written, matching the FATFS-over-WL sector semantics.
     * \param lba Logical block address (in blockSize() units).
     * \param offset Byte offset inside the block (must be 0).
     * \param buf Source buffer.
     * \param len Number of bytes to write.
     * \return true on success (bounds-checked against the volume size).
     */
    static bool blockWrite(uint32_t lba, uint32_t offset, const void* buf, uint32_t len);

    // --- USB Mass Storage host-active gate ---

    /**
     * \brief Marks whether a USB host currently holds the volume over MSC.
     *
     * While active, badge-side writers MUST refuse to write (the host is the
     * sole writer). The active->inactive edge schedules a remount (performed by
     * remountIfPending() on a safe task) so host-written files become visible.
     * Safe to call from the USB task.
     * \param active true when a host has mounted the MSC LUN.
     */
    static void setHostActive(bool active);

    /**
     * \brief Reports whether a USB host currently holds the volume over MSC.
     * \return true while badge-side writes must be refused.
     */
    static bool hostActive();

    /**
     * \brief Performs a deferred remount scheduled by setHostActive(false).
     *
     * Call from a non-USB task (e.g. a module tick) so the FATFS cache is
     * refreshed and host-written files appear in the file browser. No-op when
     * nothing is pending or a host is still attached.
     */
    static void remountIfPending();

    /**
     * \brief Applies advisory read-only + hidden + system FAT attributes to the
     *        system folder so standard host file managers hide and protect it.
     *
     * Best-effort: skipped while hostActive(); failures are logged and ignored.
     */
    static void protectSystemDir();
};

}  // namespace cdc::plugin_manager
