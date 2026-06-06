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
     * \brief Returns the VFS path prefix, e.g. "/plugins".
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
};

}  // namespace cdc::plugin_manager
