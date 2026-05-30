/**
 * ESP32 WiFi Controller Implementation
 * Full WiFi stack management with station and AP modes
 */

#include "cdc_hal/IWifiController.h"
#include "cdc_hal/hw_config.h"
#include "cdc_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include <cstring>
#include <new>

static const char* TAG = "WiFi-Ctrl";

/** \brief Event-group bit definitions for Wi-Fi connection state. */
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1
#define WIFI_SCAN_DONE_BIT    BIT2
#define WIFI_GOT_IP_BIT       BIT3

namespace cdc::hal {

/**
 * ESP32 WiFi Controller Implementation
 */
class WifiController : public IWifiController {
public:
    WifiController() = default;

    // IService implementation
    bool init() override;
    bool start() override;
    void stop() override;
    core::ServiceState getState() const override { return state_; }
    const char* getName() const override { return "wifi"; }

    // IWifiController implementation
    bool enable(WifiMode mode = WifiMode::STA) override;
    void disable() override;
    bool isEnabled() const override { return enabled_; }
    WifiMode getMode() const override { return currentMode_; }
    WifiState getWifiState() const override { return wifiState_; }

    bool connect(const char* ssid, const char* password,
                 uint32_t timeoutMs = 10000) override;
    void disconnect() override;
    bool isConnected() const override { return wifiState_ == WifiState::GOT_IP; }
    const char* getCurrentSsid() const override { return currentSsid_; }
    bool getIpAddress(char* ip, size_t len) const override;
    bool getMacAddress(uint8_t* mac) const override;
    int8_t getRssi() const override;

    bool startScan() override;
    bool isScanComplete() const override;
    uint8_t getScanResults(WifiScanResult* results, uint8_t maxResults) override;

    bool startAp(const char* ssid, const char* password = nullptr,
                 uint8_t channel = 1) override;
    uint8_t getConnectedStations() const override;

    // Event handlers (called from WiFi event callbacks)
    void onWifiEvent(int32_t eventId, void* eventData);
    void onIpEvent(int32_t eventId, void* eventData);

private:
    bool initNetif();
    void deinitNetif();

    core::ServiceState state_ = core::ServiceState::UNINITIALIZED;
    bool enabled_ = false;
    WifiMode currentMode_ = WifiMode::OFF;
    WifiState wifiState_ = WifiState::DISCONNECTED;

    // Network interfaces
    esp_netif_t* staNetif_ = nullptr;
    esp_netif_t* apNetif_ = nullptr;

    // Event handling
    EventGroupHandle_t eventGroup_ = nullptr;
    esp_event_handler_instance_t wifiEventHandler_ = nullptr;
    esp_event_handler_instance_t ipEventHandler_ = nullptr;

    // Connection info
    char currentSsid_[33] = {0};
    esp_ip4_addr_t currentIp_ = {0};
    uint8_t retryCount_ = 0;
    static constexpr uint8_t MAX_RETRY = 5;

    // Scan state
    bool scanInProgress_ = false;
    bool scanComplete_ = false;

    // Singleton for event callbacks (public for static event handlers)
public:
    static WifiController* instance_;
};

WifiController* WifiController::instance_ = nullptr;

/** \brief Wi-Fi event callback bridge. */
/**
 * \brief ESP-IDF Wi-Fi event bridge to controller instance.
 * \param arg Optional callback context (unused).
 * \param eventBase Event base (unused).
 * \param eventId Wi-Fi event id.
 * \param eventData Event payload.
 */
static void wifiEventHandler(void* arg, esp_event_base_t eventBase,
                             int32_t eventId, void* eventData) {
    (void)arg;
    if (WifiController::instance_) {
        WifiController::instance_->onWifiEvent(eventId, eventData);
    }
}

/** \brief IP event callback bridge. */
/**
 * \brief ESP-IDF IP event bridge to controller instance.
 * \param arg Optional callback context (unused).
 * \param eventBase Event base (unused).
 * \param eventId IP event id.
 * \param eventData Event payload.
 */
static void ipEventHandler(void* arg, esp_event_base_t eventBase,
                           int32_t eventId, void* eventData) {
    (void)arg;
    if (WifiController::instance_) {
        WifiController::instance_->onIpEvent(eventId, eventData);
    }
}

/**
 * \brief Initializes controller resources and event group.
 * \return `true` on successful initialization.
 */
bool WifiController::init() {
    if (state_ != core::ServiceState::UNINITIALIZED) {
        return state_ == core::ServiceState::INITIALIZED ||
               state_ == core::ServiceState::STARTED;
    }

    instance_ = this;

    // Create event group
    eventGroup_ = xEventGroupCreate();
    if (!eventGroup_) {
        LOG_E(TAG, "Failed to create event group");
        state_ = core::ServiceState::ERROR;
        return false;
    }

    state_ = core::ServiceState::INITIALIZED;
    LOG_I(TAG, "WiFi controller initialized");
    return true;
}

/**
 * \brief Starts Wi-Fi controller service state.
 * \return `true` if service is started after the call.
 */
bool WifiController::start() {
    if (state_ == core::ServiceState::INITIALIZED ||
        state_ == core::ServiceState::STOPPED) {
        state_ = core::ServiceState::STARTED;
        return true;
    }
    return state_ == core::ServiceState::STARTED;
}

/**
 * \brief Stops Wi-Fi controller and disables active Wi-Fi stack.
 */
void WifiController::stop() {
    if (state_ == core::ServiceState::STARTED) {
        if (enabled_) {
            disable();
        }
        state_ = core::ServiceState::STOPPED;
    }
}

/**
 * \brief Initializes network stack and default event loop.
 * \return `true` on success.
 */
bool WifiController::initNetif() {
    // Initialize TCP/IP stack (once)
    static bool tcpipInitialized = false;
    if (!tcpipInitialized) {
        esp_err_t ret = esp_netif_init();
        if (ret != ESP_OK) {
            LOG_E(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
            return false;
        }

        ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            // ESP_ERR_INVALID_STATE means already created, which is fine
            LOG_E(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
            return false;
        }

        tcpipInitialized = true;
    }

    return true;
}

/**
 * \brief Destroys created STA/AP network interfaces.
 */
void WifiController::deinitNetif() {
    if (staNetif_) {
        esp_netif_destroy(staNetif_);
        staNetif_ = nullptr;
    }
    if (apNetif_) {
        esp_netif_destroy(apNetif_);
        apNetif_ = nullptr;
    }
}

/**
 * \brief Enables Wi-Fi in requested mode and starts driver.
 * \param mode Requested Wi-Fi mode.
 * \return `true` on success.
 */
bool WifiController::enable(WifiMode mode) {
    if (mode == WifiMode::OFF) {
        disable();
        return true;
    }

    if (enabled_ && currentMode_ == mode) {
        return true;
    }

    if (state_ != core::ServiceState::STARTED) {
        LOG_E(TAG, "Cannot enable - service not started");
        return false;
    }

    // Disable first if changing mode
    if (enabled_) {
        disable();
    }

    if (!initNetif()) {
        return false;
    }

    // Create network interfaces based on mode
    if (mode == WifiMode::STA || mode == WifiMode::STA_AP) {
        staNetif_ = esp_netif_create_default_wifi_sta();
    }
    if (mode == WifiMode::AP || mode == WifiMode::STA_AP) {
        apNetif_ = esp_netif_create_default_wifi_ap();
    }

    // Note: CONFIG_ESP_PHY_ENABLE_USB=y in sdkconfig.defaults
    // causes ESP-IDF to automatically call phy_bbpll_en_usb(true)
    // during PHY initialization, so no manual workaround needed.

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        LOG_E(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        deinitNetif();
        return false;
    }

    // Register event handlers
    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifiEventHandler, nullptr,
                                              &wifiEventHandler_);
    if (ret != ESP_OK) {
        LOG_E(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(ret));
        esp_wifi_deinit();
        deinitNetif();
        return false;
    }

    ret = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              &ipEventHandler, nullptr,
                                              &ipEventHandler_);
    if (ret != ESP_OK) {
        LOG_E(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler_);
        wifiEventHandler_ = nullptr;
        esp_wifi_deinit();
        deinitNetif();
        return false;
    }

    // Set mode
    wifi_mode_t wifiMode = WIFI_MODE_NULL;
    switch (mode) {
        case WifiMode::STA:    wifiMode = WIFI_MODE_STA; break;
        case WifiMode::AP:     wifiMode = WIFI_MODE_AP; break;
        case WifiMode::STA_AP: wifiMode = WIFI_MODE_APSTA; break;
        default: break;
    }

    ret = esp_wifi_set_mode(wifiMode);
    if (ret != ESP_OK) {
        LOG_E(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        disable();
        return false;
    }

    LOG_I(TAG, "Calling esp_wifi_start...");
    LOG_I(TAG, "Task: %s, stack free: %lu", pcTaskGetName(nullptr),
          (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
    console_flush();
    vTaskDelay(pdMS_TO_TICKS(100));  // Give time for USB to transmit
    ret = esp_wifi_start();
    LOG_I(TAG, "esp_wifi_start returned: %s", esp_err_to_name(ret));
    console_flush();
    if (ret != ESP_OK) {
        LOG_E(TAG, "esp_wifi_start failed");
        disable();
        return false;
    }

    LOG_I(TAG, "Setting enabled state...");
    console_flush();
    enabled_ = true;
    currentMode_ = mode;
    wifiState_ = WifiState::DISCONNECTED;

    LOG_I(TAG, "WiFi enabled (mode=%d)", static_cast<int>(mode));
    console_flush();
    return true;
}

/**
 * \brief Disables Wi-Fi driver and resets runtime state.
 */
void WifiController::disable() {
    if (!enabled_) {
        return;
    }

    // Stop WiFi
    esp_wifi_stop();
    esp_wifi_deinit();

    // Unregister event handlers
    if (wifiEventHandler_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifiEventHandler_);
        wifiEventHandler_ = nullptr;
    }
    if (ipEventHandler_) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID,
                                              ipEventHandler_);
        ipEventHandler_ = nullptr;
    }

    deinitNetif();

    enabled_ = false;
    currentMode_ = WifiMode::OFF;
    wifiState_ = WifiState::DISCONNECTED;
    currentSsid_[0] = '\0';
    currentIp_.addr = 0;

    LOG_I(TAG, "WiFi disabled");
}

/**
 * \brief Connects STA interface to an access point.
 * \param ssid Target SSID.
 * \param password Optional passphrase.
 * \param timeoutMs Timeout for waiting on connection result.
 * \return `true` when IP acquisition succeeded.
 */
bool WifiController::connect(const char* ssid, const char* password,
                              uint32_t timeoutMs) {
    if (!enabled_ || (currentMode_ != WifiMode::STA &&
                      currentMode_ != WifiMode::STA_AP)) {
        LOG_E(TAG, "Cannot connect - WiFi not in STA mode");
        return false;
    }

    // Configure station
    wifi_config_t wifiConfig = {};
    strncpy((char*)wifiConfig.sta.ssid, ssid, sizeof(wifiConfig.sta.ssid) - 1);
    if (password) {
        strncpy((char*)wifiConfig.sta.password, password,
                sizeof(wifiConfig.sta.password) - 1);
    }
    wifiConfig.sta.threshold.authmode = password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifiConfig);
    if (ret != ESP_OK) {
        LOG_E(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Save SSID
    strncpy(currentSsid_, ssid, sizeof(currentSsid_) - 1);
    currentSsid_[sizeof(currentSsid_) - 1] = '\0';

    // Reset state
    retryCount_ = 0;
    wifiState_ = WifiState::CONNECTING;
    xEventGroupClearBits(eventGroup_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_GOT_IP_BIT);

    // Start connection
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        LOG_E(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        wifiState_ = WifiState::CONNECTION_FAILED;
        return false;
    }

    // Wait for connection result
    EventBits_t bits = xEventGroupWaitBits(eventGroup_,
                                           WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeoutMs));

    if (bits & WIFI_GOT_IP_BIT) {
        LOG_I(TAG, "Connected to %s", ssid);
        vTaskDelay(pdMS_TO_TICKS(500));
        return true;
    } else {
        LOG_W(TAG, "Connection to %s failed", ssid);
        wifiState_ = WifiState::CONNECTION_FAILED;
        return false;
    }
}

/**
 * \brief Disconnects from current access point and clears connection state.
 */
void WifiController::disconnect() {
    if (wifiState_ == WifiState::CONNECTED || wifiState_ == WifiState::GOT_IP) {
        esp_wifi_disconnect();
    }
    wifiState_ = WifiState::DISCONNECTED;
    currentSsid_[0] = '\0';
    currentIp_.addr = 0;
}

/**
 * \brief Returns current STA IPv4 address as text.
 * \param ip Output text buffer.
 * \param len Output buffer size.
 * \return `true` if IP address was written.
 */
bool WifiController::getIpAddress(char* ip, size_t len) const {
    if (!ip || len < 16 || currentIp_.addr == 0) {
        return false;
    }

    snprintf(ip, len, IPSTR, IP2STR(&currentIp_));
    return true;
}

/**
 * \brief Reads MAC address of active Wi-Fi interface.
 * \param mac Output 6-byte MAC buffer.
 * \return `true` on success.
 */
bool WifiController::getMacAddress(uint8_t* mac) const {
    if (!mac) return false;

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    wifi_interface_t iface = (mode == WIFI_MODE_AP) ? WIFI_IF_AP : WIFI_IF_STA;
    return esp_wifi_get_mac(iface, mac) == ESP_OK;
}

/**
 * \brief Returns RSSI of current STA connection.
 * \return RSSI in dBm, or `0` if unavailable.
 */
int8_t WifiController::getRssi() const {
    if (wifiState_ != WifiState::GOT_IP) {
        return 0;
    }

    wifi_ap_record_t apInfo;
    if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
        return apInfo.rssi;
    }
    return 0;
}

/**
 * \brief Starts asynchronous AP scan.
 * \return `true` if scan start succeeded.
 */
bool WifiController::startScan() {
    LOG_I(TAG, "startScan() called, enabled=%d, mode=%d", enabled_, static_cast<int>(currentMode_));

    if (!enabled_ || (currentMode_ != WifiMode::STA &&
                      currentMode_ != WifiMode::STA_AP)) {
        LOG_W(TAG, "startScan() - not in STA mode");
        return false;
    }

    scanInProgress_ = true;
    scanComplete_ = false;
    xEventGroupClearBits(eventGroup_, WIFI_SCAN_DONE_BIT);

    wifi_scan_config_t scanConfig = {};
    scanConfig.show_hidden = true;

    LOG_I(TAG, "Starting WiFi scan...");
    esp_err_t ret = esp_wifi_scan_start(&scanConfig, false);
    if (ret != ESP_OK) {
        LOG_E(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
        scanInProgress_ = false;
        return false;
    }

    LOG_I(TAG, "Scan started successfully");
    return true;
}

/**
 * \brief Returns whether the last scan has completed.
 * \return `true` if scan results are ready.
 */
bool WifiController::isScanComplete() const {
    return scanComplete_;
}

/**
 * \brief Copies scan results into caller buffer.
 * \param results Output result array.
 * \param maxResults Maximum writable entries.
 * \return Number of copied scan entries.
 */
uint8_t WifiController::getScanResults(WifiScanResult* results, uint8_t maxResults) {
    if (!results || maxResults == 0 || !scanComplete_) {
        return 0;
    }

    uint16_t numAps = 0;
    esp_wifi_scan_get_ap_num(&numAps);

    if (numAps == 0) {
        return 0;
    }

    uint16_t toGet = (numAps < maxResults) ? numAps : maxResults;
    wifi_ap_record_t* apRecords = new (std::nothrow) wifi_ap_record_t[toGet];
    if (!apRecords) {
        LOG_E(TAG, "OOM allocating scan results buffer");
        return 0;
    }

    if (esp_wifi_scan_get_ap_records(&toGet, apRecords) != ESP_OK) {
        delete[] apRecords;
        return 0;
    }

    for (uint16_t i = 0; i < toGet; i++) {
        strncpy(results[i].ssid, (char*)apRecords[i].ssid, 32);
        results[i].ssid[32] = '\0';
        memcpy(results[i].bssid, apRecords[i].bssid, 6);
        results[i].rssi = apRecords[i].rssi;
        results[i].channel = apRecords[i].primary;

        // Map auth mode to security type
        switch (apRecords[i].authmode) {
            case WIFI_AUTH_OPEN:         results[i].security = WifiSecurity::OPEN; break;
            case WIFI_AUTH_WEP:          results[i].security = WifiSecurity::WEP; break;
            case WIFI_AUTH_WPA_PSK:      results[i].security = WifiSecurity::WPA_PSK; break;
            case WIFI_AUTH_WPA2_PSK:     results[i].security = WifiSecurity::WPA2_PSK; break;
            case WIFI_AUTH_WPA3_PSK:     results[i].security = WifiSecurity::WPA3_PSK; break;
            case WIFI_AUTH_WPA2_ENTERPRISE: results[i].security = WifiSecurity::WPA2_ENTERPRISE; break;
            default:                     results[i].security = WifiSecurity::WPA2_PSK; break;
        }
    }

    delete[] apRecords;
    return static_cast<uint8_t>(toGet);
}

/**
 * \brief Configures and starts soft-AP parameters.
 * \param ssid AP SSID.
 * \param password Optional AP password.
 * \param channel AP channel.
 * \return `true` on success.
 */
bool WifiController::startAp(const char* ssid, const char* password,
                              uint8_t channel) {
    if (!enabled_ || (currentMode_ != WifiMode::AP &&
                      currentMode_ != WifiMode::STA_AP)) {
        LOG_E(TAG, "Cannot start AP - WiFi not in AP mode");
        return false;
    }

    wifi_config_t wifiConfig = {};
    strncpy((char*)wifiConfig.ap.ssid, ssid, sizeof(wifiConfig.ap.ssid) - 1);
    wifiConfig.ap.ssid_len = strlen(ssid);
    wifiConfig.ap.channel = channel;
    wifiConfig.ap.max_connection = 4;

    if (password && strlen(password) >= 8) {
        strncpy((char*)wifiConfig.ap.password, password,
                sizeof(wifiConfig.ap.password) - 1);
        wifiConfig.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifiConfig.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_AP, &wifiConfig);
    if (ret != ESP_OK) {
        LOG_E(TAG, "esp_wifi_set_config (AP) failed: %s", esp_err_to_name(ret));
        return false;
    }

    LOG_I(TAG, "AP started: %s (channel %d)", ssid, channel);
    return true;
}

/**
 * \brief Returns number of stations connected to soft-AP.
 * \return Connected station count.
 */
uint8_t WifiController::getConnectedStations() const {
    if (currentMode_ != WifiMode::AP && currentMode_ != WifiMode::STA_AP) {
        return 0;
    }

    wifi_sta_list_t staList;
    if (esp_wifi_ap_get_sta_list(&staList) == ESP_OK) {
        return staList.num;
    }
    return 0;
}

/**
 * \brief Handles Wi-Fi events from ESP-IDF event loop.
 * \param eventId Event identifier.
 * \param eventData Event payload.
 */
void WifiController::onWifiEvent(int32_t eventId, void* eventData) {
    switch (eventId) {
        case WIFI_EVENT_STA_START:
            LOG_I(TAG, "Station started");
            console_flush();
            LOG_I(TAG, "Event handler returning...");
            console_flush();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            wifiState_ = WifiState::CONNECTED;
            xEventGroupSetBits(eventGroup_, WIFI_CONNECTED_BIT);
            LOG_I(TAG, "Connected to AP");
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifiState_ = WifiState::DISCONNECTED;
            if (retryCount_ < MAX_RETRY) {
                retryCount_++;
                LOG_I(TAG, "Reconnecting (attempt %d)", retryCount_);
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(eventGroup_, WIFI_FAIL_BIT);
                wifiState_ = WifiState::CONNECTION_FAILED;
            }
            break;
        }

        case WIFI_EVENT_SCAN_DONE:
            scanInProgress_ = false;
            scanComplete_ = true;
            xEventGroupSetBits(eventGroup_, WIFI_SCAN_DONE_BIT);
            LOG_I(TAG, "Scan complete");
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            auto* event = (wifi_event_ap_staconnected_t*)eventData;
            LOG_I(TAG, "Station " MACSTR " connected", MAC2STR(event->mac));
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            auto* event = (wifi_event_ap_stadisconnected_t*)eventData;
            LOG_I(TAG, "Station " MACSTR " disconnected", MAC2STR(event->mac));
            break;
        }

        default:
            break;
    }
}

/**
 * \brief Handles IP-related events from ESP-IDF event loop.
 * \param eventId Event identifier.
 * \param eventData Event payload.
 */
void WifiController::onIpEvent(int32_t eventId, void* eventData) {
    if (eventId == IP_EVENT_STA_GOT_IP) {
        auto* event = (ip_event_got_ip_t*)eventData;
        currentIp_ = event->ip_info.ip;
        wifiState_ = WifiState::GOT_IP;
        retryCount_ = 0;
        xEventGroupSetBits(eventGroup_, WIFI_GOT_IP_BIT);
        LOG_I(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    } else if (eventId == IP_EVENT_STA_LOST_IP) {
        currentIp_.addr = 0;
        if (wifiState_ == WifiState::GOT_IP) {
            wifiState_ = WifiState::CONNECTED;
        }
        LOG_W(TAG, "Lost IP address");
    }
}

/** \brief Singleton Wi-Fi controller instance. */
static WifiController g_wifiController;

/**
 * \brief Returns the singleton Wi-Fi controller service instance.
 * \return Pointer to the global `IWifiController` implementation.
 */
IWifiController* getWifiControllerInstance() {
    return &g_wifiController;
}

} // namespace cdc::hal
