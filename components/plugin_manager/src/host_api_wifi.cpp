/**
 * \file host_api_wifi.cpp
 * \brief WiFi host API - hold-counted acquire/release.
 *
 * Plugins call host_wifi_request() to ask the host to establish a connection,
 * and host_wifi_release() to give it back. WifiHandlers tracks outstanding
 * holders (plugin code + PluginManager prerequisites) and the user/system
 * WiFi intent, so concurrent users don't tear down a connection another holder
 * or the user still needs.
 *
 * The prerequisite system also acquires a hold for a plugin that declares
 * `wifi_connected`, so such a plugin gets WiFi up before `plugin_on_enter`
 * without calling host_wifi_request itself.
 */

#include "cdc_hal/IWifiController.h"
#include "cdc_os_ui/WifiHandlers.h"
#include "plugin_manager/host_api.h"

#include <cstring>

extern "C" void plg_log_warn(const char* msg);

namespace {

cdc::hal::IWifiController* wifi() {
    return cdc::hal::getWifiControllerInstance();
}

}  // namespace

extern "C" {

int host_wifi_request(uint32_t /*timeout_ms*/)
{
    return cdc::ui::WifiHandlers::instance().acquire() ? HOST_OK : HOST_ERR_TIMEOUT;
}

int host_wifi_release(void)
{
    cdc::ui::WifiHandlers::instance().release();
    return HOST_OK;
}

bool host_wifi_is_connected(void)
{
    return cdc::ui::WifiHandlers::instance().isConnected();
}

int host_wifi_ssid(char* out, size_t out_size)
{
    if (!out || out_size == 0) return HOST_ERR_INVALID_ARG;
    auto* w = wifi();
    if (!w) return HOST_ERR_NOT_FOUND;
    // The IWifiController contract does not guarantee a non-null SSID, so never
    // hand a NULL source to strncpy.
    const char* ssid = w->getCurrentSsid();
    std::strncpy(out, ssid ? ssid : "", out_size - 1);
    out[out_size - 1] = '\0';
    return HOST_OK;
}

int host_wifi_ip(char* out, size_t out_size)
{
    if (!out || out_size == 0) return HOST_ERR_INVALID_ARG;
    auto* w = wifi();
    if (!w) return HOST_ERR_NOT_FOUND;
    return w->getIpAddress(out, out_size) ? HOST_OK : HOST_ERR_GENERIC;
}

int8_t host_wifi_rssi(void)
{
    auto* w = wifi();
    return w ? w->getRssi() : 0;
}

int host_wifi_mac(uint8_t* out)
{
    if (!out) return HOST_ERR_INVALID_ARG;
    auto* w = wifi();
    if (!w) return HOST_ERR_NOT_FOUND;
    return w->getMacAddress(out) ? HOST_OK : HOST_ERR_GENERIC;
}

int host_wifi_start_scan(void)
{
    auto* w = wifi();
    if (!w) return HOST_ERR_NOT_FOUND;
    return w->startScan() ? HOST_OK : HOST_ERR_GENERIC;
}

bool host_wifi_scan_done(void)
{
    auto* w = wifi();
    return w ? w->isScanComplete() : false;
}

int host_wifi_scan_results(wifi_scan_result_t* out, size_t* count)
{
    if (!out || !count) return HOST_ERR_INVALID_ARG;
    auto* w = wifi();
    if (!w) return HOST_ERR_NOT_FOUND;
    constexpr uint8_t MAX_SCAN = cdc::hal::IWifiController::MAX_SCAN_RESULTS;
    uint8_t cap = (*count > MAX_SCAN) ? MAX_SCAN : static_cast<uint8_t>(*count);
    cdc::hal::WifiScanResult tmp[MAX_SCAN];
    uint8_t n = w->getScanResults(tmp, cap);
    for (uint8_t i = 0; i < n; ++i) {
        std::memcpy(out[i].ssid, tmp[i].ssid, sizeof(out[i].ssid));
        std::memcpy(out[i].bssid, tmp[i].bssid, sizeof(out[i].bssid));
        out[i].rssi = tmp[i].rssi;
        out[i].channel = tmp[i].channel;
        out[i].auth_mode = static_cast<uint8_t>(tmp[i].security);
    }
    *count = n;
    return HOST_OK;
}

}  // extern "C"
