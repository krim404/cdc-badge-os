#include "cdc_os_ui/WifiHandlers.h"
#include "cdc_hal/IWifiController.h"
#include "cdc_hal/IRtc.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ToastView.h"
#include "nvs.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>

namespace cdc::ui {

/**
 * \brief Resets Wi-Fi wizard state to defaults.
 * \return void
 */
void WifiWizard::reset() {
    memset(this, 0, sizeof(*this));
    useDhcp = true;
    strncpy(netmask, "255.255.255.0", sizeof(netmask));
}

/**
 * \brief Returns singleton Wi-Fi handlers instance.
 * \return Reference to global `WifiHandlers` instance.
 */
WifiHandlers& WifiHandlers::instance() {
    static WifiHandlers s_instance;
    return s_instance;
}

/**
 * \brief Validates a single IPv4 octet.
 * \param val Octet value.
 * \return `true` if value is in `[0,255]`.
 */
bool WifiHandlers::isValidIpOctet(int val) {
    return val >= 0 && val <= 255;
}

/**
 * \brief Validates dotted IPv4 address string.
 * \param ip IPv4 address string.
 * \return `true` if address is syntactically and numerically valid.
 */
bool WifiHandlers::isValidIpAddress(const char* ip) {
    if (!ip || !ip[0]) return false;
    int a, b, c, d;
    if (sscanf(ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
    return isValidIpOctet(a) && isValidIpOctet(b) && isValidIpOctet(c) && isValidIpOctet(d);
}

/**
 * \brief Parses dotted IPv4 string into packed `uint32_t`.
 * \param ip IPv4 address string.
 * \return Packed IPv4 value or `0` on parse/validation error.
 */
uint32_t WifiHandlers::parseIpAddress(const char* ip) const {
    int a, b, c, d;
    if (sscanf(ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return 0;
    if (!isValidIpOctet(a) || !isValidIpOctet(b) || !isValidIpOctet(c) || !isValidIpOctet(d)) return 0;
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
}

/**
 * \brief Returns the persisted connect timeout, clamped to the valid range.
 */
uint32_t WifiHandlers::getConnectTimeoutMs() const {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        return WIFI_CONNECT_TIMEOUT_DEFAULT_MS;
    }
    uint32_t ms = WIFI_CONNECT_TIMEOUT_DEFAULT_MS;
    nvs_get_u32(nvs, "tout", &ms);
    nvs_close(nvs);
    if (ms < WIFI_CONNECT_TIMEOUT_MIN_MS) ms = WIFI_CONNECT_TIMEOUT_MIN_MS;
    if (ms > WIFI_CONNECT_TIMEOUT_MAX_MS) ms = WIFI_CONNECT_TIMEOUT_MAX_MS;
    return ms;
}

/**
 * \brief Persists the connect timeout if the value is in range.
 */
bool WifiHandlers::setConnectTimeoutMs(uint32_t ms) {
    if (ms < WIFI_CONNECT_TIMEOUT_MIN_MS || ms > WIFI_CONNECT_TIMEOUT_MAX_MS) {
        return false;
    }
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_u32(nvs, "tout", ms);
    if (err == ESP_OK) nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

/**
 * \brief Stores credentials (WPA2/DHCP defaults) and persists them.
 */
void WifiHandlers::saveCredentials(const char* ssid, const char* password) {
    wizard_.reset();
    if (ssid) {
        strncpy(wizard_.ssid, ssid, sizeof(wizard_.ssid) - 1);
    }
    if (password) {
        strncpy(wizard_.password, password, sizeof(wizard_.password) - 1);
    }
    wizard_.security = hal::WifiSecurity::WPA2_PSK;
    wizard_.useDhcp = true;
    saveConfig();
}

/**
 * \brief Erases the WiFi NVS namespace and invalidates the cached config.
 */
void WifiHandlers::clearConfig() {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    config_ = {};
    config_.valid = false;
    userEnabled_ = false;
}

/**
 * \brief Loads Wi-Fi configuration from NVS.
 * \return void
 */
void WifiHandlers::loadConfig() {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        config_.valid = false;
        userEnabled_ = false;
        return;
    }

    uint8_t ena = 0;
    nvs_get_u8(nvs, "ena", &ena);
    userEnabled_ = (ena != 0);

    size_t len = sizeof(config_.ssid);
    if (nvs_get_str(nvs, "ssid", config_.ssid, &len) != ESP_OK || len <= 1) {
        nvs_close(nvs);
        config_.valid = false;
        return;
    }

    len = sizeof(config_.password);
    nvs_get_str(nvs, "pass", config_.password, &len);
    nvs_get_u8(nvs, "sec", &config_.security);

    uint8_t dhcp = 1;
    nvs_get_u8(nvs, "dhcp", &dhcp);
    config_.useDhcp = (dhcp != 0);

    nvs_get_u32(nvs, "ip", &config_.staticIp);
    nvs_get_u32(nvs, "gw", &config_.gateway);
    nvs_get_u32(nvs, "nm", &config_.netmask);

    nvs_close(nvs);
    config_.valid = true;
}

/**
 * \brief Saves current wizard Wi-Fi configuration to NVS.
 * \return void
 */
void WifiHandlers::saveConfig() {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) return;

    nvs_set_str(nvs, "ssid", wizard_.ssid);
    nvs_set_str(nvs, "pass", wizard_.password);
    nvs_set_u8(nvs, "sec", static_cast<uint8_t>(wizard_.security));
    nvs_set_u8(nvs, "dhcp", wizard_.useDhcp ? 1 : 0);

    // Parse and save static IP config
    if (!wizard_.useDhcp) {
        nvs_set_u32(nvs, "ip", parseIpAddress(wizard_.staticIp));
        nvs_set_u32(nvs, "gw", parseIpAddress(wizard_.gateway));
        nvs_set_u32(nvs, "nm", parseIpAddress(wizard_.netmask));
    }

    nvs_commit(nvs);
    nvs_close(nvs);

    // Reload config
    loadConfig();
}

/**
 * \brief Returns whether Wi-Fi is currently connected.
 * \return `true` if station is connected.
 */
bool WifiHandlers::isConnected() const {
    auto* wifi = hal::getWifiControllerInstance();
    return wifi && wifi->isConnected();
}

/**
 * \brief Connects to Wi-Fi using saved configuration.
 * \return `true` on successful connection.
 */
bool WifiHandlers::connect() {
    if (!config_.valid) {
        lastError_ = "No config";
        return false;
    }

    auto* wifi = hal::getWifiControllerInstance();
    if (!wifi) {
        lastError_ = "No WiFi HW";
        return false;
    }

    if (!wifi->isEnabled()) {
        if (!wifi->enable(hal::WifiMode::STA)) {
            lastError_ = "WiFi init failed";
            return false;
        }
    }

    char toastMsg[96];
    std::snprintf(toastMsg, sizeof(toastMsg), "%s\n%s",
                  cdc::ui::tr("core.wifi_connecting"), config_.ssid);
    showToastTask(toastMsg, 0);

    bool connected = wifi->connect(config_.ssid, config_.password, getConnectTimeoutMs());

    ViewStack::instance().hideModal();
    ViewStack::instance().render();

    if (!connected || !wifi->isConnected()) {
        hal::WifiState state = wifi->getWifiState();
        switch (state) {
            case hal::WifiState::DISCONNECTED: lastError_ = "Disconnected"; break;
            case hal::WifiState::CONNECTION_FAILED: lastError_ = "Auth failed"; break;
            default: lastError_ = "Timeout"; break;
        }
        return false;
    }

    lastError_ = nullptr;
    return true;
}

/**
 * \brief Disconnects and disables Wi-Fi if active.
 * \return void
 */
void WifiHandlers::disconnect() {
    auto* wifi = hal::getWifiControllerInstance();
    if (!wifi) return;

    if (wifi->isConnected()) {
        wifi->disconnect();
        // Let lwIP's tcpip thread finish the DHCP release before we tear the
        // Wi-Fi driver down; otherwise dhcp_release_and_stop sends a UDP
        // packet over a half-deinitialised driver and dereferences a NULL
        // pointer in ieee80211_output_do.
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (wifi->isEnabled()) {
        wifi->disable();
    }
}

/**
 * \brief Synchronizes system time via NTP.
 *
 * Always contacts an NTP server; callers that only want a sync when the time
 * is currently unset should check `IRtc::isTimeSet()` themselves before
 * calling. The `disconnectAfter` flag controls whether a connection opened
 * by this function is also torn down on return; an existing connection
 * (opened by the caller) is never disconnected here.
 *
 * \return `true` if the system time is valid after the call.
 */
bool WifiHandlers::syncNtp(bool disconnectAfter) {
    auto* wifi = hal::getWifiControllerInstance();
    if (!wifi) {
        lastError_ = "No WiFi HW";
        return false;
    }

    // Track if we established the connection ourselves
    bool weConnected = false;

    // If not connected, try to connect using saved config
    if (!wifi->isConnected()) {
        if (!config_.valid) {
            lastError_ = "No config";
            return false;
        }

        // Enable WiFi and connect
        if (!wifi->isEnabled()) {
            wifi->enable(hal::WifiMode::STA);
        }

        bool connected = wifi->connect(config_.ssid, config_.password, getConnectTimeoutMs());

        if (!connected || !wifi->isConnected()) {
            lastError_ = "Connect failed";
            return false;
        }

        weConnected = true;
    }

    // Initialize SNTP if not done
    static bool sntpInited = false;
    if (!sntpInited) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_init();
        sntpInited = true;
    } else {
        esp_sntp_restart();
    }

    // Wait for sync
    uint32_t startMs = esp_timer_get_time() / 1000;
    bool synced = false;

    while ((esp_timer_get_time() / 1000 - startMs) < NTP_SYNC_TIMEOUT_MS) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            synced = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Only disconnect if we opened the connection AND the caller asked us to
    if (weConnected && disconnectAfter) {
        wifi->disconnect();
        wifi->disable();
    }

    if (synced) {
        auto* rtc = hal::getRtcInstance();
        if (rtc) {
            rtc->markTimeSet();
        }
        lastError_ = nullptr;
        return true;
    }

    lastError_ = "NTP timeout";
    return false;
}

/**
 * \brief Ensures WiFi is connected; runs NTP sync only when the time is unset.
 *
 * Connection stays open after return. The caller must call \ref disconnect()
 * when done. Triggers an NTP sync that reuses the connection (no tear-down).
 *
 * \return `true` on a usable connection.
 */
bool WifiHandlers::ensureConnected() {
    auto* wifi = hal::getWifiControllerInstance();
    if (!wifi) {
        lastError_ = "No WiFi HW";
        return false;
    }

    auto syncTimeIfNeeded = [this]() {
        auto* rtc = hal::getRtcInstance();
        if (!rtc || !rtc->isTimeSet()) {
            syncNtp(/*disconnectAfter=*/false);  // best-effort, ignore result
        }
    };

    if (wifi->isConnected()) {
        syncTimeIfNeeded();
        return true;
    }
    if (!config_.valid) {
        loadConfig();
    }
    if (!config_.valid) {
        lastError_ = "No WLAN configured";
        return false;
    }
    if (!connect()) {
        return false;
    }
    syncTimeIfNeeded();
    return true;
}

/**
 * \brief Persists the user/system WiFi intent flag (NVS key "ena").
 */
void WifiHandlers::persistUserEnabled(bool enabled) {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, "ena", enabled ? 1 : 0);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/**
 * \brief Tears WiFi down only when no holder remains and intent is off.
 */
void WifiHandlers::maybeRelease() {
    if (!userEnabled_ && holdCount_ <= 0) {
        disconnect();
    }
}

/**
 * \brief Applies and persists the user/system WiFi intent.
 */
bool WifiHandlers::setUserEnabled(bool enabled) {
    userEnabled_ = enabled;
    persistUserEnabled(enabled);

    if (enabled) {
        if (isConnected()) return true;
        if (!config_.valid) loadConfig();
        if (!config_.valid) {
            lastError_ = "No WLAN configured";
            return false;
        }
        return connect();
    }

    maybeRelease();
    return true;
}

/**
 * \brief Acquires a plugin/host hold and ensures WiFi is connected.
 */
bool WifiHandlers::acquire() {
    ++holdCount_;
    if (ensureConnected()) return true;
    if (holdCount_ > 0) --holdCount_;
    return false;
}

/**
 * \brief Releases a plugin/host hold and tears WiFi down if appropriate.
 */
void WifiHandlers::release() {
    if (holdCount_ > 0) --holdCount_;
    maybeRelease();
}

/**
 * \brief Reconnects at boot if WiFi intent was persisted as enabled.
 */
void WifiHandlers::restoreOnBoot() {
    loadConfig();
    if (userEnabled_ && config_.valid) {
        connect();
    }
}

} // namespace cdc::ui
