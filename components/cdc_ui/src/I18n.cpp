/**
 * \file I18n.cpp
 * \brief Translation lookup with English fallback in rodata and overlay
 *        translations loaded at runtime from a JSON file on the plugins FAT.
 */

#include "cdc_ui/I18n.h"

#include "cdc_core/Raii.h"
#include "cdc_core/Cp437.h"
#include "cdc_log.h"

#include "cJSON.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <dirent.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cdc::ui {

static const char* TAG = "I18n";

namespace {

constexpr const char* NVS_NAMESPACE = "i18n";
constexpr const char* NVS_KEY_LANG_CODE = "langc";

void* psramCjsonMalloc(std::size_t sz) {
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/// Routes cJSON allocations to PSRAM for the lifetime of the scope so a parse
/// tree never touches (or fragments) the scarce internal heap. cJSON hooks are
/// global; i18n parsing is single-threaded, so swap-and-restore is safe.
struct PsramCjsonScope {
    PsramCjsonScope() {
        cJSON_Hooks hooks{psramCjsonMalloc, std::free};
        cJSON_InitHooks(&hooks);
    }
    ~PsramCjsonScope() { cJSON_InitHooks(nullptr); }
};

/// Core firmware strings, indexed by StringId. Keys are stable
/// "core.<snake_case>" identifiers and must match assets/i18n/lang_<code>.json.
constexpr I18nEntry kCoreStrings[] = {
    {"core.main_menu",          "Main Menu"},
    {"core.settings",           "Settings"},
    {"core.hardware",           "Hardware"},
    {"core.tools",              "Tools"},
    {"core.hardware_info",      "Hardware Info"},
    {"core.name",               "Name"},
    {"core.info",               "Info"},
    {"core.info2",              "Info 2"},
    {"core.default_name",       "CDC Badge"},
    {"core.default_info",       "v" APP_VERSION},
    {"core.back",               "Back"},
    {"core.ok",                 "OK"},
    {"core.cancel",             "Cancel"},
    {"core.save",               "Save"},
    {"core.delete",             "Delete"},
    {"core.edit",               "Edit"},
    {"core.view",               "View"},
    {"core.select",             "Select"},
    {"core.open",               "Open"},
    {"core.add",                "Add"},
    {"core.new",                "New"},
    {"core.new_folder",         "New folder"},
    {"core.new_file",           "New file"},
    {"core.exists",             "Already exists"},
    {"core.not_empty",          "Not empty"},
    {"core.sure",               "Are you sure?"},
    {"core.vfat",               "vFAT"},
    {"core.yes",                "Yes"},
    {"core.no",                 "No"},
    {"core.on",                 "On"},
    {"core.off",                "Off"},
    {"core.saved",              "Saved"},
    {"core.deleted",            "Deleted"},
    {"core.failed",             "Failed"},
    {"core.timeout",            "Timeout"},
    {"core.empty",              "empty"},

    {"core.lock",               "Lock"},
    {"core.unlock",             "Unlock"},
    {"core.enter_pin",          "Enter PIN"},
    {"core.press_any_key",      "Any key: unlock  [3]: menu"},
    {"core.deep_sleep",         "Deep Sleep"},
    {"core.wrong_pin",          "Wrong PIN"},
    {"core.locked_out",         "Locked out"},
    {"core.too_many_attempts",  "Too many attempts"},

    {"core.change_pin",         "Change PIN"},
    {"core.current_pin",        "Current PIN"},
    {"core.new_pin",            "New PIN"},
    {"core.confirm_pin",        "Confirm PIN"},
    {"core.pin_changed",        "PIN changed"},
    {"core.pins_dont_match",    "PINs don't match"},
    {"core.pin_too_short",      "PIN too short"},
    {"core.pin_mismatch",       "PINs don't match"},
    {"core.retries",            "Retries"},
    {"core.error_generic",      "Error"},

    {"core.brightness",         "Brightness"},
    {"core.language",           "Language"},
    {"core.lang_name",          "English"},
    {"core.timezone",           "Timezone"},
    {"core.summer_time",        "Daylight Saving"},
    {"core.badge_text",         "Badge Text"},
    {"core.auto_sleep",         "Sleep Interval"},
    {"core.set_date",           "Set Date"},
    {"core.set_time",           "Set Time"},
    {"core.date",               "Date"},
    {"core.time",               "Time"},
    {"core.date_saved",         "Date saved"},
    {"core.time_saved",         "Time saved"},
    {"core.modules",            "Modules"},
    {"core.never",              "Never"},
    {"core.minutes",            "min"},

    {"core.wifi_menu",          "WiFi"},
    {"core.wifi_setup",         "WiFi Setup"},
    {"core.wifi_connect",       "Connect"},
    {"core.wifi_details",       "Details"},
    {"core.wifi_disconnect",    "Disconnect"},
    {"core.wifi_on",            "WiFi ON"},
    {"core.wifi_off",           "WiFi OFF"},
    {"core.wifi_scanning",      "Scanning..."},
    {"core.wifi_no_networks",   "No networks"},
    {"core.wifi_connecting",    "Connecting..."},
    {"core.wifi_connected",     "Connected!"},
    {"core.wifi_disconnected",  "Disconnected"},
    {"core.wifi_failed",        "Connection failed"},
    {"core.wifi_no_config",     "No WiFi configured"},
    {"core.wifi_password",      "Password"},
    {"core.wifi_add_manual",    "Add Manual"},
    {"core.wifi_ssid",          "SSID"},
    {"core.wifi_encryption",    "Encryption"},
    {"core.wifi_ip_mode",       "IP Mode"},
    {"core.wifi_dhcp",          "DHCP (Auto)"},
    {"core.wifi_static",        "Static IP"},
    {"core.wifi_gateway",       "Gateway"},
    {"core.wifi_netmask",       "Netmask"},
    {"core.wifi_dns",           "DNS"},
    {"core.wifi_saved_config",  "Saved Config"},
    {"core.wifi_signal",        "Signal"},
    {"core.ntp_sync",           "Sync Time"},
    {"core.ntp_syncing",        "Syncing time..."},
    {"core.ntp_success",        "Time synced!"},
    {"core.ntp_failed",         "Sync failed"},
    {"core.ntp_timeout",        "Sync timeout"},
    {"core.bluetooth",          "Bluetooth"},
    {"core.bluetooth_on",       "Bluetooth ON"},
    {"core.bluetooth_off",      "Bluetooth OFF"},
    {"core.ble_status",         "BLE Status"},
    {"core.ble_scan",           "Scan Devices"},
    {"core.ble_scanning",       "Scanning..."},
    {"core.ble_no_devices",     "No devices found"},
    {"core.ble_connected_to",   "Connected to"},
    {"core.ble_not_connected",  "Not connected"},
    {"core.ble_mac_address",    "MAC"},
    {"core.ble_signal",         "Signal"},
    {"core.ble_paired_devices", "Paired devices"},
    {"core.system_test",        "System Test"},
    {"core.tr01_cache_rebuild", "TR01 Cache Rebuild"},
    {"core.tr01_cache_cleanup", "TR01 Cache Cleanup"},
    {"core.expert",             "Expert"},
    {"core.expert_warning",     "Caution"},
    {"core.bootloader",         "Bootloader"},
    {"core.plugins",            "Plugins"},
    {"core.task_working",       "Please wait"},
    {"core.sleep",              "Sleep"},
    {"core.usb_replug_required","USB replug may be needed"},
    {"core.module_error_generic","Module error"},
    {"core.module_retry_prompt","Reload module?"},

    {"core.hw_section_memory",  "Memory"},
    {"core.hw_section_runtime", "Runtime"},
    {"core.hw_i2c_bus",         "I2C Bus"},
    {"core.hw_bq25895",         "BQ25895"},
    {"core.hw_tca9535",         "TCA9535"},
    {"core.hw_display",         "Display"},
    {"core.hw_tropic01",        "TROPIC01"},
    {"core.hw_tr01_session",    "TR01 Session"},
    {"core.hw_tr01_riscv_fw",   "TR01 RISC-V FW"},
    {"core.hw_tr01_spect_fw",   "TR01 SPECT FW"},
    {"core.hw_tr01_rmem_slot",  "TR01 R-Mem slot"},
    {"core.hw_wifi",            "WiFi"},
    {"core.hw_ble",             "BLE"},
    {"core.hw_heap",            "Heap"},
    {"core.hw_psram",           "PSRAM"},
    {"core.hw_nvs",             "NVS"},
    {"core.hw_entries",         "entries"},
    {"core.hw_battery",         "Battery"},
    {"core.hw_temp",            "Temp"},
    {"core.hw_uptime",          "Uptime"},
    {"core.hw_charging_suffix", " (chg)"},
    {"core.hw_not_available",   "n/a"},

    {"core.actions",            "Actions"},
    {"core.light",              "Light"},

    {"core.hint_back",          "[N] Back"},
    {"core.hint_select",        "[Y] Select"},
    {"core.hint_ok_back",       "[Y] OK [N] Back"},
    {"core.hint_approve_deny",  "[Y] Approve  [N] Deny"},
    {"core.hint_brightness",    "<4 6> Adjust [Y] Save"},
    {"core.hint_pin_input",     "[0-9] Input [Y] OK"},
    {"core.hint_t9_input",      "[0-9] T9 [Y] OK"},
    {"core.t9_full",            "Full"},
    {"core.hint_list_menu",     "[3] Menu"},
    {"core.hint_scroll_back",   "[2/8] Scroll [N] Back"},
    {"core.hint_field_nav",     "[4] <  [6] >"},
    {"core.hint_date_input",    "[0-9] [Y] OK [N] Clear"},
    {"core.hint_time_input",    "[0-9] [Y] OK [N] Clear"},

    {"core.wifi_connecting",    "Connecting to"},
    {"core.plugin_loading",     "Loading"},
    {"core.stop",               "Stop"},
    {"core.start",              "Start"},
    {"core.plugin_bg_running",  "Runs in background"},
    {"core.plugin_not_running", "Not running"},
    {"core.hint_plugin_list",   "[Y] Start [3] Menu [N] Back"},
    {"core.hint_password_hidden",   "[hold Y] Show [Y] Save"},
    {"core.hint_password_revealed", "[hold Y] Hide [Y] Save"},

    {"core.qr_error",           "QR Error"},
    {"core.no_data",            "No data"},
};

constexpr std::size_t kCoreCount =
    sizeof(kCoreStrings) / sizeof(kCoreStrings[0]);

}  // namespace

I18n::I18n() = default;

I18n& I18n::instance()
{
    static I18n s;
    return s;
}

void I18n::registerCoreEnglishTable()
{
    registerEnglishTable(kCoreStrings, kCoreCount);
}

bool I18n::init()
{
    registerCoreEnglishTable();
    loadLanguageFromNvs();
    LOG_I(TAG, "I18n initialized, lang=%s, core=%u entries",
          currentLang_.c_str(), static_cast<unsigned>(kCoreCount));
    return true;
}

void I18n::registerEnglishTable(const I18nEntry* entries, std::size_t count)
{
    if (!entries || count == 0) return;
    en_.reserve(en_.size() + count);
    en_.insert(en_.end(), entries, entries + count);
    enSorted_ = false;
}

bool I18n::sortIfNeeded() const
{
    if (enSorted_) return true;
    std::sort(en_.begin(), en_.end(),
              [](const I18nEntry& a, const I18nEntry& b) {
                  return std::strcmp(a.key, b.key) < 0;
              });
    enSorted_ = true;
    return true;
}

const char* I18n::enLookup(const char* key) const
{
    sortIfNeeded();
    I18nEntry probe{key, nullptr};
    auto it = std::lower_bound(en_.begin(), en_.end(), probe,
                                [](const I18nEntry& a, const I18nEntry& b) {
                                    return std::strcmp(a.key, b.key) < 0;
                                });
    if (it != en_.end() && std::strcmp(it->key, key) == 0) return it->en;
    return nullptr;
}

const char* I18n::overlayLookup(const char* key) const
{
    if (overlayCount_ == 0 || !overlayRefs_) return nullptr;
    const OverlayRef* base = overlayRefs_.get();
    std::size_t lo = 0, hi = overlayCount_;
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        int c = std::strcmp(base[mid].key, key);
        if (c == 0) return base[mid].value;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

const char* I18n::overlayTr(const char* key) const
{
    if (!key || currentLang_ == "en") return nullptr;
    return overlayLookup(key);
}

const char* I18n::tr(const char* key) const
{
    if (!key) return "";
    if (currentLang_ != "en") {
        if (const char* s = overlayLookup(key)) return s;
    }
    if (const char* s = enLookup(key)) return s;
    static thread_local char missing[64];
    std::snprintf(missing, sizeof(missing), "?%s", key);
    return missing;
}

bool I18n::setLanguageCode(const char* code)
{
    std::string newLang = (code && *code) ? code : "en";
    if (newLang == currentLang_) return true;
    currentLang_ = std::move(newLang);

    overlayBlob_.reset();
    overlayRefs_.reset();
    overlayCount_ = 0;
    if (currentLang_ != "en") loadActiveOverlayFile();
    if (onChanged_) onChanged_();

    saveLanguageToNvs();
    LOG_I(TAG, "Language changed to %s", currentLang_.c_str());
    return true;
}

void I18n::scanAvailableLanguages()
{
    overlayLangs_.clear();
    DIR* dir = opendir(OVERLAY_DIR);
    if (!dir) return;

    constexpr const char* kPrefix = "lang_";
    constexpr size_t kPrefixLen = 5;   // strlen("lang_")
    constexpr size_t kSuffixLen = 5;   // strlen(".json")

    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        const char* n = ent->d_name;
        const size_t len = std::strlen(n);
        if (len <= kPrefixLen + kSuffixLen) continue;
        if (std::strncmp(n, kPrefix, kPrefixLen) != 0) continue;
        if (std::strcmp(n + len - kSuffixLen, ".json") != 0) continue;

        std::string code(n + kPrefixLen, len - kPrefixLen - kSuffixLen);
        if (code.empty() || code == "en") continue;   // English is in-code

        // Default the display name to the code, then try to read the file's
        // own `core.lang_name` endonym.
        OverlayLanguage lang{code, code};
        const std::string path = std::string(OVERLAY_DIR) + "/" + n;
        if (auto fp = cdc::core::openFile(path.c_str(), "rb")) {
            std::fseek(fp.get(), 0, SEEK_END);
            const long size = std::ftell(fp.get());
            std::fseek(fp.get(), 0, SEEK_SET);
            if (size > 0 && size <= 1024 * 1024) {
                auto buf = cdc::core::psramAlloc<char>(static_cast<std::size_t>(size) + 1);
                if (buf && std::fread(buf.get(), 1, size, fp.get()) == static_cast<size_t>(size)) {
                    buf.get()[size] = '\0';
                    PsramCjsonScope cjson_psram;
                    if (cJSON* root = cJSON_Parse(buf.get())) {
                        cJSON* nm = cJSON_GetObjectItemCaseSensitive(root, "core.lang_name");
                        if (cJSON_IsString(nm) && nm->valuestring && *nm->valuestring) {
                            lang.name = cdc::core::cp437::fromUtf8(nm->valuestring);
                        }
                        cJSON_Delete(root);
                    }
                }
            }
        }
        overlayLangs_.push_back(std::move(lang));
    }
    closedir(dir);

    std::sort(overlayLangs_.begin(), overlayLangs_.end(),
              [](const OverlayLanguage& a, const OverlayLanguage& b) {
                  return a.code < b.code;
              });
}

bool I18n::loadActiveOverlayFile()
{
    overlayBlob_.reset();
    overlayRefs_.reset();
    overlayCount_ = 0;

    const std::string path =
        std::string(OVERLAY_DIR) + "/lang_" + currentLang_ + ".json";

    auto fp = cdc::core::openFile(path.c_str(), "rb");
    if (!fp) {
        LOG_W(TAG, "Overlay file not found: %s", path.c_str());
        return false;
    }

    std::fseek(fp.get(), 0, SEEK_END);
    long size = std::ftell(fp.get());
    std::fseek(fp.get(), 0, SEEK_SET);
    if (size <= 0 || size > 1024 * 1024) {
        LOG_W(TAG, "Overlay file size invalid: %ld", size);
        return false;
    }

    auto buf = cdc::core::psramAlloc<char>(static_cast<std::size_t>(size) + 1);
    if (!buf) {
        LOG_E(TAG, "Overlay PSRAM allocation failed (%ld bytes)", size);
        return false;
    }
    if (std::fread(buf.get(), 1, size, fp.get()) != static_cast<size_t>(size)) {
        LOG_E(TAG, "Overlay file read failed");
        return false;
    }
    buf.get()[size] = '\0';

    // Parse tree + final storage both live in PSRAM.
    PsramCjsonScope cjson_psram;
    cJSON* root = cJSON_Parse(buf.get());
    if (!root || !cJSON_IsObject(root)) {
        LOG_E(TAG, "Overlay JSON parse failed near: %s",
              cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "<unknown>");
        if (root) cJSON_Delete(root);
        return false;
    }

    // Pass 1: count string entries and the bytes needed for the packed blob
    // (key + CP437-converted value, each null-terminated).
    std::size_t count = 0;
    std::size_t bytes = 0;
    for (cJSON* e = root->child; e; e = e->next) {
        if (!cJSON_IsString(e) || !e->string || !e->valuestring) continue;
        ++count;
        bytes += std::strlen(e->string) + 1;
        bytes += cdc::core::cp437::fromUtf8(e->valuestring).size() + 1;
    }
    if (count == 0) { cJSON_Delete(root); return true; }

    auto blob = cdc::core::psramAlloc<char>(bytes);
    auto refs = cdc::core::psramAlloc<OverlayRef>(count);
    if (!blob || !refs) {
        LOG_E(TAG, "Overlay storage PSRAM allocation failed");
        cJSON_Delete(root);
        return false;
    }

    // Pass 2: pack into the PSRAM blob and record key/value pointers.
    char* w = blob.get();
    std::size_t idx = 0;
    for (cJSON* e = root->child; e; e = e->next) {
        if (!cJSON_IsString(e) || !e->string || !e->valuestring) continue;
        const std::string val = cdc::core::cp437::fromUtf8(e->valuestring);
        const std::size_t kl = std::strlen(e->string);
        refs.get()[idx].key = w;
        std::memcpy(w, e->string, kl + 1);
        w += kl + 1;
        refs.get()[idx].value = w;
        std::memcpy(w, val.c_str(), val.size() + 1);
        w += val.size() + 1;
        ++idx;
    }
    cJSON_Delete(root);

    std::sort(refs.get(), refs.get() + count,
              [](const OverlayRef& a, const OverlayRef& b) {
                  return std::strcmp(a.key, b.key) < 0;
              });

    overlayBlob_  = std::move(blob);
    overlayRefs_  = std::move(refs);
    overlayCount_ = count;

    LOG_I(TAG, "Overlay '%s' loaded: %u entries (PSRAM)",
          currentLang_.c_str(), static_cast<unsigned>(count));
    return true;
}

bool I18n::loadOverlay()
{
    scanAvailableLanguages();

    bool ok = true;
    if (currentLang_ != "en") {
        ok = loadActiveOverlayFile();
    } else {
        overlayBlob_.reset();
        overlayRefs_.reset();
        overlayCount_ = 0;
    }

    LOG_I(TAG, "i18n overlay: %u languages available, active='%s'",
          static_cast<unsigned>(overlayLangs_.size()),
          currentLang_.c_str());

    if (onChanged_) onChanged_();
    return ok;
}

const char* I18n::languageName(const char* code) const
{
    if (!code || !*code || std::strcmp(code, "en") == 0) {
        const char* en = enLookup("core.lang_name");
        return en ? en : "English";
    }
    for (const auto& l : overlayLangs_) {
        if (l.code == code) return l.name.c_str();
    }
    return code;
}

void I18n::loadLanguageFromNvs()
{
    cdc::core::NvsScope nvs(NVS_NAMESPACE, NVS_READONLY);
    if (!nvs) return;
    size_t len = 0;
    if (nvs_get_str(nvs, NVS_KEY_LANG_CODE, nullptr, &len) != ESP_OK || len == 0) return;
    if (len > 8) len = 8;
    char buf[9] = {};
    if (nvs_get_str(nvs, NVS_KEY_LANG_CODE, buf, &len) == ESP_OK) {
        currentLang_ = buf;
    }
}

void I18n::saveLanguageToNvs()
{
    cdc::core::NvsScope nvs(NVS_NAMESPACE, NVS_READWRITE);
    if (!nvs) return;
    nvs_set_str(nvs, NVS_KEY_LANG_CODE, currentLang_.c_str());
    nvs.commit();
}

}  // namespace cdc::ui
