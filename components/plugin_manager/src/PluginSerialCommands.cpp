/**
 * \file PluginSerialCommands.cpp
 * \brief Serial console commands for managing installed plugins:
 *        LIST, INFO, START, STOP, DELETE, UPLOAD.
 *
 * Upload uses a chunked protocol with CRC32 per chunk and ACK/NACK retries.
 * While an upload session is active, a line interceptor steals all serial
 * input from the normal command parser until END or ABORT is received.
 *
 *   > PLUGIN UPLOAD <id> <total_size> wasm|meta
 *   < READY
 *   > <chunk_idx> <crc32> <hex_bytes>
 *   < ACK <chunk_idx>      |   NACK <chunk_idx> <reason>
 *   ... repeat ...
 *   > END
 *   < OK <total_bytes>
 */

#include "plugin_manager/PluginManager.h"
#include "plugin_manager/PluginStorage.h"
#include "plugin_manager/PluginManifest.h"
#include "serial_cmd/ICommandRegistry.h"
#include "serial_cmd/SubCommand.h"
#include "serial_cmd/Console.h"
#include "serial_cmd/SerialCmd.h"
#include "cdc_core/Raii.h"
#include "cdc_ui/I18n.h"
#include "cdc_log.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>

namespace cdc::plugin_manager {

static const char* TAG = "PLG_CMD";
static const char* CMD_MODULE = "plugin_manager";

namespace {

struct UploadSession {
    bool                 active = false;
    std::string          id;
    std::string          target_path;   // .wasm or .meta or .lang
    std::string          tmp_path;      // <target_path>.partial
    size_t               total_size = 0;
    size_t               received   = 0;
    uint32_t             expected_crc = 0;
    uint32_t             running_crc  = 0xffffffffu;
    bool                 was_lang = false;
    bool                 was_wasm = false;
    ::cdc::core::FilePtr fp;
    int64_t              last_activity_us = 0;
};

static UploadSession s_upload{};

// CRC-32 (IEEE 802.3) byte-wise update so we can checksum the stream as it
// arrives in the ISR/CDC callback, without buffering the entire payload.
static inline uint32_t crc32_update(uint32_t crc, uint8_t b) {
    crc ^= b;
    for (int i = 0; i < 8; ++i) {
        crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return crc;
}

// Auto-abort a stuck upload after this many seconds of inactivity. Without
// this, a crashed/interrupted client would keep the line interceptor active
// and the serial console permanently unresponsive.
static constexpr int64_t UPLOAD_INACTIVITY_LIMIT_US = 15 * 1000000LL;

static void touch_upload_activity() {
    s_upload.last_activity_us = esp_timer_get_time();
}

void send(const char* line) {
    ::cdc::serial::Console::print(line);
    ::cdc::serial::Console::print("\r\n");
}
void sendf(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ::cdc::serial::Console::print(buf);
    ::cdc::serial::Console::print("\r\n");
}

static esp_timer_handle_t s_upload_timeout_timer = nullptr;
static void on_upload_timeout(void*);

// 256-byte page-aligned write buffer for binary upload streaming.
// `s_byte_buffer_pos` is reset to 0 by abort_upload() and start_upload().
static uint8_t s_byte_buffer[256];
static size_t  s_byte_buffer_pos = 0;

static void ensure_timeout_timer() {
    if (s_upload_timeout_timer) return;
    esp_timer_create_args_t args = {
        .callback = on_upload_timeout,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "plugin_upload_to",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &s_upload_timeout_timer);
}

static void rearm_upload_timeout() {
    ensure_timeout_timer();
    esp_timer_stop(s_upload_timeout_timer);
    esp_timer_start_once(s_upload_timeout_timer, UPLOAD_INACTIVITY_LIMIT_US);
}

void abort_upload()
{
    s_upload.fp.reset();
    if (!s_upload.tmp_path.empty()) std::remove(s_upload.tmp_path.c_str());
    s_upload = UploadSession{};
    s_byte_buffer_pos = 0;
    cdc::serial::getCommandRegistry().setByteInterceptor(nullptr);
    if (s_upload_timeout_timer) esp_timer_stop(s_upload_timeout_timer);
}

static void on_upload_timeout(void*) {
    if (!s_upload.active) return;
    int64_t since = esp_timer_get_time() - s_upload.last_activity_us;
    if (since >= UPLOAD_INACTIVITY_LIMIT_US) {
        LOG_W(TAG, "Upload session timed out after %lld us, aborting", since);
        send("ERR upload_timeout");
        abort_upload();
    } else {
        esp_timer_start_once(s_upload_timeout_timer,
                             UPLOAD_INACTIVITY_LIMIT_US - since);
    }
}

// Byte-streaming finaliser - called after `total_size` payload bytes have
// been received. Verifies the whole-stream CRC, renames the partial file in
// place and clears all upload state.
static void finalize_upload()
{
    s_upload.fp.reset();

    if (s_upload.received != s_upload.total_size) {
        sendf("ERR size_mismatch %u/%u",
              static_cast<unsigned>(s_upload.received),
              static_cast<unsigned>(s_upload.total_size));
        abort_upload();
        return;
    }

    const uint32_t actual_crc = s_upload.running_crc ^ 0xffffffffu;
    if (actual_crc != s_upload.expected_crc) {
        sendf("ERR crc_mismatch got=%08X want=%08X",
              static_cast<unsigned>(actual_crc),
              static_cast<unsigned>(s_upload.expected_crc));
        abort_upload();
        return;
    }

    // FAT-FS rejects rename when the target exists, so drop any previous
    // version first.
    std::remove(s_upload.target_path.c_str());
    if (std::rename(s_upload.tmp_path.c_str(), s_upload.target_path.c_str()) != 0) {
        send("ERR rename_failed");
        abort_upload();
        return;
    }

    const bool was_lang = s_upload.was_lang;
    const bool was_wasm = s_upload.was_wasm;
    const std::string uploaded_id = s_upload.id;
    sendf("OK %u", static_cast<unsigned>(s_upload.total_size));

    s_upload = UploadSession{};
    if (s_upload_timeout_timer) esp_timer_stop(s_upload_timeout_timer);

    if (was_lang) {
        cdc::ui::I18n::instance().loadOverlay();
    }
    if (was_wasm && !uploaded_id.empty()) {
        auto mf = PluginManager::instance().getManifest(uploaded_id);
        if (mf && mf->capabilities.background) {
            PluginManager::instance().reloadBackgroundPlugin(uploaded_id);
        }
    }
}

// Byte interceptor: receives every byte from the serial CDC stream while the
// upload session is active. Buffers in 256-byte page-aligned blocks before
// calling fwrite() to keep filesystem syscalls cheap on FAT.

static inline void flush_byte_buffer()
{
    if (s_byte_buffer_pos == 0) return;
    if (s_upload.fp) {
        std::fwrite(s_byte_buffer, 1, s_byte_buffer_pos, s_upload.fp.get());
    }
    s_byte_buffer_pos = 0;
}

extern "C" void upload_byte(uint8_t b)
{
    if (!s_upload.active) return;

    // Keep auth session and inactivity watchdog alive without forcing the
    // host to send periodic commands.
    cdc::serial::SerialCmd::touchAuthSession();
    touch_upload_activity();

    s_byte_buffer[s_byte_buffer_pos++] = b;
    s_upload.running_crc = crc32_update(s_upload.running_crc, b);
    s_upload.received++;

    if (s_byte_buffer_pos >= sizeof(s_byte_buffer)) {
        flush_byte_buffer();
        rearm_upload_timeout();
    }

    if (s_upload.received >= s_upload.total_size) {
        flush_byte_buffer();
        cdc::serial::getCommandRegistry().setByteInterceptor(nullptr);
        finalize_upload();
    }
}

void cmdList(const char*)
{
    auto ids = PluginManager::instance().listInstalledIds();
    send("[");
    for (size_t i = 0; i < ids.size(); ++i) {
        std::string name = ids[i];
        std::string version;
        if (auto mf = PluginManager::instance().getManifest(ids[i])) {
            version = mf->version;
            auto it = mf->i18n_meta.find("name");
            if (it != mf->i18n_meta.end() && !it->second.by_lang.empty()) {
                auto by = it->second.by_lang.find(mf->default_language);
                if (by != it->second.by_lang.end()) name = by->second;
                else                                name = it->second.by_lang.begin()->second;
            }
        }
        sendf("  {\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\"}%s",
              ids[i].c_str(), name.c_str(), version.c_str(),
              (i + 1 < ids.size()) ? "," : "");
    }
    send("]");
}

void cmdInfo(const char* args)
{
    if (!args || !*args) { send("ERR missing_id"); return; }
    std::string id = args;
    auto mf = PluginManager::instance().getManifest(id);
    if (!mf) { send("ERR not_found"); return; }
    sendf("id:           %s", mf->id.c_str());
    sendf("version:      %s", mf->version.c_str());
    sendf("author:       %s", mf->author.c_str());
    sendf("api_level:    %s", mf->host_api_level_min.c_str());
    sendf("linear_kb:    %u", static_cast<unsigned>(mf->linear_memory_kb));

    const auto& c = mf->capabilities;

    // Boolean capabilities: list only the requested (true) ones.
    std::string caps;
    auto addCap = [&caps](bool on, const char* name) {
        if (on) { if (!caps.empty()) caps += ' '; caps += name; }
    };
    addCap(c.wifi, "wifi");
    addCap(c.ble, "ble");
    addCap(c.http, "http");
    addCap(c.ui_exclusive, "ui_exclusive");
    addCap(c.display_lowlevel, "display_lowlevel");
    addCap(c.sao, "sao");
    addCap(c.grove, "grove");
    addCap(c.pixel_strip, "pixel_strip");
    addCap(c.background, "background");
    addCap(c.usb_cdc, "usb_cdc");
    addCap(c.prevent_sleep, "prevent_sleep");
    sendf("caps:         %s", caps.empty() ? "-" : caps.c_str());

    auto joinPins = [](const std::vector<uint8_t>& v) {
        std::string s;
        char num[8];
        for (uint8_t p : v) {
            if (!s.empty()) s += ',';
            snprintf(num, sizeof(num), "%u", static_cast<unsigned>(p));
            s += num;
        }
        return s;
    };
    auto joinStr = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& e : v) { if (!s.empty()) s += ','; s += e; }
        return s;
    };

    // Concrete resource requests (printed only when present).
    if (!c.gpio_pins.empty())         sendf("gpio_pins:    %s", joinPins(c.gpio_pins).c_str());
    if (!c.pwm_pins.empty())          sendf("pwm_pins:     %s", joinPins(c.pwm_pins).c_str());
    if (!c.adc_pins.empty())          sendf("adc_pins:     %s", joinPins(c.adc_pins).c_str());
    if (!c.i2c_bus.empty())           sendf("i2c_bus:      %s", joinPins(c.i2c_bus).c_str());
    if (!c.rmem.empty())              sendf("rmem:         %s", joinStr(c.rmem).c_str());
    if (!c.ecc.empty())               sendf("ecc:          %s", joinStr(c.ecc).c_str());
    if (!c.ble_service_uuids.empty()) sendf("ble_uuids:    %s", joinStr(c.ble_service_uuids).c_str());
    if (!c.nvs_namespace.empty())     sendf("nvs_ns:       %s", c.nvs_namespace.c_str());

    // Prerequisites: name + on-fail policy.
    sendf("prereqs:      %u", static_cast<unsigned>(mf->prerequisites.size()));
    for (const auto& p : mf->prerequisites) {
        if (p.on_fail.empty()) {
            sendf("  - %s", p.name.c_str());
        } else {
            sendf("  - %s (on_fail=%s)", p.name.c_str(), p.on_fail.c_str());
        }
    }
}

void cmdDelete(const char* args)
{
    if (!args || !*args) { send("ERR missing_id"); return; }
    std::string id = args;
    if (PluginManager::instance().hasActivePlugin() &&
        PluginManager::instance().activePluginId() == id) {
        PluginManager::instance().stopActivePlugin();
    }
    std::remove(PluginStorage::wasmPath(id).c_str());
    std::remove(PluginStorage::aotPath(id).c_str());
    std::remove(PluginStorage::metaPath(id).c_str());
    std::remove(PluginStorage::langPath(id).c_str());
    send("OK");
}

void cmdStart(const char* args)
{
    if (!args || !*args) { send("ERR missing_id"); return; }
    auto res = PluginManager::instance().startPlugin(args);
    if (res == StartResult::Ok) {
        sendf("OK started %s", args);
    } else {
        sendf("ERR start %d %s", static_cast<int>(res), args);
    }
}

void cmdStop(const char*)
{
    if (PluginManager::instance().stopActivePlugin()) send("OK");
    else send("ERR no_active_plugin");
}

void cmdCmd(const char* args)
{
    if (!args || !*args) { send("ERR missing_id"); return; }

    char id_buf[64] = {0};
    int  consumed = 0;
    std::sscanf(args, "%63s%n", id_buf, &consumed);
    if (id_buf[0] == '\0') { send("ERR missing_id"); return; }

    const char* cmd = args + consumed;
    while (*cmd == ' ') ++cmd;
    std::string id = id_buf;

    bool started_here = false;
    if (!PluginManager::instance().isLoaded(id)) {
        auto res = PluginManager::instance().startPlugin(id);
        if (res != StartResult::Ok && res != StartResult::PluginAlreadyRunning) {
            sendf("ERR start %d %s", static_cast<int>(res), id.c_str());
            return;
        }
        started_here = true;
    }

    if (PluginManager::instance().dispatchCmd(id, cmd, std::strlen(cmd))) send("OK");
    else send("ERR no_handler");

    // Only unload what this command loaded; a plugin already running stays running.
    if (started_here) PluginManager::instance().unloadFromRam(id);
}

enum class PluginUploadKind { Wasm, Aot, Meta, Lang };

// Shared by every upload path (plugin wasm/meta/lang, core lang overlay,
// generic file): the caller fills s_upload.target_path + the post-finalize
// flags, then this opens the .partial temp, resets the byte counters, installs
// the streaming interceptor and answers READY. It is all one vFAT file write.
void arm_upload(uint32_t crc)
{
    s_upload.received     = 0;
    s_upload.running_crc  = 0xffffffffu;
    s_upload.expected_crc = crc;
    s_upload.tmp_path     = s_upload.target_path + ".partial";
    s_upload.fp = ::cdc::core::openFile(s_upload.tmp_path.c_str(), "wb");
    if (!s_upload.fp) { send("ERR cannot_open"); return; }
    s_byte_buffer_pos = 0;
    s_upload.active = true;
    touch_upload_activity();
    rearm_upload_timeout();
    cdc::serial::getCommandRegistry().setByteInterceptor(upload_byte);
    send("READY");
}

// Parses "<id> <total_size> <crc32_hex>". Derives the target path from the
// kind, then arms the shared upload.
void start_upload(const char* args, PluginUploadKind kind)
{
    if (s_upload.active) { send("ERR upload_in_progress"); return; }

    char id_buf[64] = {0};
    unsigned long total_size = 0;
    unsigned long crc_arg = 0;
    int parsed = std::sscanf(args, "%63s %lu %lx", id_buf, &total_size, &crc_arg);
    if (parsed < 2 || total_size == 0) {
        send("ERR usage:_PLUGIN_UPLOAD_<id>_<size>_<crc32_hex>");
        return;
    }

    if (kind == PluginUploadKind::Wasm || kind == PluginUploadKind::Aot) {
        const std::string target_id = id_buf;
        if (PluginManager::instance().hasActivePlugin() &&
            PluginManager::instance().activePluginId() == target_id) {
            PluginManager::instance().unloadFromRam(target_id);
        }
    }

    s_upload.id       = id_buf;
    s_upload.total_size = static_cast<size_t>(total_size);
    s_upload.was_lang = (kind == PluginUploadKind::Lang);
    s_upload.was_wasm = (kind == PluginUploadKind::Wasm || kind == PluginUploadKind::Aot);
    switch (kind) {
        case PluginUploadKind::Wasm:
            s_upload.target_path = PluginStorage::wasmPath(s_upload.id);
            std::remove(PluginStorage::aotPath(s_upload.id).c_str());
            break;
        case PluginUploadKind::Aot:
            s_upload.target_path = PluginStorage::aotPath(s_upload.id);
            std::remove(PluginStorage::wasmPath(s_upload.id).c_str());
            break;
        case PluginUploadKind::Meta:
            s_upload.target_path = PluginStorage::metaPath(s_upload.id);
            break;
        case PluginUploadKind::Lang:
            s_upload.target_path = PluginStorage::langPath(s_upload.id);
            break;
    }

    arm_upload(static_cast<uint32_t>(crc_arg));
}

void cmdUpload    (const char* args) { start_upload(args, PluginUploadKind::Wasm); }
void cmdUploadAot (const char* args) { start_upload(args, PluginUploadKind::Aot);  }
void cmdUploadMeta(const char* args) { start_upload(args, PluginUploadKind::Meta); }
void cmdUploadLang(const char* args) { start_upload(args, PluginUploadKind::Lang); }

void cmdAbort(const char*)
{
    if (!s_upload.active) { send("OK no_upload"); return; }
    abort_upload();
    send("ABORTED");
}

void cmdDebug(const char*)
{
    static bool s_debug_enabled = false;
    s_debug_enabled = !s_debug_enabled;
    auto level = s_debug_enabled ? ESP_LOG_DEBUG : ESP_LOG_INFO;
    static const char* const PLUGIN_TAGS[] = {
        "PLG_CMD", "PLG_MGR", "PLG_STO", "PLG_UI", "PLG_PRE", "PLG_MAN",
        "PLUGIN", "GPIO_CMD", "WamrImports",
    };
    for (auto* t : PLUGIN_TAGS) esp_log_level_set(t, level);
    esp_log_level_set("host_*", level);

    if (!s_debug_enabled) {
        send("OK plugin debug DISABLED");
        return;
    }

    sendf("OK plugin debug ENABLED");
    auto& pm = PluginManager::instance();
    sendf("--- diagnostic snapshot ---");
    sendf("active_plugin:    %s",
          pm.hasActivePlugin() ? pm.activePluginId().c_str() : "(none)");
    sendf("installed_count:  %u",
          static_cast<unsigned>(pm.listInstalledIds().size()));
    sendf("psram_free:       %u KB",
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    sendf("internal_free:    %u KB",
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    sendf("upload_active:    %s", s_upload.active ? "yes" : "no");
}

void cmdLangInfo(const char*)
{
    auto& i18n = cdc::ui::I18n::instance();
    sendf("lang:    %s", i18n.getLanguageCode().c_str());
    sendf("dir:     %s", cdc::ui::I18n::OVERLAY_DIR);
    sendf("avail:   %u", static_cast<unsigned>(i18n.availableOverlayLanguages().size()));
    for (const auto& c : i18n.availableOverlayLanguages()) {
        sendf("  - %s", c.code.c_str());
    }
}

void cmdLangReload(const char*)
{
    if (cdc::ui::I18n::instance().loadOverlay()) send("OK");
    else send("ERR reload_failed");
}

const cdc::serial::SubCommand kPluginSubs[] = {
    {"LIST",        "",                              "List installed plugins (JSON)",                     cmdList},
    {"INFO",        "<id>",                          "Show manifest details for one plugin",              cmdInfo},
    {"START",       "<id>",                          "Start a plugin",                                    cmdStart},
    {"STOP",        "",                              "Stop the currently active plugin",                  cmdStop},
    {"CMD",         "<id> <args>",                   "Forward a command string to a plugin",              cmdCmd},
    {"DELETE",      "<id>",                          "Delete wasm + meta + lang files for plugin",        cmdDelete},
    {"UPLOAD",      "<id> <size> <crc32_hex>",       "Upload .wasm payload (binary stream)",              cmdUpload},
    {"UPLOAD_AOT",  "<id> <size> <crc32_hex>",       "Upload .aot payload (binary stream)",               cmdUploadAot},
    {"UPLOAD_META", "<id> <size> <crc32_hex>",       "Upload .meta payload (binary stream)",              cmdUploadMeta},
    {"UPLOAD_LANG", "<id> <size> <crc32_hex>",       "Upload .lang payload (binary stream)",              cmdUploadLang},
    {"ABORT",       "",                              "Abort an active upload session",                    cmdAbort},
    {"DEBUG",       "",                              "Toggle verbose plugin/host_* logging",              cmdDebug},
    {nullptr, nullptr, nullptr, nullptr},
};
void cmdPluginDispatch(const char* args) {
    cdc::serial::dispatchSubCommand("PLUGIN", args, kPluginSubs);
}

const cdc::serial::SubCommand kLangSubs[] = {
    {"INFO",   "",       "Show active language and available overlays",     cmdLangInfo},
    {"RELOAD", "",       "Rescan + reload overlays from /plugins/i18n/",    cmdLangReload},
    {nullptr, nullptr, nullptr, nullptr},
};
void cmdLangDispatch(const char* args) {
    cdc::serial::dispatchSubCommand("LANG", args, kLangSubs);
}

}  // namespace

bool beginFileReceive(const char* abs_path, size_t size, uint32_t crc)
{
    if (!abs_path || size == 0) { send("ERR bad_args"); return false; }
    if (s_upload.active)        { send("ERR upload_in_progress"); return false; }
    s_upload.id          = "";
    s_upload.total_size  = size;
    s_upload.was_lang    = false;
    s_upload.was_wasm    = false;
    s_upload.target_path = abs_path;
    arm_upload(crc);
    return s_upload.active;
}

void registerPluginSerialCommands()
{
    auto& reg = cdc::serial::getCommandRegistry();
    reg.registerCommand({"PLUGIN",
                         "Plugin manager: LIST/INFO/START/STOP/DELETE/UPLOAD/UPLOAD_META/UPLOAD_LANG/ABORT/DEBUG",
                         cmdPluginDispatch, CMD_MODULE, true, kPluginSubs});
    reg.registerCommand({"LANG",
                         "i18n overlay: INFO/RELOAD",
                         cmdLangDispatch, CMD_MODULE, true, kLangSubs});
    LOG_I(TAG, "PLUGIN and LANG serial commands registered");
}

}  // namespace cdc::plugin_manager
