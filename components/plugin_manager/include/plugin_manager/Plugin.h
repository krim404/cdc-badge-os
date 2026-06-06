/**
 * \file Plugin.h
 * \brief Owned WAMR module instance + per-plugin state.
 *
 * One Plugin object exists per currently running plugin. Holds the loaded
 * bytecode buffer (in PSRAM), the WAMR module + instance handles, the parsed
 * manifest, and the list of host resources (WiFi, BLE, GPIO pins, ...) that
 * were acquired during prerequisite setup so they can be released in reverse
 * order on exit. All owned resources use RAII wrappers - the destructor is
 * the only cleanup path.
 */

#pragma once

#include "plugin_manager/PluginManifest.h"
#include "plugin_manager/Raii.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Forward-declare WAMR types here so consumers of this header don't pull in
// wasm_export.h (which clashes with cdc_log's log_level_t).
struct WASMModuleCommon;
struct WASMModuleInstanceCommon;
struct WASMExecEnv;

namespace cdc::plugin_manager {

struct WasmModuleDeleter {
    void operator()(WASMModuleCommon* m) const noexcept;
};
struct WasmInstanceDeleter {
    void operator()(WASMModuleInstanceCommon* m) const noexcept;
};
struct WasmExecEnvDeleter {
    void operator()(WASMExecEnv* e) const noexcept;
};

using WasmModulePtr   = std::unique_ptr<WASMModuleCommon, WasmModuleDeleter>;
using WasmInstancePtr = std::unique_ptr<WASMModuleInstanceCommon, WasmInstanceDeleter>;
using WasmExecEnvPtr  = std::unique_ptr<WASMExecEnv, WasmExecEnvDeleter>;

class Plugin {
public:
    Plugin() noexcept;
    ~Plugin();

    Plugin(const Plugin&)            = delete;
    Plugin& operator=(const Plugin&) = delete;
    Plugin(Plugin&&) noexcept            = default;
    Plugin& operator=(Plugin&&) noexcept = default;

    /**
     * \brief Load bytecode + instantiate the module. Does NOT run plugin_init
     *        yet - the manager handles ordering of init + prerequisites.
     */
    [[nodiscard]] bool load(const std::string& id, const PluginManifest& manifest);

    /// Destroy WAMR instance + free bytecode buffer. Idempotent.
    void unload() noexcept;

    /**
     * \brief Call an exported i32(i32...)->i32 function by name.
     * \param name Exported function name.
     * \param args Optional int32 arguments.
     * \param out_i32 Receives the i32 return value when non-null.
     * \return false if the export is missing or the call trapped.
     */
    [[nodiscard]] bool callI(const char* name,
                             std::initializer_list<int32_t> args = {},
                             int32_t* out_i32 = nullptr);

    [[nodiscard]] bool hasExport(const char* name) const;

    /// True if the most recent callI() failed because the WASM module trapped
    /// (as opposed to a missing export). Reset on every callI().
    [[nodiscard]] bool lastCallTrapped() const noexcept { return last_call_trapped_; }

    /// WAMR exception text captured by the last trapping callI(). Empty string
    /// if the last call did not trap.
    [[nodiscard]] const char* lastTrapMessage() const noexcept { return last_trap_; }

    [[nodiscard]] const PluginManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] const std::string&    id()       const noexcept { return id_; }
    [[nodiscard]] bool                  isLoaded() const noexcept
    {
        return module_inst_ != nullptr;
    }

    /**
     * \brief Load the plugin's translation overlay from disk into PSRAM.
     *
     * Parses a JSON file with a `{ "translations": { "<code>": {...} } }`
     * schema and keeps the active language's key/value pairs in a sorted array
     * for binary-search lookup via `trKey()`. Missing or invalid file is
     * non-fatal - lookups fall back to the manifest's English strings.
     *
     * \param path Filesystem path, defaults to `/plugins/<id>.lang`.
     * \return true on success.
     */
    bool loadLangOverlay(const char* path = nullptr);

    /**
     * \brief Look up a plugin-local translation key in the loaded overlay.
     * \param key Plugin-local key (e.g. "toggle_ok").
     * \return Pointer into PSRAM overlay storage, or nullptr if not found.
     *         Stable until the next `loadLangOverlay()` call.
     */
    [[nodiscard]] const char* trKey(const char* key) const noexcept;

    // Resources acquired during prerequisite setup; released in reverse order
    // by PluginManager on exit. Stored as opaque prerequisite names for now.
    std::vector<std::string> acquired_prereqs;

private:
    struct OverlayEntry {
        std::string key;
        std::string value;
    };

    std::string             id_;
    PluginManifest          manifest_;
    PsramUniquePtr<uint8_t> bytecode_;
    std::size_t             bytecode_len_ = 0;
    WasmModulePtr           module_;
    WasmInstancePtr         module_inst_;
    WasmExecEnvPtr          exec_env_;

    // Per-plugin translation overlay, sorted by key. Loaded from `<id>.lang`.
    // Active language only; reload on language switch.
    std::vector<OverlayEntry> langOverlay_;
    std::string               langOverlayLang_;

    std::set<std::string>     missingExports_;

    bool last_call_trapped_ = false;
    char last_trap_[160]    = {0};
};

}  // namespace cdc::plugin_manager
