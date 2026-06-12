#pragma once

#include "cdc_hal/IWifiController.h"
#include <cstdint>

namespace cdc::ui {

// WiFi timeout constants (milliseconds)
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_DEFAULT_MS = 15000;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MIN_MS     = 3000;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MAX_MS     = 60000;
static constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 10000;
static constexpr uint32_t NTP_SYNC_TIMEOUT_MS = 10000;

// WiFi Setup Wizard State
struct WifiWizard {
    char ssid[33] = {};
    char password[65] = {};
    hal::WifiSecurity security = hal::WifiSecurity::WPA2_PSK;
    bool useDhcp = true;
    char staticIp[16] = {};
    char gateway[16] = {};
    char netmask[16] = "255.255.255.0";
    bool fromScan = false;

    void reset();
};

// WiFi Stored Configuration
struct WifiConfig {
    char ssid[33] = {};
    char password[65] = {};
    uint8_t security = 0;
    bool useDhcp = true;
    uint32_t staticIp = 0;
    uint32_t gateway = 0;
    uint32_t netmask = 0;
    bool valid = false;
};

// WiFi management handler class
class WifiHandlers {
public:
    static WifiHandlers& instance();

    // Config persistence
    void loadConfig();
    void saveConfig();
    WifiConfig& config() { return config_; }
    WifiWizard& wizard() { return wizard_; }

    /**
     * \brief Stores credentials directly (WPA2-PSK, DHCP) and persists them.
     *
     * Resets the wizard, fills SSID/password, marks DHCP+WPA2, then writes to NVS
     * via `saveConfig()` and reloads the cached config.
     *
     * \param ssid Null-terminated SSID (max 32 chars, truncated if longer).
     * \param password Null-terminated password (nullptr or "" for open networks).
     */
    void saveCredentials(const char* ssid, const char* password);

    /**
     * \brief Erases the entire "wifi" NVS namespace and invalidates the cached
     *        configuration.
     */
    void clearConfig();

    /**
     * \brief Reads the persisted connect timeout (NVS key "tout").
     * \return Timeout in ms, clamped to [WIFI_CONNECT_TIMEOUT_MIN_MS,
     *         WIFI_CONNECT_TIMEOUT_MAX_MS]; default
     *         WIFI_CONNECT_TIMEOUT_DEFAULT_MS if unset.
     */
    uint32_t getConnectTimeoutMs() const;

    /**
     * \brief Persists the connect timeout to NVS.
     * \param ms Timeout in ms; must lie within
     *           [WIFI_CONNECT_TIMEOUT_MIN_MS, WIFI_CONNECT_TIMEOUT_MAX_MS].
     * \return `true` on success, `false` if the value is out of range or NVS
     *         could not be opened.
     */
    bool setConnectTimeoutMs(uint32_t ms);

    // Connection management
    bool connect();
    void disconnect();
    bool isConnected() const;

    /**
     * \brief Ensures the device is connected to WiFi and optionally syncs
     *        time, leaving the connection up for the caller to use.
     *
     * Idempotent: if already connected, returns true after triggering an NTP
     * sync only when the system clock has not been set yet.
     *
     * The caller owns the lifetime of the connection and must call
     * \ref disconnect() when done. There is no auto-shutdown.
     *
     * \return `true` if WiFi is connected on return.
     */
    bool ensureConnected();

    /**
     * \brief Sets the user/system WiFi intent and applies it immediately.
     *
     * Persists the intent flag (NVS key "ena") and either brings WiFi up
     * (using the saved configuration) or tears it down. Tear-down is deferred
     * while plugin holders are still active (see \ref acquire); the radio only
     * goes down once no holder remains and the user intent is off.
     *
     * \param enabled `true` to turn WiFi on, `false` to turn it off.
     * \return When enabling, `true` if WiFi is connected on return; when
     *         disabling, always `true`.
     */
    bool setUserEnabled(bool enabled);

    /**
     * \brief Returns the persisted user/system WiFi intent.
     * \return `true` if WiFi was last turned on by the user/system.
     */
    bool isUserEnabled() const { return userEnabled_; }

    /**
     * \brief Persists the user/system WiFi intent without bringing the radio
     *        up or down.
     *
     * Unlike \ref setUserEnabled, this only stores the intent flag and updates
     * the cached value; the persisted state takes effect at the next
     * \ref restoreOnBoot. Used during settings restore to avoid a blocking
     * connect.
     *
     * \param enabled Intent to persist.
     */
    void persistUserIntent(bool enabled) {
        userEnabled_ = enabled;
        persistUserEnabled(enabled);
    }

    /**
     * \brief Acquires a hold on the WiFi connection for a plugin/host caller.
     *
     * Increments the holder count and ensures WiFi is connected. While at
     * least one holder is active, \ref release and \ref setUserEnabled(false)
     * will not tear the connection down. Pairs with \ref release.
     *
     * \return `true` if WiFi is connected on return.
     */
    bool acquire();

    /**
     * \brief Releases a previously acquired WiFi hold.
     *
     * Decrements the holder count and tears the connection down only when no
     * holder remains and the user intent is off. Safe to call when the count
     * is already zero.
     */
    void release();

    /**
     * \brief Restores the persisted WiFi intent at boot.
     *
     * Reloads the configuration and, if the user had WiFi enabled before the
     * last reboot and a valid configuration exists, reconnects.
     */
    void restoreOnBoot();

    /**
     * \brief Synchronizes system time via NTP.
     *
     * When `disconnectAfter` is `true` (default), the function reproduces the
     * legacy "connect, sync, disconnect" pattern used for one-shot time
     * fetches. When `false`, the caller is expected to manage the WiFi
     * lifetime; the function will reuse an active connection or open one
     * without tearing it down on return.
     *
     * If the RTC reports an already-valid time, the function returns `true`
     * without contacting any NTP server.
     */
    bool syncNtp(bool disconnectAfter = true);

    // Helper: IP validation
    static bool isValidIpAddress(const char* ip);

    // Get connection error message
    const char* getLastError() const { return lastError_; }

private:
    WifiHandlers() = default;

    WifiConfig config_;
    WifiWizard wizard_;
    const char* lastError_ = nullptr;

    bool userEnabled_ = false;
    int holdCount_ = 0;

    static bool isValidIpOctet(int val);
    uint32_t parseIpAddress(const char* ip) const;

    void persistUserEnabled(bool enabled);
    void maybeRelease();
};

} // namespace cdc::ui
