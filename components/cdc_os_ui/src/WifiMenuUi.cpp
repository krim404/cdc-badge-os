/**
 * \file
 * \brief Wi-Fi UI flows for scanning, setup wizard, connection, and diagnostics.
 */

#include "AppUiInternal.h"
#include "cdc_os_ui/WifiHandlers.h"
#include "cdc_hal/IWifiController.h"
#include "cdc_views/SliderView.h"
#include "cdc_views/RenderHelpers.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

namespace cdc::ui {

using namespace cdc::ui;

/** \brief Wi-Fi menu size limits. */

static constexpr uint8_t WIFI_MAX_NETWORKS = 20;
static constexpr uint8_t WIFI_MENU_MAX_ITEMS = 16;

/** \brief One scanned Wi-Fi network entry displayed in the scan list. */
struct WifiItem {
    char ssid[33];
    int8_t rssi;
    hal::WifiSecurity security;
};

/** \brief Fixed Wi-Fi menu and wizard index enums. */

enum WifiMainMenuIdx {
    WIFI_IDX_CONNECT = 0,
    WIFI_IDX_SETUP,
    WIFI_IDX_DETAILS,
    WIFI_IDX_NTP_SYNC,
    WIFI_IDX_COUNT
};

enum WifiAuthIdx {
    WIFI_AUTH_WPA2 = 0,
    WIFI_AUTH_WPA_WPA2,
    WIFI_AUTH_WPA3,
    WIFI_AUTH_WPA,
    WIFI_AUTH_OPEN,
    WIFI_AUTH_WEP,
    WIFI_AUTH_COUNT
};

enum WifiIpModeIdx {
    WIFI_IP_DHCP = 0,
    WIFI_IP_STATIC,
    WIFI_IP_COUNT
};

/** \brief Static view pointers and wizard/scan state. */

static constexpr uint8_t WIFI_MENU_FIXED_COUNT = WIFI_IDX_COUNT;
static ListView* s_wifiMainMenu = nullptr;
static ListView* s_wifiScanView = nullptr;
static ListView* s_wifiAuthMenu = nullptr;
static ListView* s_wifiIpMenu = nullptr;

static ListItem s_wifiMainItems[WIFI_MENU_MAX_ITEMS];
static core::ModuleMenuItem s_wifiModuleItems[12];
static uint8_t s_wifiModuleCount = 0;
static ListItem s_wifiAuthItems[WIFI_AUTH_COUNT];
static ListItem s_wifiIpItems[WIFI_IP_COUNT];
static ListItem s_wifiScanItems[WIFI_MAX_NETWORKS + 1];
static char s_wifiManualLabel[48];
static WifiItem s_wifiScanResults[WIFI_MAX_NETWORKS];
static uint8_t s_wifiScanCount = 0;
static const char* s_wifiAuthLabels[WIFI_AUTH_COUNT] = {
    "WPA2", "WPA/WPA2", "WPA3", "WPA", "Open", "WEP"
};

/** \brief PSRAM-backed text buffer used for Wi-Fi details view. */
static constexpr size_t WIFI_DETAILS_BUF_SIZE = 512;
static EXT_RAM_BSS_ATTR char s_wifiDetailsBuf[WIFI_DETAILS_BUF_SIZE];

/** \brief Handles top-level Wi-Fi menu selection. */
static void onWifiMainSelect(uint16_t index, void* userData);
/** \brief Connects using saved Wi-Fi configuration. */
static void wifiConnect();
/** \brief Starts Wi-Fi setup wizard flow. */
static void wifiSetup();
/** \brief Shows Wi-Fi details/info screen. */
static void wifiShowDetails();
/** \brief Disconnects active Wi-Fi session. */
static void wifiDisconnect();
/** \brief Runs NTP synchronization via Wi-Fi. */
static void wifiNtpSync();
/** \brief Starts network scan and opens scan result list. */
static void wifiStartScan();
/** \brief Handles scan result selection. */
static void onWifiScanSelect(uint16_t index, void* userData);
/** \brief Sorts scan results by RSSI descending. */
static void sortWifiScanResults();
/** \brief Opens security/authentication selection menu. */
static void wifiShowAuthMenu();
/** \brief Handles authentication mode selection. */
static void onWifiAuthSelect(uint16_t index, void* userData);
/** \brief Opens password input for selected network. */
static void wifiShowPasswordInput();
/** \brief Stores entered password and continues wizard. */
static void onWifiPasswordEntered(const char* password);
/** \brief Opens IP mode selection menu (DHCP/static). */
static void wifiShowIpModeMenu();
/** \brief Handles IP mode selection. */
static void onWifiIpModeSelect(uint16_t index, void* userData);
/** \brief Opens generic IP-related text input step. */
static void wifiShowIpInputField(const char* title, char* target, size_t targetSize,
                                  T9InputView::SaveCallback onComplete);
/** \brief Validates and stores static IP input. */
static void onWifiStaticIpEntered(const char* ip);
/** \brief Validates and stores gateway input. */
static void onWifiGatewayEntered(const char* gateway);
/** \brief Validates and stores netmask input. */
static void onWifiNetmaskEntered(const char* netmask);
/** \brief Persists wizard config and attempts connection. */
static void wifiFinishSetup();

/**
 * \brief Draws padlock icon for secured Wi-Fi networks.
 * \param gfx Display graphics context.
 * \param x Left position.
 * \param y Top position.
 * \param inverted Whether colors are inverted due to row selection.
 */
static void drawWifiLockIcon(Gdey029T94* gfx, int x, int y, bool inverted) {
    if (!gfx) return;
    uint16_t fg = inverted ? EPD_WHITE : EPD_BLACK;
    uint16_t bg = inverted ? EPD_BLACK : EPD_WHITE;

    gfx->fillRect(x, y + 5, 9, 7, fg);
    gfx->drawRect(x + 2, y, 5, 6, fg);
    gfx->fillRect(x + 3, y + 1, 3, 4, bg);
}

/**
 * \brief Custom list-row renderer for Wi-Fi scan entries.
 * \param gfx Display graphics context.
 * \param item Item metadata and optional scan-result user data.
 * \param index Row index.
 * \param x Row left position.
 * \param y Row top position.
 * \param w Row width.
 * \param h Row height.
 * \param selected Whether row is selected.
 * \param userCtx Optional renderer context.
 * \return Always `true`.
 */
static bool renderWifiRow(Gdey029T94* gfx, const ListItem& item,
                          uint16_t index, int x, int y, int w, int h,
                          bool selected, void* userCtx) {
    (void)index;
    (void)h;
    (void)userCtx;
    if (!gfx) return true;

    gfx->setFont(nullptr);
    gfx->setTextSize(1);

    int baseline = y + 6;

    if (item.userData == nullptr) {
        if (item.label) {
            gfx->setCursor(x + 22, baseline);
            render::printText(gfx, item.label);
        }
        return true;
    }

    const auto* net = reinterpret_cast<const WifiItem*>(item.userData);

    drawSignalBars(gfx, x + 4, baseline - 6, net ? net->rssi : -100, selected);

    const char* ssid = item.label ? item.label : "";
    char ssidDisplay[36];
    strncpy(ssidDisplay, ssid, sizeof(ssidDisplay) - 1);
    ssidDisplay[sizeof(ssidDisplay) - 1] = '\0';

    gfx->setCursor(x + 22, baseline);
    render::printText(gfx, ssidDisplay);

    if (net && net->security != hal::WifiSecurity::OPEN) {
        drawWifiLockIcon(gfx, x + w - 15, baseline - 5, selected);
    }

    return true;
}

/**
 * \brief Rebuilds top-level Wi-Fi menu items and dynamic module extensions.
 */
void rebuildWifiMainMenu() {
    auto* wifi = hal::getWifiControllerInstance();
    bool connected = wifi && wifi->isConnected();
    auto& wifiHandlers = WifiHandlers::instance();
    bool hasConfig = wifiHandlers.config().valid;

    if (connected) {
        s_wifiMainItems[WIFI_IDX_CONNECT] = {ui::tr("core.wifi_disconnect"), '*', false, nullptr};
    } else if (hasConfig) {
        s_wifiMainItems[WIFI_IDX_CONNECT] = {ui::tr("core.wifi_connect"), 0, false, nullptr};
    } else {
        s_wifiMainItems[WIFI_IDX_CONNECT] = {ui::tr("core.wifi_no_config"), 0, true, nullptr};
    }

    s_wifiMainItems[WIFI_IDX_SETUP] = {ui::tr("core.wifi_setup"), 0, false, nullptr};
    s_wifiMainItems[WIFI_IDX_DETAILS] = {ui::tr("core.wifi_details"), 0, false, nullptr};
    s_wifiMainItems[WIFI_IDX_NTP_SYNC] = {ui::tr("core.ntp_sync"), 0, !connected && !hasConfig, nullptr};

    auto& moduleReg = core::ModuleRegistry::instance();
    s_wifiModuleCount = moduleReg.getMenuItems(
        core::MenuLocation::WIFI_MENU,
        s_wifiModuleItems,
        WIFI_MENU_MAX_ITEMS - WIFI_MENU_FIXED_COUNT
    );

    for (uint8_t i = 0; i < s_wifiModuleCount; i++) {
        s_wifiMainItems[WIFI_MENU_FIXED_COUNT + i] = {s_wifiModuleItems[i].label, 0, false, nullptr};
    }

    if (s_wifiMainMenu) {
        s_wifiMainMenu->init(ui::tr("core.wifi_menu"), s_wifiMainItems, WIFI_MENU_FIXED_COUNT + s_wifiModuleCount);
    }
}

/**
 * \brief Shows top-level Wi-Fi menu and reloads stored configuration.
 */
void showWifiMainMenu() {
    WifiHandlers::instance().loadConfig();

    if (!s_wifiMainMenu) {
        s_wifiMainMenu = new ListView();
        s_wifiMainMenu->setOnSelect(onWifiMainSelect);
    }

    rebuildWifiMainMenu();
    ViewStack::instance().push(s_wifiMainMenu);
}

/**
 * \brief Handles top-level Wi-Fi menu actions.
 * \param index Selected item index.
 * \param userData Optional callback user data.
 */
static void onWifiMainSelect(uint16_t index, void* userData) {
    (void)userData;

    switch (index) {
        case WIFI_IDX_CONNECT: {
            auto* wifi = hal::getWifiControllerInstance();
            if (wifi && wifi->isConnected()) {
                wifiDisconnect();
            } else {
                wifiConnect();
            }
            return;
        }
        case WIFI_IDX_SETUP:
            wifiSetup();
            return;
        case WIFI_IDX_DETAILS:
            wifiShowDetails();
            return;
        case WIFI_IDX_NTP_SYNC:
            wifiNtpSync();
            return;
    }

    uint8_t moduleIdx = index - WIFI_MENU_FIXED_COUNT;
    if (moduleIdx < s_wifiModuleCount) {
        auto& item = s_wifiModuleItems[moduleIdx];
        if (item.getView) {
            IView* view = item.getView();
            if (view) {
                ViewStack::instance().push(view);
            }
        }
        rebuildWifiMainMenu();
    }
}

/**
 * \brief Connects to Wi-Fi using current saved configuration.
 */
static void wifiConnect() {
    auto& wifiHandlers = WifiHandlers::instance();

    if (!wifiHandlers.config().valid) {
        showToastError(ui::tr("core.wifi_no_config"));
        return;
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "%s\n%s", ui::tr("core.wifi_connecting"), wifiHandlers.config().ssid);
    showToastInfo(msg, 0);

    bool connected = wifiHandlers.connect();
    ViewStack::instance().hideModal();

    if (connected) {
        auto* wifi = hal::getWifiControllerInstance();
        char ipBuf[20] = {};
        if (wifi) wifi->getIpAddress(ipBuf, sizeof(ipBuf));
        snprintf(msg, sizeof(msg), "%s (IP: %s)", ui::tr("core.wifi_connected"), ipBuf);
        showToastSuccess(msg, TOAST_DURATION_LONG_MS);
    } else {
        snprintf(msg, sizeof(msg), "%s: %s", ui::tr("core.wifi_failed"), wifiHandlers.getLastError());
        showToastError(msg, TOAST_DURATION_LONG_MS);
    }

    rebuildWifiMainMenu();
}

/**
 * \brief Disconnects current Wi-Fi connection and refreshes menu state.
 */
static void wifiDisconnect() {
    WifiHandlers::instance().disconnect();
    showToastInfo(ui::tr("core.wifi_disconnected"));
    rebuildWifiMainMenu();
}

/**
 * \brief Resets setup wizard state and begins network scan.
 */
static void wifiSetup() {
    WifiHandlers::instance().wizard().reset();
    wifiStartScan();
}

/**
 * \brief Sorts scanned networks by signal strength in descending order.
 */
static void sortWifiScanResults() {
    if (s_wifiScanCount < 2) return;
    for (uint8_t i = 0; i < s_wifiScanCount - 1; i++) {
        for (uint8_t j = i + 1; j < s_wifiScanCount; j++) {
            if (s_wifiScanResults[j].rssi > s_wifiScanResults[i].rssi) {
                WifiItem temp = s_wifiScanResults[i];
                s_wifiScanResults[i] = s_wifiScanResults[j];
                s_wifiScanResults[j] = temp;
            }
        }
    }
}

/**
 * \brief Performs active Wi-Fi scan and opens scan result list view.
 */
static void wifiStartScan() {
    auto* wifi = hal::getWifiControllerInstance();
    if (!wifi) {
        showToastError(ui::tr("core.hw_not_available"));
        return;
    }

    if (!wifi->isEnabled()) {
        wifi->enable(hal::WifiMode::STA);
    }

    showToastInfo(ui::tr("core.wifi_scanning"), 0);

    s_wifiScanCount = 0;
    if (wifi->startScan()) {
        uint32_t startMs = esp_timer_get_time() / 1000;
        while (!wifi->isScanComplete()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if ((esp_timer_get_time() / 1000 - startMs) > WIFI_SCAN_TIMEOUT_MS) break;
        }

        hal::WifiScanResult rawResults[WIFI_MAX_NETWORKS];
        uint8_t rawCount = wifi->getScanResults(rawResults, WIFI_MAX_NETWORKS);

        for (uint8_t i = 0; i < rawCount && s_wifiScanCount < WIFI_MAX_NETWORKS; i++) {
            if (rawResults[i].ssid[0] == '\0') continue;

            bool found = false;
            for (uint8_t j = 0; j < s_wifiScanCount; j++) {
                if (strcmp(s_wifiScanResults[j].ssid, rawResults[i].ssid) == 0) {
                    if (rawResults[i].rssi > s_wifiScanResults[j].rssi) {
                        s_wifiScanResults[j].rssi = rawResults[i].rssi;
                        s_wifiScanResults[j].security = rawResults[i].security;
                    }
                    found = true;
                    break;
                }
            }

            if (!found) {
                strncpy(s_wifiScanResults[s_wifiScanCount].ssid, rawResults[i].ssid, 32);
                s_wifiScanResults[s_wifiScanCount].ssid[32] = '\0';
                s_wifiScanResults[s_wifiScanCount].rssi = rawResults[i].rssi;
                s_wifiScanResults[s_wifiScanCount].security = rawResults[i].security;
                s_wifiScanCount++;
            }
        }
    }

    sortWifiScanResults();

    ViewStack::instance().hideModal();

    if (!s_wifiScanView) {
        s_wifiScanView = new ListView();
        s_wifiScanView->setOnSelect(onWifiScanSelect);
        s_wifiScanView->setItemRenderer(renderWifiRow, nullptr);
    }

    snprintf(s_wifiManualLabel, sizeof(s_wifiManualLabel), "+ %s", ui::tr("core.wifi_add_manual"));
    s_wifiScanItems[0] = {s_wifiManualLabel, 0, false, nullptr};

    uint8_t itemCount = s_wifiScanCount + 1;
    for (uint8_t i = 0; i < s_wifiScanCount; i++) {
        s_wifiScanItems[i + 1] = {s_wifiScanResults[i].ssid, 0, false, &s_wifiScanResults[i]};
    }

    s_wifiScanView->init(ui::tr("core.wifi_setup"), s_wifiScanItems, itemCount);
    s_wifiScanView->setHint(ui::tr("core.hint_select"));
    ViewStack::instance().push(s_wifiScanView);
}

/**
 * \brief Handles selected scan entry and advances setup wizard.
 * \param index Selected list index.
 * \param userData Optional pointer to selected `WifiItem`.
 */
static void onWifiScanSelect(uint16_t index, void* userData) {
    auto& wizard = WifiHandlers::instance().wizard();
    const auto* item = reinterpret_cast<const WifiItem*>(userData);

    if (index == 0 || item == nullptr) {
        wizard.fromScan = false;
        showT9Input(ui::tr("core.wifi_ssid"), "", [](const char* ssid) {
            strncpy(WifiHandlers::instance().wizard().ssid, ssid,
                    sizeof(WifiHandlers::instance().wizard().ssid) - 1);
            wifiShowAuthMenu();
        }, 32);
        return;
    }

    wizard.fromScan = true;
    strncpy(wizard.ssid, item->ssid, sizeof(wizard.ssid) - 1);
    wizard.security = item->security;

    if (wizard.security == hal::WifiSecurity::OPEN) {
        wizard.password[0] = '\0';
        wifiShowIpModeMenu();
    } else {
        wifiShowPasswordInput();
    }
}

/**
 * \brief Shows Wi-Fi authentication mode menu.
 */
static void wifiShowAuthMenu() {
    if (!s_wifiAuthMenu) {
        s_wifiAuthMenu = new ListView();
        s_wifiAuthMenu->setOnSelect(onWifiAuthSelect);

        for (uint8_t i = 0; i < WIFI_AUTH_COUNT; i++) {
            s_wifiAuthItems[i] = {s_wifiAuthLabels[i], 0, false, nullptr};
        }
    }

    s_wifiAuthMenu->init(ui::tr("core.wifi_encryption"), s_wifiAuthItems, WIFI_AUTH_COUNT);
    ViewStack::instance().push(s_wifiAuthMenu);
}

/**
 * \brief Stores selected authentication mode and continues wizard.
 * \param index Selected authentication mode index.
 * \param userData Optional callback user data.
 */
static void onWifiAuthSelect(uint16_t index, void* userData) {
    (void)userData;
    auto& wizard = WifiHandlers::instance().wizard();

    switch (index) {
        case WIFI_AUTH_WPA2:     wizard.security = hal::WifiSecurity::WPA2_PSK; break;
        case WIFI_AUTH_WPA_WPA2: wizard.security = hal::WifiSecurity::WPA2_PSK; break;
        case WIFI_AUTH_WPA3:     wizard.security = hal::WifiSecurity::WPA3_PSK; break;
        case WIFI_AUTH_WPA:      wizard.security = hal::WifiSecurity::WPA_PSK; break;
        case WIFI_AUTH_OPEN:     wizard.security = hal::WifiSecurity::OPEN; break;
        case WIFI_AUTH_WEP:      wizard.security = hal::WifiSecurity::WEP; break;
    }

    if (wizard.security == hal::WifiSecurity::OPEN) {
        wizard.password[0] = '\0';
        wifiShowIpModeMenu();
    } else {
        wifiShowPasswordInput();
    }
}

/**
 * \brief Opens password input view for non-open Wi-Fi networks.
 */
static void wifiShowPasswordInput() {
    showT9Input(ui::tr("core.wifi_password"), "", onWifiPasswordEntered, 64);
}

/**
 * \brief Stores entered Wi-Fi password and proceeds to IP-mode step.
 * \param password Entered Wi-Fi password.
 */
static void onWifiPasswordEntered(const char* password) {
    auto& wizard = WifiHandlers::instance().wizard();
    strncpy(wizard.password, password, sizeof(wizard.password) - 1);
    wifiShowIpModeMenu();
}

/**
 * \brief Shows DHCP/static IP selection menu.
 */
static void wifiShowIpModeMenu() {
    if (!s_wifiIpMenu) {
        s_wifiIpMenu = new ListView();
        s_wifiIpMenu->setOnSelect(onWifiIpModeSelect);
    }

    s_wifiIpItems[WIFI_IP_DHCP] = {ui::tr("core.wifi_dhcp"), 0, false, nullptr};
    s_wifiIpItems[WIFI_IP_STATIC] = {ui::tr("core.wifi_static"), 0, false, nullptr};

    s_wifiIpMenu->init(ui::tr("core.wifi_ip_mode"), s_wifiIpItems, WIFI_IP_COUNT);
    ViewStack::instance().push(s_wifiIpMenu);
}

/**
 * \brief Handles IP-mode choice and proceeds with DHCP or static inputs.
 * \param index Selected IP-mode index.
 * \param userData Optional callback user data.
 */
static void onWifiIpModeSelect(uint16_t index, void* userData) {
    (void)userData;
    auto& wizard = WifiHandlers::instance().wizard();

    wizard.useDhcp = (index == WIFI_IP_DHCP);

    if (wizard.useDhcp) {
        wifiFinishSetup();
    } else {
        wifiShowIpInputField("IP", wizard.staticIp, sizeof(wizard.staticIp), onWifiStaticIpEntered);
    }
}

/**
 * \brief Opens generic text input for one static-IP field.
 * \param title Input prompt title.
 * \param target Initial field value.
 * \param targetSize Size of `target` in bytes.
 * \param onComplete Callback invoked with entered text.
 */
static void wifiShowIpInputField(const char* title, char* target, size_t targetSize,
                                  T9InputView::SaveCallback onComplete) {
    (void)targetSize;
    showT9Input(title, target, onComplete, 15);
}

/**
 * \brief Validates and stores static IP value.
 * \param ip Entered IP address string.
 */
static void onWifiStaticIpEntered(const char* ip) {
    auto& wizard = WifiHandlers::instance().wizard();
    if (!WifiHandlers::isValidIpAddress(ip)) {
        showToastError("Invalid IP", TOAST_DURATION_MEDIUM_MS);
        wifiShowIpInputField("IP", wizard.staticIp, sizeof(wizard.staticIp), onWifiStaticIpEntered);
        return;
    }
    strncpy(wizard.staticIp, ip, sizeof(wizard.staticIp) - 1);
    wifiShowIpInputField(ui::tr("core.wifi_gateway"), wizard.gateway, sizeof(wizard.gateway), onWifiGatewayEntered);
}

/**
 * \brief Validates and stores gateway value.
 * \param gateway Entered gateway IP string.
 */
static void onWifiGatewayEntered(const char* gateway) {
    auto& wizard = WifiHandlers::instance().wizard();
    if (!WifiHandlers::isValidIpAddress(gateway)) {
        showToastError("Invalid IP", TOAST_DURATION_MEDIUM_MS);
        wifiShowIpInputField(ui::tr("core.wifi_gateway"), wizard.gateway, sizeof(wizard.gateway), onWifiGatewayEntered);
        return;
    }
    strncpy(wizard.gateway, gateway, sizeof(wizard.gateway) - 1);
    wifiShowIpInputField(ui::tr("core.wifi_netmask"), wizard.netmask, sizeof(wizard.netmask), onWifiNetmaskEntered);
}

/**
 * \brief Validates and stores netmask value.
 * \param netmask Entered netmask string.
 */
static void onWifiNetmaskEntered(const char* netmask) {
    auto& wizard = WifiHandlers::instance().wizard();
    if (!WifiHandlers::isValidIpAddress(netmask)) {
        showToastError("Invalid IP", TOAST_DURATION_MEDIUM_MS);
        wifiShowIpInputField(ui::tr("core.wifi_netmask"), wizard.netmask, sizeof(wizard.netmask), onWifiNetmaskEntered);
        return;
    }
    strncpy(wizard.netmask, netmask, sizeof(wizard.netmask) - 1);
    wifiFinishSetup();
}

/**
 * \brief Saves completed wizard configuration and connects to Wi-Fi.
 */
static void wifiFinishSetup() {
    WifiHandlers::instance().saveConfig();

    while (ViewStack::instance().current() != s_wifiMainMenu &&
           ViewStack::instance().depth() > 1) {
        ViewStack::instance().pop();
    }

    wifiConnect();
}

/**
 * \brief Builds and shows Wi-Fi status/details information view.
 */
static void wifiShowDetails() {
    auto* wifi = hal::getWifiControllerInstance();
    auto& wifiHandlers = WifiHandlers::instance();
    char* info = s_wifiDetailsBuf;
    memset(info, 0, WIFI_DETAILS_BUF_SIZE);
    size_t pos = 0;

    auto append = [&](const char* fmt, ...) {
        if (pos >= WIFI_DETAILS_BUF_SIZE) return;
        va_list args;
        va_start(args, fmt);
        int written = vsnprintf(info + pos, WIFI_DETAILS_BUF_SIZE - pos, fmt, args);
        va_end(args);
        if (written > 0) {
            size_t w = static_cast<size_t>(written);
            pos += (w < (WIFI_DETAILS_BUF_SIZE - pos)) ? w : (WIFI_DETAILS_BUF_SIZE - pos - 1);
        }
    };

    if (wifi && wifi->isConnected()) {
        char ipBuf[20] = {};
        uint8_t mac[6] = {};

        append("Status: %s\n\n", ui::tr("core.wifi_connected"));
        append("SSID: %s\n", wifi->getCurrentSsid());

        if (wifi->getMacAddress(mac)) {
            append("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }

        if (wifi->getIpAddress(ipBuf, sizeof(ipBuf))) {
            append("IP: %s\n", ipBuf);
        }

        append("%s: %d dBm\n", ui::tr("core.wifi_signal"), wifi->getRssi());

    } else if (wifiHandlers.config().valid) {
        append("Status: %s\n\n", ui::tr("core.wifi_disconnected"));
        append("=== %s ===\n", ui::tr("core.wifi_saved_config"));
        append("SSID: %s\n", wifiHandlers.config().ssid);
        append("IP: %s\n", wifiHandlers.config().useDhcp ? "DHCP" : "Static");
    } else {
        append("%s", ui::tr("core.wifi_no_config"));
    }

    showInfo(ui::tr("core.wifi_details"), info);
}

/**
 * \brief Triggers NTP synchronization, connecting Wi-Fi first if required.
 */
static void wifiNtpSync() {
    auto& wifiHandlers = WifiHandlers::instance();

    if (!wifiHandlers.config().valid && !wifiHandlers.isConnected()) {
        showToastError(ui::tr("core.wifi_no_config"));
        return;
    }

    bool wasConnected = wifiHandlers.isConnected();

    if (!wasConnected) {
        showToastTask(ui::tr("core.wifi_connecting"), 0);
        if (!wifiHandlers.connect()) {
            ViewStack::instance().hideModal();
            showToastError(ui::tr("core.wifi_failed"));
            return;
        }
        ViewStack::instance().hideModal();
    }

    showToastTask(ui::tr("core.ntp_syncing"), 0);

    bool synced = wifiHandlers.syncNtp();
    ViewStack::instance().hideModal();

    if (synced) {
        showToastSuccess(ui::tr("core.ntp_success"));
    } else {
        showToastError(ui::tr("core.ntp_timeout"));
    }
}

} // namespace cdc::ui
