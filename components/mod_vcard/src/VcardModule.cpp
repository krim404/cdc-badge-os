#include "mod_vcard/VcardModule.h"
#include "mod_vcard/VcardWizard.h"
#include "mod_vcard/ble_vcard.h"
#include "mod_vcard/vcard_store.h"
#include "serial_cmd/ICommandRegistry.h"
#include "serial_cmd/SubCommand.h"
#include "serial_cmd/Console.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/EventBus.h"
#include "cdc_core/Raii.h"
#include "cdc_ui/BackupImport.h"
#include "cJSON.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ListView.h"
#include "cdc_views/InfoView.h"
#include "cdc_views/ConfirmView.h"
#include "cdc_views/QRCodeView.h"
#include "esp_timer.h"
#include "cdc_views/ToastView.h"
#include "cdc_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdio>

static const char* TAG = "VCARD";

namespace cdc::mod_vcard {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_vcard.title",            "vCards"},          // 0  STR_VCARD
    {"mod_vcard.my_vcard",         "My vCard"},        // 1  STR_MY_VCARD
    {"mod_vcard.nearby",           "Nearby"},          // 2  STR_NEARBY
    {"mod_vcard.scan",             "Start Scan"},      // 3  STR_SCAN
    {"mod_vcard.stop_scan",        "Stop Scan"},       // 4  STR_STOP_SCAN
    {"mod_vcard.advertising",      "Start Advertising"},// 5 STR_ADVERTISING
    {"mod_vcard.stop_adv",         "Stop Advertising"},// 6  STR_STOP_ADV
    {"mod_vcard.exchange",         "Exchange"},        // 7  STR_EXCHANGE
    {"mod_vcard.no_peers",         "No peers found"},  // 8  STR_NO_PEERS
    {"mod_vcard.scanning",         "Scanning..."},     // 9  STR_SCANNING
    {"mod_vcard.exchange_req",     "Exchange Request"},// 10 STR_EXCHANGE_REQ
    {"mod_vcard.accept",           "Accept"},          // 11 STR_ACCEPT
    {"mod_vcard.decline",          "Decline"},         // 12 STR_DECLINE
    {"mod_vcard.exchange_ok",      "Exchange successful"},// 13 STR_EXCHANGE_OK
    {"mod_vcard.exchange_fail",    "Exchange failed"}, // 14 STR_EXCHANGE_FAIL
    {"mod_vcard.connecting",       "Connecting..."},   // 15 STR_CONNECTING
    {"mod_vcard.edit_my_vcard",    "Edit my vCard"},   // 16 STR_EDIT_MY_VCARD
    {"mod_vcard.no_vcard",         "No vCard set"},    // 17 STR_NO_VCARD
    {"mod_vcard.saved",            "Saved"},           // 18 STR_SAVED
    {"mod_vcard.given_name",       "First name"},      // 19 STR_GIVEN_NAME
    {"mod_vcard.family_name",      "Last name"},       // 20 STR_FAMILY_NAME
    {"mod_vcard.formatted_name",   "Display name"},    // 21 STR_FORMATTED_NAME
    {"mod_vcard.organization",     "Organization"},    // 22 STR_ORGANIZATION
    {"mod_vcard.position",         "Position"},        // 23 STR_POSITION
    {"mod_vcard.email",            "Email"},           // 24 STR_EMAIL
    {"mod_vcard.tel_cell",         "Phone (Mobile)"},  // 25 STR_TEL_CELL
    {"mod_vcard.tel_home",         "Phone (Home)"},    // 26 STR_TEL_HOME
    {"mod_vcard.tel_work",         "Phone (Work)"},    // 27 STR_TEL_WORK
    {"mod_vcard.url",              "Website"},         // 28 STR_URL
    {"mod_vcard.telegram",         "Telegram"},        // 29 STR_TELEGRAM
    {"mod_vcard.signal",           "Signal"},          // 30 STR_SIGNAL
    {"mod_vcard.matrix",           "Matrix"},          // 31 STR_MATRIX
    {"mod_vcard.threema",          "Threema"},         // 32 STR_THREEMA
    {"mod_vcard.social_profile",   "Social Profile"},  // 33 STR_SOCIAL_PROFILE
    {"mod_vcard.note",             "Note"},            // 34 STR_NOTE
};

// Numeric offsets retained because VcardWizard takes an offset-array + resolver.
static constexpr uint16_t STR_VCARD = 0;
static constexpr uint16_t STR_MY_VCARD = 1;
static constexpr uint16_t STR_NEARBY = 2;
static constexpr uint16_t STR_SCAN = 3;
static constexpr uint16_t STR_STOP_SCAN = 4;
static constexpr uint16_t STR_ADVERTISING = 5;
static constexpr uint16_t STR_STOP_ADV = 6;
static constexpr uint16_t STR_EXCHANGE = 7;
static constexpr uint16_t STR_NO_PEERS = 8;
static constexpr uint16_t STR_SCANNING = 9;
static constexpr uint16_t STR_EXCHANGE_REQ = 10;
static constexpr uint16_t STR_ACCEPT = 11;
static constexpr uint16_t STR_DECLINE = 12;
static constexpr uint16_t STR_EXCHANGE_OK = 13;
static constexpr uint16_t STR_EXCHANGE_FAIL = 14;
static constexpr uint16_t STR_CONNECTING = 15;
static constexpr uint16_t STR_EDIT_MY_VCARD = 16;
static constexpr uint16_t STR_NO_VCARD = 17;
static constexpr uint16_t STR_SAVED = 18;
static constexpr uint16_t STR_GIVEN_NAME = 19;
static constexpr uint16_t STR_FAMILY_NAME = 20;
static constexpr uint16_t STR_FORMATTED_NAME = 21;
static constexpr uint16_t STR_ORGANIZATION = 22;
static constexpr uint16_t STR_POSITION = 23;
static constexpr uint16_t STR_EMAIL = 24;
static constexpr uint16_t STR_TEL_CELL = 25;
static constexpr uint16_t STR_TEL_HOME = 26;
static constexpr uint16_t STR_TEL_WORK = 27;
static constexpr uint16_t STR_URL = 28;
static constexpr uint16_t STR_TELEGRAM = 29;
static constexpr uint16_t STR_SIGNAL = 30;
static constexpr uint16_t STR_MATRIX = 31;
static constexpr uint16_t STR_THREEMA = 32;
static constexpr uint16_t STR_SOCIAL_PROFILE = 33;
static constexpr uint16_t STR_NOTE = 34;

static const uint16_t s_wizardStepOffsets[16] = {
    STR_GIVEN_NAME, STR_FAMILY_NAME, STR_FORMATTED_NAME, STR_ORGANIZATION,
    STR_POSITION, STR_EMAIL, STR_TEL_CELL, STR_TEL_HOME, STR_TEL_WORK,
    STR_URL, STR_TELEGRAM, STR_SIGNAL, STR_MATRIX, STR_THREEMA,
    STR_SOCIAL_PROFILE, STR_NOTE,
};

static const char* mstr(uint16_t offset) {
    if (offset >= std::size(kStrings)) return "?";
    return ui::tr(kStrings[offset].key);
}

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

/**
 * \brief View instances used by vCard module UI flow.
 */
static ui::ListView s_mainMenu;
static ui::ListView s_peerList;
static ui::InfoView s_consentView;
static bool s_viewsInitialized = false;

/**
 * \brief Peer discovery storage used for UI list rendering.
 */
static constexpr uint16_t MAX_UI_PEERS = 16;
EXT_RAM_BSS_ATTR static vcard_peer_t s_uiPeers[MAX_UI_PEERS] = {};
static uint16_t s_uiPeerCount = 0;
static ui::ListItem s_peerItems[MAX_UI_PEERS + 1] = {};
static char s_peerLabels[MAX_UI_PEERS][48] = {};

/**
 * \brief Main-menu item identifiers.
 */
enum MainMenuItem {
    MENU_MY_VCARD = 0,
    MENU_EDIT_MY_VCARD,
    MENU_NEARBY,
    MENU_SCAN_TOGGLE,
    MENU_ADV_TOGGLE,
    MENU_COUNT
};
static ui::ListItem s_mainMenuItems[MENU_COUNT] = {};

static void rebuildMainMenu();
static void onMainMenuSelect(uint16_t index, void* userData);
static void rebuildPeerList();
static void onPeerSelect(uint16_t index, void* userData);

/**
 * \brief Handles user acceptance of incoming vCard transfer consent.
 * \param userData Optional callback context (unused).
 * \return void
 */
static void onConsentAccept(void* userData) {
    (void)userData;
    ble_vcard_respond_consent(true);
    ui::ViewStack::instance().pop();
    ui::showToastInfo("Waiting for vCard...");
}

/**
 * \brief Handles user decline of incoming vCard exchange request.
 * \param userData Optional callback context (unused).
 */
static void onConsentDecline(void* userData) {
    (void)userData;
    ble_vcard_respond_consent(false);
    ui::ViewStack::instance().pop();
}

// Consent and exchange-complete callbacks fire on the nimble_host task; the UI
// work must run on the main task. Requests are parked here under a mutex and
// deferred via BLE_CONSENT_REQUEST / BLE_EXCHANGE_COMPLETE.
struct PendingConsent {
    bool valid = false;
    char peerName[32] = {0};
};
struct PendingExchange {
    bool valid = false;
    bool success = false;
    char error[64] = {0};
};
static PendingConsent s_pendingConsent;
static PendingExchange s_pendingExchange;
static SemaphoreHandle_t s_vcardBleMutex = nullptr;

/**
 * \brief Incoming consent request, invoked on the nimble_host task.
 * \param peerName Remote peer display name.
 */
static void onConsentRequest(const char* peerName) {
    {
        core::MutexGuard guard(s_vcardBleMutex);
        s_pendingConsent.valid = true;
        if (peerName) {
            strncpy(s_pendingConsent.peerName, peerName, sizeof(s_pendingConsent.peerName) - 1);
            s_pendingConsent.peerName[sizeof(s_pendingConsent.peerName) - 1] = '\0';
        } else {
            s_pendingConsent.peerName[0] = '\0';
        }
    }
    core::EventBus::instance().publish(core::EventType::BLE_CONSENT_REQUEST);
}

/**
 * \brief Main-task handler that displays the consent prompt.
 * \param evt Unused; the request is read from s_pendingConsent.
 */
static void onConsentRequestEvent(const core::Event& evt) {
    (void)evt;
    PendingConsent req;
    {
        core::MutexGuard guard(s_vcardBleMutex);
        req = s_pendingConsent;
        s_pendingConsent.valid = false;
    }
    if (!req.valid) return;

    static char promptText[256];
    snprintf(promptText, sizeof(promptText),
             "%s\n\n%s\nmoechte vCard tauschen\n\n[Y] %s\n[N] %s",
             mstr(STR_EXCHANGE_REQ),
             req.peerName,
             mstr(STR_ACCEPT),
             mstr(STR_DECLINE));

    s_consentView.init(mstr(STR_EXCHANGE_REQ), promptText);
    s_consentView.setYesNoCallbacks(onConsentAccept, onConsentDecline, nullptr);
    ui::ViewStack::instance().push(&s_consentView);
}

/**
 * \brief Exchange-completion callback, invoked on the nimble_host task.
 * \param success `true` when exchange succeeded.
 * \param error Optional error text on failure.
 */
static void onExchangeComplete(bool success, const char* error) {
    {
        core::MutexGuard guard(s_vcardBleMutex);
        s_pendingExchange.valid = true;
        s_pendingExchange.success = success;
        if (!success && error && error[0]) {
            strncpy(s_pendingExchange.error, error, sizeof(s_pendingExchange.error) - 1);
            s_pendingExchange.error[sizeof(s_pendingExchange.error) - 1] = '\0';
        } else {
            s_pendingExchange.error[0] = '\0';
        }
    }
    core::EventBus::instance().publish(core::EventType::BLE_EXCHANGE_COMPLETE);
}

/**
 * \brief Main-task handler that shows the exchange-completion toast.
 * \param evt Unused; the result is read from s_pendingExchange.
 */
static void onExchangeCompleteEvent(const core::Event& evt) {
    (void)evt;
    PendingExchange req;
    {
        core::MutexGuard guard(s_vcardBleMutex);
        req = s_pendingExchange;
        s_pendingExchange.valid = false;
    }
    if (!req.valid) return;

    if (req.success) {
        ui::showToastSuccess(mstr(STR_EXCHANGE_OK));
    } else if (req.error[0]) {
        ui::showToastError(req.error);
    } else {
        ui::showToastError(mstr(STR_EXCHANGE_FAIL));
    }
}

/**
 * \brief Rebuilds vCard main menu items from current BLE state.
 */
static void rebuildMainMenu() {
    bool scanning = ble_vcard_is_scan_active();
    bool advertising = ble_vcard_is_adv_active();

    s_mainMenuItems[MENU_MY_VCARD]      = {mstr(STR_MY_VCARD),         0, false, nullptr};
    s_mainMenuItems[MENU_EDIT_MY_VCARD] = {mstr(STR_EDIT_MY_VCARD),    0, false, nullptr};
    s_mainMenuItems[MENU_NEARBY]        = {mstr(STR_NEARBY),           0, false, nullptr};
    s_mainMenuItems[MENU_SCAN_TOGGLE]   = {
        scanning ? mstr(STR_STOP_SCAN) : mstr(STR_SCAN),
        0, false, nullptr
    };
    s_mainMenuItems[MENU_ADV_TOGGLE]    = {
        advertising ? mstr(STR_STOP_ADV) : mstr(STR_ADVERTISING),
        0, false, nullptr
    };

    s_mainMenu.init(mstr(STR_VCARD), s_mainMenuItems, MENU_COUNT);
}

/**
 * \brief Handles main-menu actions for local and nearby vCard operations.
 * \param index Selected menu index.
 * \param userData Optional callback context (unused).
 */
static void onMainMenuSelect(uint16_t index, void* userData) {
    (void)userData;

    switch (index) {
        case MENU_MY_VCARD: {
            static EXT_RAM_BSS_ATTR char vcardText[VCARD_MAX_LEN + 1];
            size_t len = vcard_store_get_own(vcardText, sizeof(vcardText));
            if (len > 0) {
                static ui::InfoView infoView;
                infoView.init(mstr(STR_MY_VCARD), vcardText);
                ui::ViewStack::instance().push(&infoView);
            } else {
                ui::showToastInfo(mstr(STR_NO_VCARD));
            }
            break;
        }

        case MENU_EDIT_MY_VCARD:
            if (vcard_store_has_own()) {
                VcardWizard::edit(&s_mainMenu);
            } else {
                VcardWizard::start(&s_mainMenu);
            }
            break;

        case MENU_NEARBY:
            rebuildPeerList();
            ui::ViewStack::instance().push(&s_peerList);
            break;

        case MENU_SCAN_TOGGLE:
            if (ble_vcard_is_scan_active()) {
                ble_vcard_set_scan_enabled(false);
                ui::showToastInfo("Scan stopped");
            } else {
                ble_vcard_set_scan_enabled(true);
                ui::showToastInfo(mstr(STR_SCANNING));
            }
            rebuildMainMenu();
            break;

        case MENU_ADV_TOGGLE:
            if (ble_vcard_is_adv_active()) {
                ble_vcard_set_adv_enabled(false);
                ui::showToastInfo("Advertising stopped");
            } else {
                ble_vcard_set_adv_enabled(true);
                ui::showToastInfo("Advertising started");
            }
            rebuildMainMenu();
            break;
    }
}

/**
 * \brief Rebuilds nearby-peer list from BLE discovery cache.
 */
static void rebuildPeerList() {
    s_uiPeerCount = ble_vcard_get_peers(s_uiPeers, MAX_UI_PEERS);

    if (s_uiPeerCount == 0) {
        s_peerItems[0] = {mstr(STR_NO_PEERS), 0, true, nullptr};
        s_peerList.init(mstr(STR_NEARBY), s_peerItems, 1);
        return;
    }

    for (uint16_t i = 0; i < s_uiPeerCount; i++) {
        snprintf(s_peerLabels[i], sizeof(s_peerLabels[i]),
                 "%s (%ddBm)", s_uiPeers[i].name, s_uiPeers[i].rssi);
        s_peerItems[i] = {s_peerLabels[i], 0, false, reinterpret_cast<void*>(static_cast<uintptr_t>(i))};
    }

    s_peerList.init(mstr(STR_NEARBY), s_peerItems, s_uiPeerCount);
}

/**
 * \brief Starts exchange with selected nearby peer.
 * \param index Selected peer index.
 * \param userData Optional callback context (unused).
 */
static void onPeerSelect(uint16_t index, void* userData) {
    (void)userData;

    if (index >= s_uiPeerCount) return;

    vcard_peer_t& peer = s_uiPeers[index];

    if (ble_vcard_exchange_with(peer.addr, peer.addr_type)) {
        ui::showToastInfo(mstr(STR_CONNECTING));
    } else {
        ui::showToastError("Exchange failed");
    }
}

// ============================================================================
// Lock-screen quick action: show own vCard as a QR code.
// ============================================================================

/**
 * \brief Returns the localized label for the lock-screen quick action.
 */
static const char* getMyVcardLockscreenLabel() {
    return mstr(STR_MY_VCARD);
}

/**
 * \brief Lock-screen quick action: shows the own vCard as a QR code.
 *        Falls back to a toast when no vCard has been configured yet.
 */
static void onMyVcardLockscreenSelect() {
    static EXT_RAM_BSS_ATTR char s_qrBuf[VCARD_MAX_LEN + 1];
    size_t len = vcard_store_get_own(s_qrBuf, sizeof(s_qrBuf));
    if (len == 0) {
        ui::showToastError(mstr(STR_NO_VCARD));
        return;
    }

    static EXT_RAM_BSS_ATTR vcard_data_t s_parsed;
    static char s_qrTitle[96];
    static char s_qrSubtitle[96];

    memset(&s_parsed, 0, sizeof(s_parsed));
    vcard_parse_to_struct(s_qrBuf, &s_parsed);

    if (s_parsed.formatted_name[0]) {
        snprintf(s_qrTitle, sizeof(s_qrTitle), "%s", s_parsed.formatted_name);
    } else if (s_parsed.given_name[0] || s_parsed.family_name[0]) {
        snprintf(s_qrTitle, sizeof(s_qrTitle), "%s %s",
                 s_parsed.given_name, s_parsed.family_name);
    } else {
        snprintf(s_qrTitle, sizeof(s_qrTitle), "%s", mstr(STR_MY_VCARD));
    }

    const char* sub = s_parsed.organization[0] ? s_parsed.organization
                    : s_parsed.title[0]        ? s_parsed.title
                    : s_parsed.email[0]        ? s_parsed.email
                    : "";
    snprintf(s_qrSubtitle, sizeof(s_qrSubtitle), "%s", sub);

    ui::showQRCode(s_qrBuf, s_qrTitle, s_qrSubtitle[0] ? s_qrSubtitle : nullptr);
}

// ============================================================================
// Serial Commands (VCARD_SET / VCARD_GET / VCARD_DELETE)
// ============================================================================

EXT_RAM_BSS_ATTR static char s_vcardBuf[VCARD_MAX_LEN + 64];
static int  s_vcardBufPos = 0;
static bool s_vcardInputMode = false;
static esp_timer_handle_t s_vcardIdleTimer = nullptr;
// Cancel a stalled paste session after this many seconds of inactivity so a
// crashed/interrupted client cannot lock the serial console forever.
static constexpr int64_t VCARD_IDLE_LIMIT_US = 30 * 1000000LL;

static void vcard_session_clear() {
    s_vcardInputMode = false;
    s_vcardBufPos = 0;
    memset(s_vcardBuf, 0, sizeof(s_vcardBuf));
    serial::getCommandRegistry().setLineInterceptor(nullptr);
    if (s_vcardIdleTimer) esp_timer_stop(s_vcardIdleTimer);
}

static void vcard_idle_fired(void*) {
    if (!s_vcardInputMode) return;
    serial::Console::printf("\r\nERROR: vCard paste timed out\r\n");
    vcard_session_clear();
    serial::Console::showPrompt();
}

static void vcard_arm_idle_timer() {
    if (!s_vcardIdleTimer) {
        esp_timer_create_args_t args = {
            .callback = vcard_idle_fired,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vcard_idle",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &s_vcardIdleTimer);
    }
    esp_timer_stop(s_vcardIdleTimer);
    esp_timer_start_once(s_vcardIdleTimer, VCARD_IDLE_LIMIT_US);
}

/**
 * \brief Intercepts multiline vCard paste input, accumulates lines, and stops at `"---"`.
 * \param line Incoming text line.
 * \return `true` to keep interception active, `false` to stop interception.
 */
static bool vcardLineInterceptor(const char* line) {
    if (!s_vcardInputMode) return false;

    using Console = serial::Console;
    vcard_arm_idle_timer();

    if (strncmp(line, "---", 3) == 0) {
        s_vcardBuf[s_vcardBufPos] = '\0';

        char err[64] = {};
        if (vcard_store_set_own(s_vcardBuf, static_cast<size_t>(s_vcardBufPos), err, sizeof(err))) {
            Console::printf("OK: vCard updated\r\n");
        } else {
            Console::printf("ERROR: %s\r\n", err[0] ? err : "Invalid vCard");
        }

        vcard_session_clear();
        Console::showPrompt();
        return true;
    }

    if (strcmp(line, "ABORT") == 0 || strcmp(line, "VCARD_ABORT") == 0) {
        Console::printf("ABORTED\r\n");
        vcard_session_clear();
        Console::showPrompt();
        return true;
    }

    size_t lineLen = strlen(line);
    if (s_vcardBufPos + static_cast<int>(lineLen) + 2 < static_cast<int>(sizeof(s_vcardBuf))) {
        memcpy(s_vcardBuf + s_vcardBufPos, line, lineLen);
        s_vcardBufPos += static_cast<int>(lineLen);
        s_vcardBuf[s_vcardBufPos++] = '\n';
    } else {
        Console::printf("ERROR: vCard too large\r\n");
        vcard_session_clear();
        Console::showPrompt();
    }
    return true;
}

/**
 * \brief Serial command entering multiline vCard paste mode.
 * \param args Unused command arguments.
 */
static void cmdVcardSet(const char* args) {
    (void)args;
    serial::Console::printf("Paste vCard 4.0, end with '---' on a new line "
                            "(or 'ABORT' to cancel):\r\n");
    s_vcardBufPos = 0;
    s_vcardInputMode = true;
    serial::getCommandRegistry().setLineInterceptor(vcardLineInterceptor);
    vcard_arm_idle_timer();
}

/**
 * \brief Serial command printing stored vCard or template.
 * \param args Unused command arguments.
 */
static void cmdVcardGet(const char* args) {
    (void)args;
    using Console = serial::Console;

    char out[VCARD_MAX_LEN + 1];
    size_t len = vcard_store_get_own(out, sizeof(out));

    if (len == 0) {
        Console::printf("BEGIN:VCARD\r\n");
        Console::printf("VERSION:4.0\r\n");
        Console::printf("N:;;\r\n");
        Console::printf("FN:\r\n");
        Console::printf("NOTE:\r\n");
        Console::printf("TEL;TYPE=HOME:\r\n");
        Console::printf("TEL;TYPE=CELL:\r\n");
        Console::printf("TEL;TYPE=WORK:\r\n");
        Console::printf("EMAIL:\r\n");
        Console::printf("URL:\r\n");
        Console::printf("ORG:\r\n");
        Console::printf("TITLE:\r\n");
        Console::printf("X-SOCIALPROFILE:\r\n");
        Console::printf("IMPP:telegram:\r\n");
        Console::printf("IMPP:signal:\r\n");
        Console::printf("IMPP:matrix:\r\n");
        Console::printf("IMPP:threema:\r\n");
        Console::printf("END:VCARD\r\n");
        return;
    }

    char* line = out;
    char* next;
    while ((next = strchr(line, '\n')) != nullptr) {
        *next = '\0';
        Console::printf("%s\r\n", line);
        line = next + 1;
    }
    if (*line) {
        Console::printf("%s\r\n", line);
    }
}

/**
 * \brief Serial command deleting stored local vCard.
 * \param args Unused command arguments.
 */
static void cmdVcardDelete(const char* args) {
    (void)args;
    if (vcard_store_clear_own()) {
        serial::Console::printf("OK: vCard deleted\r\n");
    } else {
        serial::Console::printf("ERROR: Failed to delete vCard\r\n");
    }
}

static const serial::SubCommand kVcardSubs[] = {
    {"SET",    "", "Set own vCard (multiline paste, terminate with '---' or 'ABORT')", cmdVcardSet},
    {"GET",    "", "Show own vCard",                                                   cmdVcardGet},
    {"DELETE", "", "Delete own vCard",                                                 cmdVcardDelete},
    {nullptr, nullptr, nullptr, nullptr},
};

static void cmdVcard(const char* args) {
    serial::dispatchSubCommand("VCARD", args, kVcardSubs);
}

/**
 * \brief Registers serial commands exposed by vCard module.
 */
static void registerSerialCommands() {
    auto& reg = serial::getCommandRegistry();
    reg.registerCommand({"VCARD",
                         "vCard storage: SET/GET/DELETE",
                         cmdVcard, "vcard", false, kVcardSubs});
}

// ============================================================================
// Module Implementation
// ============================================================================

/**
 * \brief Returns singleton vCard module instance.
 * \return Module singleton reference.
 */
VcardModule& VcardModule::instance() {
    static VcardModule inst;
    return inst;
}

/**
 * \brief Initializes module UI strings, serial commands, and BLE service hooks.
 * \return `true` if initialization succeeded.
 */
bool VcardModule::init() {
    LOG_I(TAG, "Initializing vCard module");
    registerStrings();
    registerSerialCommands();

    VcardWizard::configure(mstr, s_wizardStepOffsets, STR_SAVED, STR_EXCHANGE_FAIL);

    if (!ble_vcard_init()) {
        LOG_W(TAG, "BLE vCard init failed (BLE might not be available)");
    }

    if (!s_vcardBleMutex) {
        s_vcardBleMutex = xSemaphoreCreateMutex();
    }
    core::EventBus::instance().subscribe(onConsentRequestEvent,
                                         core::EventBus::eventMask(core::EventType::BLE_CONSENT_REQUEST));
    core::EventBus::instance().subscribe(onExchangeCompleteEvent,
                                         core::EventBus::eventMask(core::EventType::BLE_EXCHANGE_COMPLETE));

    ble_vcard_set_consent_callback(onConsentRequest);
    ble_vcard_set_exchange_complete_callback(onExchangeComplete);

    ble_vcard_set_receive_enabled(true);

    core::ModuleRegistry::instance().registerModule(this);
    state_ = core::ServiceState::INITIALIZED;
    return true;
}

/**
 * \brief Starts vCard module service.
 * \return `true` if start transition succeeded.
 */
bool VcardModule::start() {
    if (state_ != core::ServiceState::INITIALIZED && state_ != core::ServiceState::STOPPED) {
        return false;
    }
    state_ = core::ServiceState::STARTED;
    return true;
}

/**
 * \brief Stops vCard BLE service and module runtime.
 */
void VcardModule::stop() {
    ble_vcard_deinit();
    state_ = core::ServiceState::STOPPED;
}

/**
 * \brief Provides tools-menu entry for vCard module.
 * \param items Output array for menu items.
 * \param maxItems Maximum writable entries.
 * \return Number of populated menu items.
 */
uint8_t VcardModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    items[0] = {
        mstr(STR_VCARD),
        110,
        []() -> ui::IView* {
            if (!s_viewsInitialized) {
                s_mainMenu.setOnSelect(onMainMenuSelect);
                s_peerList.setOnSelect(onPeerSelect);
                s_viewsInitialized = true;
            }
            rebuildMainMenu();
            return &s_mainMenu;
        },
        nullptr,
        getName(),
        core::MenuLocation::TOOLS_MENU,
        nullptr
    };

    return 1;
}

/**
 * \brief Provides the lock-screen quick action that shows the owner vCard as a QR code.
 * \param items Output array for context items.
 * \param maxItems Maximum writable entries.
 * \return Number of populated entries.
 */
uint8_t VcardModule::getLockScreenContextItems(core::LockScreenContextItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;
    items[0] = {
        getMyVcardLockscreenLabel,
        onMyVcardLockscreenSelect,
        60,
        nullptr,
    };
    return 1;
}

/**
 * \brief Periodic vCard module tick forwarding BLE state machine.
 * \param nowMs Current uptime in milliseconds.
 */
void VcardModule::onTick(uint32_t nowMs) {
    ble_vcard_tick(nowMs);
}

/// Schema version written to and expected from the vCard backup section.
static constexpr int kSchemaVer = 1;

/**
 * \brief Exports the own vCard and all received vCards into the backup section.
 *
 * Writes `schema_ver`, an optional `own` string, and a `received` array of raw
 * vCard texts. Returns `false` only when there is nothing to export.
 *
 * \param out cJSON object that forms the module's section in the backup file.
 * \return `true` if any vCard data was exported.
 */
bool VcardModule::exportBackup(cJSON* out) {
    if (!out) return false;

    cJSON_AddNumberToObject(out, "schema_ver", kSchemaVer);

    bool any = false;

    char buf[VCARD_MAX_LEN + 1];
    if (vcard_store_get_own(buf, sizeof(buf)) > 0) {
        cJSON_AddStringToObject(out, "own", buf);
        any = true;
    }

    cJSON* received = cJSON_AddArrayToObject(out, "received");
    if (!received) return any;

    uint16_t slots[VCARD_MAX_CARDS];
    uint16_t count = vcard_store_get_sorted(slots, VCARD_MAX_CARDS);
    for (uint16_t i = 0; i < count; i++) {
        if (vcard_store_get(slots[i], buf, sizeof(buf)) == 0) continue;
        cJSON* item = cJSON_CreateString(buf);
        if (!item) continue;
        cJSON_AddItemToArray(received, item);
        any = true;
    }

    return any;
}

/**
 * \brief Imports one received vCard string into storage.
 *
 * The vCard's identity is its full text; the store deduplicates on exact text,
 * so an already-present card counts as imported (no-op upsert). Genuine
 * validation/storage failures return `false` to be tallied as failed.
 *
 * \param je JSON array element (expected to be a string).
 * \param user Unused.
 * \return `true` if the card is present after the operation.
 */
static bool importReceivedVcard(const cJSON* je, void* user) {
    (void)user;
    if (!cJSON_IsString(je) || !je->valuestring || je->valuestring[0] == '\0') return false;

    const char* text = je->valuestring;
    size_t len = strlen(text);
    char err[32] = {};
    if (vcard_store_add(text, len, err, sizeof(err))) return true;

    // An already-present card means the identity is satisfied (no-op upsert);
    // only genuine validation/storage failures count as failed.
    return vcard_store_contains(text, len);
}

/**
 * \brief Restores the own vCard and received vCards from the backup section.
 *
 * The own vCard overwrites the current one; received vCards are upserted by
 * exact text. Best-effort: failures are tallied, never aborting the restore.
 *
 * \param in cJSON object holding the previously exported section.
 * \return Tally of imported and failed records.
 */
core::IModule::BackupResult VcardModule::importBackup(const cJSON* in) {
    if (!in) return {};

    const cJSON* schemaVer = cJSON_GetObjectItemCaseSensitive(in, "schema_ver");
    if (cJSON_IsNumber(schemaVer) && static_cast<int>(schemaVer->valuedouble) != kSchemaVer) {
        LOG_W(TAG, "vCard backup schema_ver %d != expected %d, skipping",
              static_cast<int>(schemaVer->valuedouble), kSchemaVer);
        return {};
    }

    core::IModule::BackupResult result = {};

    const cJSON* own = cJSON_GetObjectItemCaseSensitive(in, "own");
    if (cJSON_IsString(own) && own->valuestring && own->valuestring[0] != '\0') {
        char err[32] = {};
        if (vcard_store_set_own(own->valuestring, strlen(own->valuestring), err, sizeof(err))) {
            result.imported++;
        } else {
            LOG_W(TAG, "vCard import: own vCard rejected (%s)", err);
            result.failed++;
        }
    }

    const cJSON* received = cJSON_GetObjectItemCaseSensitive(in, "received");
    core::IModule::BackupResult rx = cdc::ui::importJsonArray(received, importReceivedVcard, nullptr);
    result.imported = static_cast<uint16_t>(result.imported + rx.imported);
    result.failed = static_cast<uint16_t>(result.failed + rx.failed);

    return result;
}

} // namespace cdc::mod_vcard

/**
 * \brief Registers vCard module initializer in global module registry.
 */
extern "C" void mod_vcard_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_vcard::VcardModule::instance();
        module.init();
    });
}
