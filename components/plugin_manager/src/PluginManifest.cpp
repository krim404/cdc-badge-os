#include "plugin_manager/PluginManifest.h"
#include "cdc_log.h"

#include "cJSON.h"

#include <cstdlib>
#include <cstring>

namespace cdc::plugin_manager {

static const char* TAG = "PLG_MAN";

static std::string str_or(const cJSON* node, const char* key, const char* dflt = "")
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(node, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return dflt;
}

static bool parse_api_level(const std::string& s, uint16_t& major, uint16_t& minor)
{
    if (s.empty()) return false;
    const char* dot = std::strchr(s.c_str(), '.');
    if (!dot) return false;
    major = static_cast<uint16_t>(std::atoi(s.c_str()));
    minor = static_cast<uint16_t>(std::atoi(dot + 1));
    return true;
}

static void parse_localized(const cJSON* root, std::map<std::string, LocalizedString>& out)
{
    if (!cJSON_IsObject(root)) return;
    for (cJSON* it = root->child; it != nullptr; it = it->next) {
        if (!it->string || !cJSON_IsObject(it)) continue;
        LocalizedString ls;
        for (cJSON* lang_it = it->child; lang_it != nullptr; lang_it = lang_it->next) {
            if (lang_it->string && cJSON_IsString(lang_it) && lang_it->valuestring) {
                ls.by_lang[lang_it->string] = lang_it->valuestring;
            }
        }
        out[it->string] = std::move(ls);
    }
}

static void parse_capabilities(const cJSON* root, PluginCapabilities& cap)
{
    if (!cJSON_IsObject(root)) return;

    auto get_bool = [&](const char* key, bool dflt) {
        const cJSON* n = cJSON_GetObjectItemCaseSensitive(root, key);
        return cJSON_IsBool(n) ? cJSON_IsTrue(n) : dflt;
    };

    cap.wifi             = get_bool("wifi", false);
    cap.ble              = get_bool("ble", false);
    cap.http             = get_bool("http", false);
    cap.socket           = get_bool("socket", false);
    cap.ui_exclusive     = get_bool("ui_exclusive", false);
    cap.display_lowlevel = get_bool("display_lowlevel", false);
    cap.sao              = get_bool("sao", false);
    cap.grove            = get_bool("grove", false);
    cap.pixel_strip      = get_bool("pixel_strip", false);
    cap.background       = get_bool("background", false);
    cap.usb_cdc          = get_bool("usb_cdc", false);
    cap.prevent_sleep    = get_bool("prevent_sleep", false);
    cap.autoload         = get_bool("autoload", false);
    cap.vfat             = get_bool("vfat", false);

    cap.nvs_namespace = str_or(root, "nvs_namespace", "");

    auto get_int_array_u8 = [&](const char* key, std::vector<uint8_t>& out) {
        const cJSON* n = cJSON_GetObjectItemCaseSensitive(root, key);
        if (!cJSON_IsArray(n)) return;
        for (cJSON* it = n->child; it != nullptr; it = it->next) {
            if (cJSON_IsNumber(it)) {
                out.push_back(static_cast<uint8_t>(it->valueint));
            }
        }
    };

    auto get_str_array = [&](const char* key, std::vector<std::string>& out) {
        const cJSON* n = cJSON_GetObjectItemCaseSensitive(root, key);
        if (!cJSON_IsArray(n)) return;
        for (cJSON* it = n->child; it != nullptr; it = it->next) {
            if (cJSON_IsString(it) && it->valuestring) {
                out.push_back(it->valuestring);
            }
        }
    };

    get_str_array    ("rmem",              cap.rmem);
    get_str_array    ("ecc",               cap.ecc);
    get_str_array    ("ble_service_uuids", cap.ble_service_uuids);
    get_int_array_u8 ("gpio_pins",         cap.gpio_pins);
    get_int_array_u8 ("pwm_pins",          cap.pwm_pins);
    get_int_array_u8 ("adc_pins",          cap.adc_pins);
    get_int_array_u8 ("i2c_bus",           cap.i2c_bus);
}

static void parse_prereqs(const cJSON* root, std::vector<PrerequisiteSpec>& out)
{
    if (!cJSON_IsObject(root)) return;
    for (cJSON* it = root->child; it != nullptr; it = it->next) {
        if (!it->string || !cJSON_IsObject(it)) continue;
        PrerequisiteSpec spec;
        spec.name = it->string;
        spec.on_fail = "abort";
        for (cJSON* p = it->child; p != nullptr; p = p->next) {
            if (!p->string) continue;
            if (cJSON_IsString(p) && p->valuestring) {
                if (std::strcmp(p->string, "on_fail") == 0) {
                    spec.on_fail = p->valuestring;
                } else {
                    spec.params[p->string] = p->valuestring;
                }
            } else if (cJSON_IsNumber(p)) {
                spec.params[p->string] = std::to_string(p->valueint);
            } else if (cJSON_IsBool(p)) {
                spec.params[p->string] = cJSON_IsTrue(p) ? "true" : "false";
            }
        }
        out.push_back(std::move(spec));
    }
}

bool PluginManifest::parse(const char* json, size_t len, PluginManifest& out)
{
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!root) {
        LOG_E(TAG, "JSON parse failed");
        return false;
    }

    out.id                  = str_or(root, "id");
    out.version             = str_or(root, "version");
    out.author              = str_or(root, "author");
    out.icon                = str_or(root, "icon");
    out.host_api_level_min  = str_or(root, "host_api_level_min");

    if (out.id.empty() || out.version.empty() || out.host_api_level_min.empty()) {
        LOG_E(TAG, "manifest missing required fields");
        cJSON_Delete(root);
        return false;
    }

    if (!parse_api_level(out.host_api_level_min, out.api_level_major, out.api_level_minor)) {
        LOG_E(TAG, "invalid host_api_level_min '%s'", out.host_api_level_min.c_str());
        cJSON_Delete(root);
        return false;
    }

    if (const cJSON* lm = cJSON_GetObjectItemCaseSensitive(root, "linear_memory_kb");
        cJSON_IsNumber(lm)) {
        out.linear_memory_kb = static_cast<uint32_t>(lm->valueint);
    }

    if (const cJSON* i18n = cJSON_GetObjectItemCaseSensitive(root, "i18n")) {
        out.default_language = str_or(i18n, "default_language", "en");
        parse_localized(cJSON_GetObjectItemCaseSensitive(i18n, "meta"),    out.i18n_meta);
        parse_localized(cJSON_GetObjectItemCaseSensitive(i18n, "strings"), out.i18n_strings);
    }

    parse_capabilities(cJSON_GetObjectItemCaseSensitive(root, "capabilities"), out.capabilities);
    parse_prereqs    (cJSON_GetObjectItemCaseSensitive(root, "prerequisites"), out.prerequisites);

    cJSON_Delete(root);
    return true;
}

}  // namespace cdc::plugin_manager
