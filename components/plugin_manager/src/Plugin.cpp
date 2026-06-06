#include "plugin_manager/Plugin.h"
#include "plugin_manager/PluginStorage.h"
#include "cdc_core/Raii.h"
#include "cdc_ui/I18n.h"
#include "cJSON.h"

extern "C" {
#include "wasm_export.h"
void plg_log_info (const char* msg);
void plg_log_warn (const char* msg);
void plg_log_error(const char* msg);
void plg_set_active_plugin(void* plugin);
void* plg_get_active_plugin(void);
}

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cdc::plugin_manager {

void WasmModuleDeleter::operator()(WASMModuleCommon* m) const noexcept
{
    if (m) wasm_runtime_unload(m);
}

void WasmInstanceDeleter::operator()(WASMModuleInstanceCommon* m) const noexcept
{
    if (m) wasm_runtime_deinstantiate(m);
}

void WasmExecEnvDeleter::operator()(WASMExecEnv* e) const noexcept
{
    if (e) wasm_runtime_destroy_exec_env(e);
}

Plugin::Plugin() noexcept = default;
Plugin::~Plugin()         = default;

namespace {

// Per-call WASM instruction budget. Caps a runaway or hostile guest loop so the
// call traps with an exception and returns instead of hanging the plugin tick
// task forever. Generous enough that legitimate per-tick work (e.g. a few Olm
// session establishes in one tick) never reaches it, while a true infinite loop
// still traps within a few seconds of tick-task time.
constexpr int kInstrLimit = 500'000'000;

// WASM bytecode can be hundreds of KB. Keep it in PSRAM so it does not chew
// into internal SRAM, which is the project-wide bottleneck (see CLAUDE.md).
[[nodiscard]] bool load_bytecode_psram(const std::string& path,
                                       PsramUniquePtr<uint8_t>& out,
                                       std::size_t& out_len)
{
    out.reset();
    out_len = 0;

    auto fp = ::cdc::core::openFile(path.c_str(), "rb");
    if (!fp) return false;
    std::fseek(fp.get(), 0, SEEK_END);
    long n = std::ftell(fp.get());
    if (n <= 0) return false;
    std::fseek(fp.get(), 0, SEEK_SET);

    auto buf = psramAlloc<uint8_t>(static_cast<std::size_t>(n));
    if (!buf) return false;

    if (std::fread(buf.get(), 1, n, fp.get()) != static_cast<std::size_t>(n)) {
        return false;
    }
    out     = std::move(buf);
    out_len = static_cast<std::size_t>(n);
    return true;
}

}  // namespace

bool Plugin::load(const std::string& id, const PluginManifest& manifest)
{
    id_       = id;
    manifest_ = manifest;

    if (!load_bytecode_psram(PluginStorage::binaryPath(id), bytecode_, bytecode_len_)) {
        plg_log_error("plugin: failed to read plugin binary into PSRAM");
        return false;
    }

    char err_buf[128] = {0};
    module_.reset(wasm_runtime_load(bytecode_.get(),
                                    static_cast<uint32_t>(bytecode_len_),
                                    err_buf, sizeof(err_buf)));
    if (!module_) {
        plg_log_error("plugin: wasm_runtime_load failed");
        plg_log_error(err_buf);
        unload();
        return false;
    }

    (void)manifest_.linear_memory_kb;
    const uint32_t stack_bytes = 64 * 1024;
    // heap_bytes=0 is mandatory for Rust plugins. Any non-zero value makes
    // WAMR re-purpose __heap_base for its own GC allocator, which collides
    // with Rust's allocator (Rust grows linear memory directly via
    // memory.grow). Plugins that need more memory must set --initial-memory
    // / --max-memory via their own build.rs.
    const uint32_t heap_bytes  = 0;

    module_inst_.reset(wasm_runtime_instantiate(module_.get(), stack_bytes,
                                                heap_bytes, err_buf,
                                                sizeof(err_buf)));
    if (!module_inst_) {
        plg_log_error("plugin: wasm_runtime_instantiate failed");
        plg_log_error(err_buf);
        unload();
        return false;
    }

    exec_env_.reset(wasm_runtime_create_exec_env(module_inst_.get(), stack_bytes));
    if (!exec_env_) {
        plg_log_error("plugin: wasm_runtime_create_exec_env failed");
        unload();
        return false;
    }

    return true;
}

bool Plugin::hasExport(const char* name) const
{
    if (!isLoaded() || !name) return false;
    return wasm_runtime_lookup_function(module_inst_.get(), name) != nullptr;
}

void Plugin::unload() noexcept
{
    // RAII order: exec_env -> module_inst -> module -> bytecode.
    exec_env_.reset();
    module_inst_.reset();
    module_.reset();
    bytecode_.reset();
    bytecode_len_ = 0;
    acquired_prereqs.clear();
}

bool Plugin::callI(const char* name,
                   std::initializer_list<int32_t> args,
                   int32_t* out_i32)
{
    if (!isLoaded()) return false;

    // WAMR uses argv as both input slots and the return-value slot, so it
    // must hold at least max(argc, 1) uint32_t.
    constexpr std::size_t kMaxArgs = 8;
    if (args.size() > kMaxArgs) {
        plg_log_error("plugin: callI argument overflow");
        return false;
    }
    uint32_t argv[kMaxArgs + 1] = {0};
    std::size_t i = 0;
    for (int32_t a : args) argv[i++] = static_cast<uint32_t>(a);
    const uint32_t argc = static_cast<uint32_t>(args.size());

    void* prev_active = plg_get_active_plugin();
    plg_set_active_plugin(this);
    wasm_runtime_set_instruction_count_limit(exec_env_.get(), kInstrLimit);
    bool ok = true;
    last_call_trapped_ = false;
    wasm_function_inst_t fn = wasm_runtime_lookup_function(module_inst_.get(), name);
    if (!fn) {
        if (missingExports_.insert(name).second) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "plugin: missing export '%s'", name);
            plg_log_warn(buf);
        }
        ok = false;
    } else if (!wasm_runtime_call_wasm(exec_env_.get(), fn, argc, argv)) {
        const char* exc = wasm_runtime_get_exception(module_inst_.get());
        std::snprintf(last_trap_, sizeof(last_trap_), "%s", exc ? exc : "unknown trap");
        last_call_trapped_ = true;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "plugin: call '%s' failed: %s", name, last_trap_);
        plg_log_error(buf);
        wasm_runtime_clear_exception(module_inst_.get());
        ok = false;
    } else if (out_i32) {
        *out_i32 = static_cast<int32_t>(argv[0]);
    }
    plg_set_active_plugin(prev_active);
    return ok;
}

bool Plugin::loadLangOverlay(const char* path)
{
    langOverlay_.clear();
    langOverlayLang_.clear();

    std::string default_path;
    if (!path || !*path) {
        default_path = PluginStorage::langPath(id_);
        path = default_path.c_str();
    }

    auto fp = ::cdc::core::openFile(path, "rb");
    if (!fp) return false;

    std::fseek(fp.get(), 0, SEEK_END);
    long size = std::ftell(fp.get());
    std::fseek(fp.get(), 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) {
        plg_log_warn("plugin: lang file size invalid");
        return false;
    }

    auto buf = ::cdc::core::psramAlloc<char>(static_cast<std::size_t>(size) + 1);
    if (!buf) return false;
    if (std::fread(buf.get(), 1, size, fp.get()) != static_cast<size_t>(size)) {
        return false;
    }
    buf.get()[size] = '\0';

    cJSON* root = cJSON_Parse(buf.get());
    if (!root) {
        plg_log_warn("plugin: lang file JSON parse failed");
        return false;
    }

    cJSON* translations = cJSON_GetObjectItemCaseSensitive(root, "translations");
    if (!translations || !cJSON_IsObject(translations)) {
        cJSON_Delete(root);
        return false;
    }

    const std::string& active = ::cdc::ui::I18n::instance().getLanguageCode();
    if (active == "en") {
        cJSON_Delete(root);
        return true;
    }

    cJSON* lang_obj = cJSON_GetObjectItemCaseSensitive(translations, active.c_str());
    if (!lang_obj || !cJSON_IsObject(lang_obj)) {
        cJSON_Delete(root);
        return true;
    }

    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, lang_obj) {
        if (!cJSON_IsString(entry) || !entry->string || !entry->valuestring) continue;
        langOverlay_.push_back({entry->string, entry->valuestring});
    }
    std::sort(langOverlay_.begin(), langOverlay_.end(),
              [](const OverlayEntry& a, const OverlayEntry& b) {
                  return a.key < b.key;
              });
    langOverlayLang_ = active;

    cJSON_Delete(root);
    return true;
}

const char* Plugin::trKey(const char* key) const noexcept
{
    if (!key || langOverlay_.empty()) return nullptr;
    auto it = std::lower_bound(
        langOverlay_.begin(), langOverlay_.end(), key,
        [](const OverlayEntry& e, const char* k) { return e.key < k; });
    if (it != langOverlay_.end() && it->key == key) return it->value.c_str();
    return nullptr;
}

}  // namespace cdc::plugin_manager
