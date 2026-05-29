/**
 * \file I18n.cpp
 * \brief Translation lookup with English fallback in rodata and overlay
 *        translations loaded at runtime from a JSON file on the plugins FAT.
 */

#include "cdc_ui/I18n.h"

#include "cdc_core/Raii.h"
#include "cdc_log.h"

#include "cJSON.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace cdc::ui {

static const char* TAG = "I18n";

namespace {

constexpr const char* NVS_NAMESPACE = "i18n";
constexpr const char* NVS_KEY_LANG_CODE = "langc";

/**
 * \brief Maps a Unicode codepoint to its CP437 byte for the display font.
 *        Covers the Western-European set overlay languages use (German umlauts
 *        plus common accents). ASCII passes through; unmapped codepoints return
 *        0 (dropped). Canonical/complete map: `unicodeToCp437` in
 *        cdc_views/RenderHelpers; cdc_ui cannot depend on cdc_views, so this is
 *        a focused copy.
 */
uint8_t uniToCp437(uint32_t cp) {
    switch (cp) {
        case 0x00C4: return 0x8E; case 0x00D6: return 0x99;  // Ae Oe
        case 0x00DC: return 0x9A; case 0x00E4: return 0x84;  // Ue ae
        case 0x00F6: return 0x94; case 0x00FC: return 0x81;  // oe ue
        case 0x00DF: return 0xE1;                            // ss
        case 0x00E9: return 0x82; case 0x00E8: return 0x8A;  // e-acute e-grave
        case 0x00E0: return 0x85; case 0x00E2: return 0x83;  // a-grave a-circ
        case 0x00E7: return 0x87; case 0x00EA: return 0x88;  // c-cedilla e-circ
        case 0x00EE: return 0x8C; case 0x00F4: return 0x93;  // i-circ o-circ
        case 0x00FB: return 0x96; case 0x00F1: return 0xA4;  // u-circ n-tilde
        case 0x00D1: return 0xA5;                            // N-tilde
        default: return (cp < 0x80) ? static_cast<uint8_t>(cp) : 0;
    }
}

/**
 * \brief Converts a UTF-8 string (as stored in lang.json) to the CP437 bytes
 *        the display pipeline expects. Invalid/unmapped sequences are skipped.
 */
std::string utf8ToCp437(const char* s) {
    std::string out;
    if (!s) return out;
    const uint8_t* r = reinterpret_cast<const uint8_t*>(s);
    while (*r) {
        uint8_t c = *r;
        uint32_t cp = 0;
        uint8_t cont = 0;
        if ((c & 0x80) == 0) { out.push_back(static_cast<char>(c)); ++r; continue; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; cont = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; cont = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; cont = 3; }
        else { ++r; continue; }
        ++r;
        bool ok = true;
        for (uint8_t i = 0; i < cont; ++i) {
            if ((*r & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (*r & 0x3F);
            ++r;
        }
        if (!ok) continue;
        uint8_t mapped = uniToCp437(cp);
        if (mapped) out.push_back(static_cast<char>(mapped));
    }
    return out;
}

/// Core firmware strings, indexed by StringId. Keys are stable
/// "core.<snake_case>" identifiers and must match assets/i18n/lang.json.
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
    if (activeOverlay_.empty()) return nullptr;
    auto it = std::lower_bound(
        activeOverlay_.begin(), activeOverlay_.end(), key,
        [](const OverlayEntry& e, const char* k) { return e.key < k; });
    if (it != activeOverlay_.end() && it->key == key) return it->value.c_str();
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

    if (currentLang_ != "en") {
        activeOverlay_.clear();
        const char* path = overlayJsonPath_.empty()
                               ? DEFAULT_OVERLAY_PATH
                               : overlayJsonPath_.c_str();
        // loadOverlay() fires onChanged_ on success.
        loadOverlay(path);
    } else {
        activeOverlay_.clear();
        if (onChanged_) onChanged_();
    }

    saveLanguageToNvs();
    LOG_I(TAG, "Language changed to %s", currentLang_.c_str());
    return true;
}

bool I18n::loadOverlay(const char* path)
{
    if (!path) return false;
    overlayJsonPath_ = path;

    auto fp = cdc::core::openFile(path, "rb");
    if (!fp) {
        LOG_W(TAG, "Overlay file not found: %s", path);
        overlayLangs_.clear();
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

    cJSON* root = cJSON_Parse(buf.get());
    if (!root) {
        LOG_E(TAG, "Overlay JSON parse failed near: %s",
              cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "<unknown>");
        return false;
    }

    cJSON* translations = cJSON_GetObjectItemCaseSensitive(root, "translations");
    if (!translations || !cJSON_IsObject(translations)) {
        LOG_E(TAG, "Overlay missing 'translations' object");
        cJSON_Delete(root);
        return false;
    }

    overlayLangs_.clear();
    activeOverlay_.clear();

    cJSON* lang_obj = nullptr;
    cJSON_ArrayForEach(lang_obj, translations) {
        if (!cJSON_IsObject(lang_obj) || !lang_obj->string) continue;
        overlayLangs_.emplace_back(lang_obj->string);

        if (currentLang_ != lang_obj->string) continue;

        cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, lang_obj) {
            if (!cJSON_IsString(entry) || !entry->string || !entry->valuestring) continue;
            activeOverlay_.push_back({entry->string, utf8ToCp437(entry->valuestring)});
        }
    }

    std::sort(activeOverlay_.begin(), activeOverlay_.end(),
              [](const OverlayEntry& a, const OverlayEntry& b) {
                  return a.key < b.key;
              });

    cJSON_Delete(root);

    LOG_I(TAG, "Overlay loaded: %u languages, %u entries for '%s'",
          static_cast<unsigned>(overlayLangs_.size()),
          static_cast<unsigned>(activeOverlay_.size()),
          currentLang_.c_str());

    if (onChanged_) onChanged_();
    return true;
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
