/**
 * \file
 * \brief Export/import of OS-level NVS settings for the backup container.
 *
 * These settings have no owning IModule, so the central BackupManager cannot
 * reach them through the module loop. This helper serializes them into the
 * top-level "system" section and restores them best-effort. Every value is
 * read/written exclusively through the owning service's public API, so the NVS
 * namespaces and keys are never re-encoded here.
 */

#include "cdc_os_ui/SystemSettingsBackup.h"
#include "cdc_os_ui/WifiHandlers.h"
#include "cdc_os_ui/SettingsHandlers.h"
#include "cdc_os_ui/views/LockScreenView.h"

#include "cdc_core/ModuleRegistry.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_hal/ISleepController.h"
#include "cdc_hal/IRtc.h"
#include "cdc_hal/IWifiController.h"
#include "cdc_ui/I18n.h"
#include "cdc_log.h"

#include "cJSON.h"

#include <cstring>
#include <cstdio>

namespace cdc::os_ui {

namespace {

constexpr const char* TAG = "SysBackup";

/// Bumped only on an incompatible layout change of the "system" section.
constexpr int kSchemaVer = 1;

/// Adds a string field only when the source value is non-empty.
void addStr(cJSON* obj, const char* key, const char* value) {
    if (value && value[0] != '\0') {
        cJSON_AddStringToObject(obj, key, value);
    }
}

/// Reads a JSON string field, returning nullptr when absent/empty/malformed.
const char* getStr(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        return item->valuestring;
    }
    return nullptr;
}

/// Reads a JSON number into \p out, returning false when absent/malformed.
bool getNum(const cJSON* obj, const char* key, double* out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return true;
    }
    return false;
}

/// Reads a JSON boolean into \p out, returning false when absent/malformed.
bool getBool(const cJSON* obj, const char* key, bool* out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return true;
    }
    return false;
}

} // namespace

bool SystemSettingsBackup::exportSystemSettings(cJSON* out) {
    if (!out) return false;

    cJSON_AddNumberToObject(out, "schema_ver", kSchemaVer);

    // Language (I18n owns NVS "i18n"/"langc").
    addStr(out, "language", cdc::ui::I18n::instance().getLanguageCode().c_str());

    // Backlight (IDisplay owns NVS "display"/"backlight").
    if (auto* display = hal::getDisplayInstance()) {
        cJSON_AddNumberToObject(out, "backlight", display->getBacklight());
    }

    // Light-sleep interval in seconds (ISleepController owns NVS "sleep"/"interval").
    if (auto* sleep = hal::getSleepControllerInstance()) {
        cJSON_AddNumberToObject(out, "sleep_interval", sleep->getLightSleepInterval());
    }

    // Timezone offset in hours (IRtc owns NVS "rtc"/"tz_offset").
    if (auto* rtc = hal::getRtcInstance()) {
        cJSON_AddNumberToObject(out, "tz_offset", rtc->getTimezoneOffset());
    }

    // Badge display text (SettingsHandlers owns NVS "display"/name|info|info2).
    char buf[cdc::ui::LockScreenView::MAX_TEXT_LEN];
    if (cdc::ui::settings::loadDisplayField("name", buf, sizeof(buf)))  addStr(out, "badge_name", buf);
    if (cdc::ui::settings::loadDisplayField("info", buf, sizeof(buf)))  addStr(out, "badge_info", buf);
    if (cdc::ui::settings::loadDisplayField("info2", buf, sizeof(buf))) addStr(out, "badge_info2", buf);

    // WiFi configuration (WifiHandlers owns NVS "wifi"/*). The container is
    // passphrase-encrypted, so the credentials may be included.
    auto& wifi = cdc::ui::WifiHandlers::instance();
    const cdc::ui::WifiConfig& cfg = wifi.config();
    if (cfg.valid) {
        cJSON* w = cJSON_AddObjectToObject(out, "wifi");
        if (w) {
            addStr(w, "ssid", cfg.ssid);
            addStr(w, "pass", cfg.password);
            cJSON_AddNumberToObject(w, "sec", cfg.security);
            cJSON_AddBoolToObject(w, "dhcp", cfg.useDhcp);
            if (!cfg.useDhcp) {
                cJSON_AddNumberToObject(w, "ip", cfg.staticIp);
                cJSON_AddNumberToObject(w, "gw", cfg.gateway);
                cJSON_AddNumberToObject(w, "nm", cfg.netmask);
            }
        }
    }
    cJSON_AddNumberToObject(out, "wifi_timeout", wifi.getConnectTimeoutMs());
    cJSON_AddBoolToObject(out, "wifi_enabled", wifi.isUserEnabled());

    // Per-module enable state (ModuleRegistry owns NVS "modules"/"disabled").
    cJSON* mods = cJSON_AddObjectToObject(out, "modules_enabled");
    if (mods) {
        auto& reg = cdc::core::ModuleRegistry::instance();
        uint8_t count = reg.getModuleCount();
        for (uint8_t i = 0; i < count; i++) {
            cdc::core::IModule* m = reg.getModuleAt(i);
            if (m && m->getName()) {
                cJSON_AddBoolToObject(mods, m->getName(), reg.isModuleEnabled(i));
            }
        }
    }

    return true;
}

cdc::core::IModule::BackupResult SystemSettingsBackup::importSystemSettings(const cJSON* in) {
    cdc::core::IModule::BackupResult r;
    if (!in) return r;

    double sv = 0;
    if (getNum(in, "schema_ver", &sv) && static_cast<int>(sv) != kSchemaVer) {
        LOG_W(TAG, "system schema_ver %d != expected %d, skipping section",
              static_cast<int>(sv), kSchemaVer);
        return r;
    }

    // Language (applies live; persists via I18n).
    if (const char* lang = getStr(in, "language")) {
        if (cdc::ui::I18n::instance().setLanguageCode(lang)) r.imported++; else r.failed++;
    }

    // Backlight (applies live; persists to NVS).
    double num = 0;
    if (getNum(in, "backlight", &num)) {
        if (auto* display = hal::getDisplayInstance()) {
            display->setBacklight(static_cast<uint16_t>(num));
            display->saveBacklight();
            r.imported++;
        } else {
            r.failed++;
        }
    }

    // Light-sleep interval.
    if (getNum(in, "sleep_interval", &num)) {
        if (auto* sleep = hal::getSleepControllerInstance()) {
            sleep->setLightSleepInterval(static_cast<uint32_t>(num));
            r.imported++;
        } else {
            r.failed++;
        }
    }

    // Timezone offset.
    if (getNum(in, "tz_offset", &num)) {
        if (auto* rtc = hal::getRtcInstance()) {
            rtc->setTimezoneOffset(static_cast<int8_t>(num));
            r.imported++;
        } else {
            r.failed++;
        }
    }

    // Badge display text (persisted; reflected on the lock screen at next boot).
    if (const char* v = getStr(in, "badge_name"))  { cdc::ui::settings::saveDisplayField("name", v);  r.imported++; }
    if (const char* v = getStr(in, "badge_info"))  { cdc::ui::settings::saveDisplayField("info", v);  r.imported++; }
    if (const char* v = getStr(in, "badge_info2")) { cdc::ui::settings::saveDisplayField("info2", v); r.imported++; }

    // WiFi configuration. Drive the wizard + saveConfig path so SSID, password,
    // security and static-IP fields are all persisted; the connect intent is
    // applied at the next restoreOnBoot, never via a blocking connect here.
    auto& wifi = cdc::ui::WifiHandlers::instance();
    const cJSON* w = cJSON_GetObjectItemCaseSensitive(in, "wifi");
    if (cJSON_IsObject(w)) {
        const char* ssid = getStr(w, "ssid");
        if (ssid) {
            cdc::ui::WifiWizard& wz = wifi.wizard();
            wz.reset();
            std::strncpy(wz.ssid, ssid, sizeof(wz.ssid) - 1);
            if (const char* pass = getStr(w, "pass")) {
                std::strncpy(wz.password, pass, sizeof(wz.password) - 1);
            }
            double secNum = static_cast<uint8_t>(hal::WifiSecurity::WPA2_PSK);
            getNum(w, "sec", &secNum);
            wz.security = static_cast<hal::WifiSecurity>(static_cast<uint8_t>(secNum));

            bool dhcp = true;
            getBool(w, "dhcp", &dhcp);
            wz.useDhcp = dhcp;
            if (!dhcp) {
                // saveConfig() re-parses dotted-quad strings; reconstruct them
                // from the packed values produced at export.
                auto packToStr = [](const cJSON* obj, const char* key, char* dst, size_t dstSize) {
                    double v = 0;
                    getNum(obj, key, &v);
                    uint32_t ip = static_cast<uint32_t>(v);
                    std::snprintf(dst, dstSize, "%u.%u.%u.%u",
                                  static_cast<unsigned>((ip >> 24) & 0xFF),
                                  static_cast<unsigned>((ip >> 16) & 0xFF),
                                  static_cast<unsigned>((ip >> 8) & 0xFF),
                                  static_cast<unsigned>(ip & 0xFF));
                };
                packToStr(w, "ip", wz.staticIp, sizeof(wz.staticIp));
                packToStr(w, "gw", wz.gateway, sizeof(wz.gateway));
                packToStr(w, "nm", wz.netmask, sizeof(wz.netmask));
            }
            wifi.saveConfig();
            r.imported++;
        } else {
            r.failed++;
        }
    }

    if (getNum(in, "wifi_timeout", &num)) {
        if (wifi.setConnectTimeoutMs(static_cast<uint32_t>(num))) r.imported++; else r.failed++;
    }

    bool wifiEnabled = false;
    if (getBool(in, "wifi_enabled", &wifiEnabled)) {
        wifi.persistUserIntent(wifiEnabled);
        r.imported++;
    }

    // Per-module enable state.
    const cJSON* mods = cJSON_GetObjectItemCaseSensitive(in, "modules_enabled");
    if (cJSON_IsObject(mods)) {
        auto& reg = cdc::core::ModuleRegistry::instance();
        uint8_t count = reg.getModuleCount();
        for (const cJSON* item = mods->child; item; item = item->next) {
            if (!item->string || !cJSON_IsBool(item)) {
                r.failed++;
                continue;
            }
            bool found = false;
            for (uint8_t i = 0; i < count; i++) {
                cdc::core::IModule* m = reg.getModuleAt(i);
                if (m && m->getName() && std::strcmp(m->getName(), item->string) == 0) {
                    reg.setModuleEnabled(i, cJSON_IsTrue(item));
                    r.imported++;
                    found = true;
                    break;
                }
            }
            if (!found) r.failed++;
        }
    }

    LOG_I(TAG, "System settings import: %u applied, %u skipped", r.imported, r.failed);
    return r;
}

} // namespace cdc::os_ui
