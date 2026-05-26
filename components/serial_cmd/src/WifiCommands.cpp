/**
 * WiFi Serial Command Handlers
 *
 * Provides serial-console control over the WiFi subsystem:
 * scan, status, on/off, connect, timeout, forget.
 */

#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "serial_cmd/Console.h"
#include "serial_cmd/ICommandRegistry.h"
#include "cdc_hal/IWifiController.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

static constexpr const char*  WIFI_NVS_NAMESPACE     = "wifi";
static constexpr const char*  WIFI_NVS_KEY_TIMEOUT   = "tout";
static constexpr const char*  WIFI_NVS_KEY_SSID      = "ssid";
static constexpr const char*  WIFI_NVS_KEY_PASSWORD  = "pass";
static constexpr const char*  WIFI_NVS_KEY_SECURITY  = "sec";
static constexpr const char*  WIFI_NVS_KEY_DHCP      = "dhcp";

static constexpr uint32_t     WIFI_DEFAULT_TIMEOUT_MS = 15000;
static constexpr uint32_t     WIFI_TIMEOUT_MIN_MS      = 3000;
static constexpr uint32_t     WIFI_TIMEOUT_MAX_MS      = 60000;
static constexpr uint32_t     WIFI_SCAN_POLL_MS        = 100;
static constexpr uint32_t     WIFI_SCAN_TIMEOUT_MS     = 10000;
static constexpr uint8_t      WIFI_MAX_SCAN_RESULTS    = 20;

namespace cdc::serial {

/* ------------------------------------------------------------------ */
/*  NVS helpers                                                        */
/* ------------------------------------------------------------------ */

/** \brief Reads the connect-timeout value from NVS (default 15000 ms). */
static uint32_t loadTimeoutMs() {
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return WIFI_DEFAULT_TIMEOUT_MS;
    }
    uint32_t ms = WIFI_DEFAULT_TIMEOUT_MS;
    nvs_get_u32(nvs, WIFI_NVS_KEY_TIMEOUT, &ms);
    nvs_close(nvs);
    return ms;
}

/** \brief Persists a connect-timeout value to NVS. */
static void saveTimeoutMs(uint32_t ms) {
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u32(nvs, WIFI_NVS_KEY_TIMEOUT, ms);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/** \brief Reads saved SSID from NVS. Returns true if config exists. */
static bool loadSsid(char* out, size_t len) {
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t slen = len;
    esp_err_t err = nvs_get_str(nvs, WIFI_NVS_KEY_SSID, out, &slen);
    nvs_close(nvs);
    return (err == ESP_OK && out[0] != '\0');
}

/* ------------------------------------------------------------------ */
/*  Security name helper                                               */
/* ------------------------------------------------------------------ */

static const char* securityName(cdc::hal::WifiSecurity sec) {
    switch (sec) {
        case cdc::hal::WifiSecurity::OPEN:             return "OPEN";
        case cdc::hal::WifiSecurity::WEP:              return "WEP";
        case cdc::hal::WifiSecurity::WPA_PSK:          return "WPA";
        case cdc::hal::WifiSecurity::WPA2_PSK:         return "WPA2";
        case cdc::hal::WifiSecurity::WPA3_PSK:         return "WPA3";
        case cdc::hal::WifiSecurity::WPA2_ENTERPRISE:  return "WPA2-E";
        default:                                       return "?";
    }
}

static const char* stateName(cdc::hal::WifiState st) {
    switch (st) {
        case cdc::hal::WifiState::DISCONNECTED:       return "DISCONNECTED";
        case cdc::hal::WifiState::CONNECTING:          return "CONNECTING";
        case cdc::hal::WifiState::CONNECTED:           return "CONNECTED";
        case cdc::hal::WifiState::CONNECTION_FAILED:   return "FAILED";
        case cdc::hal::WifiState::GOT_IP:              return "GOT_IP";
        default:                                       return "?";
    }
}

static const char* modeName(cdc::hal::WifiMode m) {
    switch (m) {
        case cdc::hal::WifiMode::OFF:    return "OFF";
        case cdc::hal::WifiMode::STA:    return "STA";
        case cdc::hal::WifiMode::AP:     return "AP";
        case cdc::hal::WifiMode::STA_AP: return "STA_AP";
        default:                          return "?";
    }
}

/* ------------------------------------------------------------------ */
/*  Scan dedup + sort                                                  */
/* ------------------------------------------------------------------ */

/** \brief Deduplicate scan results by SSID, keeping strongest RSSI.
 *  \return Number of unique entries (sorted descending by RSSI). */
static uint8_t dedupAndSort(cdc::hal::WifiScanResult* results, uint8_t count) {
    if (count <= 1) return count;

    // Deduplicate: for each SSID, keep the entry with highest RSSI
    uint8_t unique = 0;
    for (uint8_t i = 0; i < count; i++) {
        bool seen = false;
        for (uint8_t j = 0; j < unique; j++) {
            if (strcmp(results[i].ssid, results[j].ssid) == 0) {
                seen = true;
                if (results[i].rssi > results[j].rssi) {
                    results[j] = results[i];  // replace with stronger
                }
                break;
            }
        }
        if (!seen && results[i].ssid[0] != '\0') {
            if (unique != i) results[unique] = results[i];
            unique++;
        }
    }

    // Bubble-sort descending by RSSI (small N, fine)
    for (uint8_t i = 0; i < unique; i++) {
        for (uint8_t j = i + 1; j < unique; j++) {
            if (results[j].rssi > results[i].rssi) {
                cdc::hal::WifiScanResult tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    return unique;
}

/* ================================================================== */
/*  Command Handlers                                                   */
/* ================================================================== */

/**
 * \brief WIFI_SCAN — Scan for available networks.
 *
 * Enables STA mode if needed, starts an async scan, polls until
 * completion or timeout, then prints deduplicated results sorted
 * by RSSI.  WiFi is left enabled in STA mode after the scan so the
 * user can immediately issue WIFI_CONNECT.
 */
static void cmdWifiScan(const char* args) {
    (void)args;

    auto* wifi = cdc::hal::getWifiControllerInstance();
    if (!wifi) {
        cdc::serial::Console::printf("ERROR: WiFi not available\r\n");
        return;
    }

    // Enable STA if not already in a scannable mode
    bool wasOff = !wifi->isEnabled();
    if (wasOff || wifi->getMode() == cdc::hal::WifiMode::AP) {
        if (!wifi->enable(cdc::hal::WifiMode::STA)) {
            cdc::serial::Console::printf("ERROR: Failed to enable WiFi\r\n");
            return;
        }
    }

    cdc::serial::Console::printf("Scanning...\r\n");

    if (!wifi->startScan()) {
        cdc::serial::Console::printf("ERROR: Scan start failed\r\n");
        return;
    }

    // Poll for completion
    uint32_t elapsed = 0;
    while (!wifi->isScanComplete() && elapsed < WIFI_SCAN_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_SCAN_POLL_MS));
        elapsed += WIFI_SCAN_POLL_MS;
    }

    if (!wifi->isScanComplete()) {
        cdc::serial::Console::printf("ERROR: Scan timeout\r\n");
        return;
    }

    // Fetch and process results
    cdc::hal::WifiScanResult results[WIFI_MAX_SCAN_RESULTS];
    uint8_t count = wifi->getScanResults(results, WIFI_MAX_SCAN_RESULTS);
    count = dedupAndSort(results, count);

    if (count == 0) {
        cdc::serial::Console::printf("No networks found\r\n");
    } else {
        cdc::serial::Console::printf("#  %-32s %5s %3s %s\r\n",
                                     "SSID", "RSSI", "Ch", "Security");
        for (uint8_t i = 0; i < count; i++) {
            cdc::serial::Console::printf("%-2u %-32s %4d %3u %s\r\n",
                                         static_cast<unsigned>(i + 1),
                                         results[i].ssid,
                                         results[i].rssi,
                                         static_cast<unsigned>(results[i].channel),
                                         securityName(results[i].security));
        }
    }

    cdc::serial::Console::printf("OK\r\n");
}

/**
 * \brief WIFI_STATUS — Show current WiFi state and saved configuration.
 */
static void cmdWifiStatus(const char* args) {
    (void)args;

    auto* wifi = cdc::hal::getWifiControllerInstance();
    if (!wifi) {
        cdc::serial::Console::printf("ERROR: WiFi not available\r\n");
        return;
    }

    // Runtime state
    cdc::serial::Console::printf("Mode:      %s\r\n", modeName(wifi->getMode()));
    cdc::serial::Console::printf("Enabled:   %s\r\n", wifi->isEnabled() ? "yes" : "no");
    cdc::serial::Console::printf("State:     %s\r\n", stateName(wifi->getWifiState()));

    if (wifi->isConnected()) {
        cdc::serial::Console::printf("SSID:      %s\r\n", wifi->getCurrentSsid());

        char ip[16] = {};
        if (wifi->getIpAddress(ip, sizeof(ip))) {
            cdc::serial::Console::printf("IP:        %s\r\n", ip);
        }

        uint8_t mac[6] = {};
        if (wifi->getMacAddress(mac)) {
            cdc::serial::Console::printf("MAC:       %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }

        int8_t rssi = wifi->getRssi();
        if (rssi != 0) {
            cdc::serial::Console::printf("RSSI:      %d dBm\r\n", rssi);
        }
    }

    // Saved configuration
    char savedSsid[33] = {};
    if (loadSsid(savedSsid, sizeof(savedSsid))) {
        cdc::serial::Console::printf("\r\nSaved config:\r\n");
        cdc::serial::Console::printf("SSID:      %s\r\n", savedSsid);

        // Read security and timeout from NVS
        {
            nvs_handle_t nvs;
            if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
                uint8_t sec = 0;
                if (nvs_get_u8(nvs, WIFI_NVS_KEY_SECURITY, &sec) == ESP_OK) {
                    cdc::serial::Console::printf("Security:  %s\r\n",
                        securityName(static_cast<cdc::hal::WifiSecurity>(sec)));
                }
                uint32_t timeout = WIFI_DEFAULT_TIMEOUT_MS;
                nvs_get_u32(nvs, WIFI_NVS_KEY_TIMEOUT, &timeout);
                cdc::serial::Console::printf("Timeout:   %lu ms\r\n",
                    static_cast<unsigned long>(timeout));
                nvs_close(nvs);
            }
        }
    } else {
        cdc::serial::Console::printf("\r\nSaved config: (none)\r\n");
    }

    cdc::serial::Console::printf("OK\r\n");
}

/**
 * \brief WIFI_ON — Enable WiFi radio.
 * \param args Mode string: "sta" (default), "ap", or "sta_ap".
 */
static void cmdWifiOn(const char* args) {
    cdc::hal::WifiMode mode = cdc::hal::WifiMode::STA;

    if (args && args[0]) {
        if (strcasecmp(args, "ap") == 0) {
            mode = cdc::hal::WifiMode::AP;
        } else if (strcasecmp(args, "sta_ap") == 0) {
            mode = cdc::hal::WifiMode::STA_AP;
        } else if (strcasecmp(args, "sta") != 0) {
            cdc::serial::Console::printf("Usage: WIFI_ON [sta|ap|sta_ap]\r\n");
            return;
        }
    }

    auto* wifi = cdc::hal::getWifiControllerInstance();
    if (!wifi) {
        cdc::serial::Console::printf("ERROR: WiFi not available\r\n");
        return;
    }

    if (!wifi->enable(mode)) {
        cdc::serial::Console::printf("ERROR: Failed to enable WiFi\r\n");
        return;
    }

    cdc::serial::Console::printf("OK: %s mode enabled\r\n", modeName(mode));

    // If STA mode and saved config exists, auto-connect
    if (mode == cdc::hal::WifiMode::STA || mode == cdc::hal::WifiMode::STA_AP) {
        char savedSsid[33] = {};
        char savedPass[65] = {};

        nvs_handle_t nvs;
        if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(savedSsid);
            if (nvs_get_str(nvs, WIFI_NVS_KEY_SSID, savedSsid, &len) == ESP_OK && savedSsid[0]) {
                len = sizeof(savedPass);
                nvs_get_str(nvs, WIFI_NVS_KEY_PASSWORD, savedPass, &len);

                uint32_t timeout = WIFI_DEFAULT_TIMEOUT_MS;
                nvs_get_u32(nvs, WIFI_NVS_KEY_TIMEOUT, &timeout);

                cdc::serial::Console::printf("Reconnecting to %s...\r\n", savedSsid);
                bool ok = wifi->connect(savedSsid, savedPass[0] ? savedPass : nullptr, timeout);
                if (ok && wifi->isConnected()) {
                    char ip[16] = {};
                    wifi->getIpAddress(ip, sizeof(ip));
                    cdc::serial::Console::printf("OK: %s\r\n", ip[0] ? ip : "connected");
                } else {
                    wifi->disable();
                    cdc::serial::Console::printf("ERROR: Reconnect failed\r\n");
                }
            }
            nvs_close(nvs);
        }
    }
}

/**
 * \brief WIFI_OFF — Disable WiFi radio (disconnects first if connected).
 */
static void cmdWifiOff(const char* args) {
    (void)args;

    auto* wifi = cdc::hal::getWifiControllerInstance();
    if (!wifi) {
        cdc::serial::Console::printf("ERROR: WiFi not available\r\n");
        return;
    }

    if (!wifi->isEnabled()) {
        cdc::serial::Console::printf("OK: already off\r\n");
        return;
    }

    wifi->disconnect();
    wifi->disable();
    cdc::serial::Console::printf("OK: WiFi disabled\r\n");
}

/**
 * \brief WIFI_CONNECT — Connect to a network and persist credentials.
 * \param args SSID followed by password, separated by whitespace.
 */
static void cmdWifiConnect(const char* args) {
    if (!args || !args[0]) {
        cdc::serial::Console::printf("Usage: WIFI_CONNECT <ssid> <password>\r\n");
        return;
    }

    // Parse SSID and password from args
    char ssid[33] = {};
    char password[65] = {};

    const char* space = strchr(args, ' ');
    if (!space) {
        cdc::serial::Console::printf("Usage: WIFI_CONNECT <ssid> <password>\r\n");
        return;
    }

    size_t ssidLen = static_cast<size_t>(space - args);
    if (ssidLen >= sizeof(ssid)) ssidLen = sizeof(ssid) - 1;
    memcpy(ssid, args, ssidLen);
    ssid[ssidLen] = '\0';

    const char* pw = space + 1;
    while (*pw == ' ') pw++;  // skip extra spaces
    size_t pwLen = strlen(pw);
    if (pwLen >= sizeof(password)) pwLen = sizeof(password) - 1;
    memcpy(password, pw, pwLen);
    password[pwLen] = '\0';

    if (ssid[0] == '\0') {
        cdc::serial::Console::printf("ERROR: SSID required\r\n");
        return;
    }

    auto* wifi = cdc::hal::getWifiControllerInstance();
    if (!wifi) {
        cdc::serial::Console::printf("ERROR: WiFi not available\r\n");
        return;
    }

    // Enable STA if needed
    if (!wifi->isEnabled() || wifi->getMode() != cdc::hal::WifiMode::STA) {
        if (!wifi->enable(cdc::hal::WifiMode::STA)) {
            cdc::serial::Console::printf("ERROR: Failed to enable WiFi\r\n");
            return;
        }
    }

    uint32_t timeout = loadTimeoutMs();
    cdc::serial::Console::printf("Connecting to %s (timeout: %lu ms)...\r\n",
                                 ssid, static_cast<unsigned long>(timeout));

    bool ok = wifi->connect(ssid, password[0] ? password : nullptr, timeout);

    if (!ok || !wifi->isConnected()) {
        wifi->disable();  // full teardown stops internal retry loop
        cdc::serial::Console::printf("ERROR: Connection failed\r\n");
        return;
    }

    // Persist credentials to NVS
    {
        nvs_handle_t nvs;
        if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, WIFI_NVS_KEY_SSID, ssid);
            nvs_set_str(nvs, WIFI_NVS_KEY_PASSWORD, password);
            nvs_set_u8(nvs, WIFI_NVS_KEY_SECURITY,
                       static_cast<uint8_t>(cdc::hal::WifiSecurity::WPA2_PSK));
            nvs_set_u8(nvs, WIFI_NVS_KEY_DHCP, 1);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }

    char ip[16] = {};
    wifi->getIpAddress(ip, sizeof(ip));
    cdc::serial::Console::printf("OK: %s\r\n", ip[0] ? ip : "connected");
}

/**
 * \brief WIFI_TIMEOUT — Get or set the connect timeout.
 * \param args Optional timeout in milliseconds (3000–60000).
 */
static void cmdWifiTimeout(const char* args) {
    if (!args || !args[0]) {
        uint32_t ms = loadTimeoutMs();
        cdc::serial::Console::printf("%lu ms\r\n", static_cast<unsigned long>(ms));
        return;
    }

    char* end = nullptr;
    long val = strtol(args, &end, 10);
    if (end == args || *end != '\0' || val < static_cast<long>(WIFI_TIMEOUT_MIN_MS)
        || val > static_cast<long>(WIFI_TIMEOUT_MAX_MS)) {
        cdc::serial::Console::printf("Usage: WIFI_TIMEOUT [%lu-%lu]\r\n",
                                     static_cast<unsigned long>(WIFI_TIMEOUT_MIN_MS),
                                     static_cast<unsigned long>(WIFI_TIMEOUT_MAX_MS));
        return;
    }

    uint32_t ms = static_cast<uint32_t>(val);
    saveTimeoutMs(ms);
    cdc::serial::Console::printf("OK: %lu ms\r\n", static_cast<unsigned long>(ms));
}

/**
 * \brief WIFI_FORGET — Clear all saved WiFi configuration.
 *
 * Disconnects and disables WiFi first, then erases the "wifi" NVS
 * namespace (SSID, password, security, DHCP, timeout).
 */
static void cmdWifiForget(const char* args) {
    (void)args;

    auto* wifi = cdc::hal::getWifiControllerInstance();
    if (wifi && wifi->isEnabled()) {
        wifi->disconnect();
        wifi->disable();
    }

    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    cdc::serial::Console::printf("OK: WiFi configuration cleared\r\n");
}

/* ================================================================== */
/*  Registration                                                       */
/* ================================================================== */

void registerWifiCommands() {
    static bool s_registered = false;
    if (s_registered) return;

    auto& reg = cdc::serial::getCommandRegistry();

    reg.registerCommand({"WIFI_SCAN",
                         "Scan for WiFi networks",
                         cmdWifiScan, "wifi", true});
    reg.registerCommand({"WIFI_STATUS",
                         "Show WiFi status and saved config",
                         cmdWifiStatus, "wifi", true});
    reg.registerCommand({"WIFI_ON",
                         "Enable WiFi [sta|ap|sta_ap]",
                         cmdWifiOn, "wifi", true});
    reg.registerCommand({"WIFI_OFF",
                         "Disable WiFi",
                         cmdWifiOff, "wifi", true});
    reg.registerCommand({"WIFI_CONNECT",
                         "Connect to network (SSID password)",
                         cmdWifiConnect, "wifi", true});
    reg.registerCommand({"WIFI_TIMEOUT",
                         "Get/set connect timeout [ms]",
                         cmdWifiTimeout, "wifi", true});
    reg.registerCommand({"WIFI_FORGET",
                         "Clear saved WiFi configuration",
                         cmdWifiForget, "wifi", true});

    s_registered = true;
}

} // namespace cdc::serial
