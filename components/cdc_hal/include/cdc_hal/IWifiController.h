#pragma once

#include "cdc_core/IService.h"
#include <cstdint>
#include <cstddef>

namespace cdc::hal {

/**
 * WiFi operating modes
 */
enum class WifiMode : uint8_t {
    OFF,        // WiFi disabled
    STA,        // Station mode (connect to AP)
    AP,         // Access Point mode
    STA_AP      // Both modes simultaneously
};

/**
 * WiFi security types
 */
enum class WifiSecurity : uint8_t {
    OPEN,
    WEP,
    WPA_PSK,
    WPA2_PSK,
    WPA3_PSK,
    WPA2_ENTERPRISE
};

/**
 * WiFi connection state
 */
enum class WifiState : uint8_t {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    CONNECTION_FAILED,
    GOT_IP
};

/**
 * WiFi scan result entry
 */
struct WifiScanResult {
    char ssid[33];          // SSID (max 32 chars + null)
    uint8_t bssid[6];       // MAC address
    int8_t rssi;            // Signal strength in dBm
    uint8_t channel;        // WiFi channel
    WifiSecurity security;  // Security type
};

/**
 * WiFi Controller Interface
 * Handles WiFi stack initialization and connection management
 */
class IWifiController : public core::IService {
public:
    virtual ~IWifiController() = default;

    /**
     * Enable WiFi with specified mode
     * @param mode Operating mode
     * @return true if successfully enabled
     */
    virtual bool enable(WifiMode mode = WifiMode::STA) = 0;

    /**
     * Disable WiFi (save power)
     */
    virtual void disable() = 0;

    /**
     * Check if WiFi is currently enabled
     */
    virtual bool isEnabled() const = 0;

    /**
     * Get current operating mode
     */
    virtual WifiMode getMode() const = 0;

    /**
     * Get current connection state
     */
    virtual WifiState getWifiState() const = 0;

    /**
     * Connect to an access point (station mode)
     * @param ssid Network SSID
     * @param password Network password (nullptr for open networks)
     * @param timeoutMs Connection timeout in milliseconds
     * @return true if connection initiated
     */
    virtual bool connect(const char* ssid, const char* password,
                         uint32_t timeoutMs = 10000) = 0;

    /**
     * Disconnect from current network
     */
    virtual void disconnect() = 0;

    /**
     * Check if connected to a network
     */
    virtual bool isConnected() const = 0;

    /**
     * Get current SSID (when connected)
     */
    virtual const char* getCurrentSsid() const = 0;

    /**
     * Get current IP address (when connected)
     * @param ip Output buffer for IP string (min 16 bytes)
     * @return true if IP available
     */
    virtual bool getIpAddress(char* ip, size_t len) const = 0;

    /**
     * Get WiFi MAC address
     * @param mac Output buffer (6 bytes)
     * @return true if address retrieved
     */
    virtual bool getMacAddress(uint8_t* mac) const = 0;

    /**
     * Get signal strength of current connection
     * @return RSSI in dBm, or 0 if not connected
     */
    virtual int8_t getRssi() const = 0;

    /**
     * Maximum number of scan results retained/returned. Single source of truth
     * for both the controller's result buffer and any caller-side buffers.
     */
    static constexpr uint8_t MAX_SCAN_RESULTS = 32;

    /**
     * Start a WiFi scan
     * @return true if scan started
     */
    virtual bool startScan() = 0;

    /**
     * Check if scan is complete
     */
    virtual bool isScanComplete() const = 0;

    /**
     * Get scan results
     * @param results Output array
     * @param maxResults Maximum entries to return
     * @return Number of results found
     */
    virtual uint8_t getScanResults(WifiScanResult* results, uint8_t maxResults) = 0;

    // Access Point mode functions

    /**
     * Start an access point
     * @param ssid AP name
     * @param password AP password (nullptr for open)
     * @param channel WiFi channel (1-13)
     * @return true if AP started
     */
    virtual bool startAp(const char* ssid, const char* password = nullptr,
                         uint8_t channel = 1) = 0;

    /**
     * Get number of connected stations (AP mode)
     */
    virtual uint8_t getConnectedStations() const = 0;
};

// Factory function
IWifiController* getWifiControllerInstance();

} // namespace cdc::hal
