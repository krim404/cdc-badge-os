#include "mod_vcard/VcardModule.h"
#include "mod_vcard/VcardWizard.h"
#include "mod_vcard/vcard_store.h"
#include "cdc_msg/MessageTransfer.h"
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
#include "cdc_views/ContextMenuView.h"
#include "cdc_views/QRCodeView.h"
#include "esp_timer.h"
#include "cdc_views/ToastView.h"
#include "cdc_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char* TAG = "VCARD";

namespace cdc::mod_vcard {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_vcard.title",            "vCards"},
    {"mod_vcard.my_vcard",         "My vCard"},
    {"mod_vcard.nearby",           "Nearby"},
    {"mod_vcard.scan",             "Start Scan"},
    {"mod_vcard.stop_scan",        "Stop Scan"},
    {"mod_vcard.advertising",      "Start Advertising"},
    {"mod_vcard.stop_adv",         "Stop Advertising"},
    {"mod_vcard.exchange",         "Exchange"},
    {"mod_vcard.no_peers",         "No peers found"},
    {"mod_vcard.scanning",         "Scanning..."},
    {"mod_vcard.exchange_req",     "Exchange Request"},
    {"mod_vcard.accept",           "Accept"},
    {"mod_vcard.decline",          "Decline"},
    {"mod_vcard.exchange_ok",      "Exchange successful"},
    {"mod_vcard.exchange_fail",    "Exchange failed"},
    {"mod_vcard.connecting",       "Connecting..."},
    {"mod_vcard.edit_my_vcard",    "Edit my vCard"},
    {"mod_vcard.no_vcard",         "No vCard set"},
    {"mod_vcard.given_name",       "First name"},
    {"mod_vcard.family_name",      "Last name"},
    {"mod_vcard.formatted_name",   "Display name"},
    {"mod_vcard.organization",     "Organization"},
    {"mod_vcard.position",         "Position"},
    {"mod_vcard.email",            "Email"},
    {"mod_vcard.tel_cell",         "Phone (Mobile)"},
    {"mod_vcard.tel_home",         "Phone (Home)"},
    {"mod_vcard.tel_work",         "Phone (Work)"},
    {"mod_vcard.url",              "Website"},
    {"mod_vcard.telegram",         "Telegram"},
    {"mod_vcard.matrix",           "Matrix"},
    {"mod_vcard.threema",          "Threema"},
    {"mod_vcard.social_profile",   "Social Profile"},
    {"mod_vcard.note",             "Note"},
    {"mod_vcard.send",             "Send vCard"},
    {"mod_vcard.received",         "Contact (vCard)"},
    {"mod_vcard.received_title",   "Received vCards"},
    {"mod_vcard.no_received",      "No received vCards"},
    {"mod_vcard.show_qr",          "Show QR"},
    {"mod_vcard.forward",          "Forward"},
    {"mod_vcard.confirm_delete",   "Delete this contact?"},
};

static const char* const s_wizardStepKeys[16] = {
    "mod_vcard.given_name", "mod_vcard.family_name", "mod_vcard.formatted_name",
    "mod_vcard.organization", "mod_vcard.position", "mod_vcard.email",
    "mod_vcard.tel_cell", "mod_vcard.tel_home", "mod_vcard.tel_work",
    "mod_vcard.url", "mod_vcard.telegram", "core.signal",
    "mod_vcard.matrix", "mod_vcard.threema", "mod_vcard.social_profile",
    "mod_vcard.note",
};

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

/**
 * \brief View instances used by vCard module UI flow.
 */
static ui::ListView s_mainMenu;
static bool s_viewsInitialized = false;

/**
 * \brief Main-menu item identifiers.
 */
enum MainMenuItem {
    MENU_MY_VCARD = 0,
    MENU_EDIT_MY_VCARD,
    MENU_SEND,
    MENU_RECEIVED,
    MENU_COUNT
};
static ui::ListItem s_mainMenuItems[MENU_COUNT] = {};

// Received-contacts list view and its backing buffers (PSRAM).
static ui::ListView s_receivedMenu;
static bool s_receivedInitialized = false;
static EXT_RAM_BSS_ATTR ui::ListItem s_recvItems[VCARD_MAX_CARDS];
static EXT_RAM_BSS_ATTR char s_recvLabels[VCARD_MAX_CARDS][64];
static uint16_t s_recvSlots[VCARD_MAX_CARDS];
static uint16_t s_recvCount = 0;
// Slot the context menu / confirm dialog currently acts on.
static uint16_t s_activeSlot = 0;

static void rebuildMainMenu();
static void onMainMenuSelect(uint16_t index, void* userData);
static void openReceivedList();
static void rebuildReceivedList();
static void showVcardDetails(const char* title, const char* raw, bool withActions);
static void showVcardQr(const char* raw, const char* fallbackTitle);
static void onReceivedViewMenu(void* userData);

/**
 * \brief Delivers a received vCard into the contact store.
 *
 * Invoked by the message-transfer framework after the user consented and the
 * encrypted transfer completed. \p data is UNTRUSTED, attacker-controlled and
 * not NUL-terminated; it is copied into a bounded buffer before parsing.
 * \return true if the vCard was accepted/stored.
 */
static bool deliverVcard(const uint8_t* data, uint32_t len, const char* /*mime*/,
                         const char* /*peerName*/) {
    if (!data || len == 0 || len > VCARD_MAX_LEN) return false;
    static EXT_RAM_BSS_ATTR char buf[VCARD_MAX_LEN + 1];
    memcpy(buf, data, len);
    buf[len] = '\0';
    char err[64] = {0};
    if (!vcard_store_add(buf, len, err, sizeof(err))) {
        LOG_W(TAG, "Failed to store received vCard: %s", err);
        return false;
    }
    return true;
}

/**
 * \brief Renders a vCard's parsed fields into a scrollable InfoView.
 *
 * Shows the display/full name as the first line, then one "<label>: <value>"
 * line per non-empty field, reusing the editor field labels. Falls back to the
 * raw text when nothing parses.
 * \param title View header title.
 * \param raw NUL-terminated vCard 4.0 text.
 * \param withActions When true, key 3 opens the received-contact action menu.
 */
static void showVcardDetails(const char* title, const char* raw, bool withActions) {
    static EXT_RAM_BSS_ATTR vcard_data_t s_parsed;
    static EXT_RAM_BSS_ATTR char s_text[ui::InfoView::MAX_TEXT_LEN];

    memset(&s_parsed, 0, sizeof(s_parsed));
    vcard_parse_to_struct(raw, &s_parsed);

    int n = 0;
    const int cap = static_cast<int>(sizeof(s_text));
    auto append = [&](const char* fmt, const char* a, const char* b) {
        if (n >= cap - 1) return;
        int w = snprintf(s_text + n, static_cast<size_t>(cap - n), fmt, a, b);
        if (w > 0) n += w;
        if (n > cap - 1) n = cap - 1;
    };

    if (s_parsed.formatted_name[0]) {
        append("%s%s\n\n", s_parsed.formatted_name, "");
    } else if (s_parsed.given_name[0] || s_parsed.family_name[0]) {
        append("%s %s\n\n", s_parsed.given_name, s_parsed.family_name);
    }

    auto field = [&](const char* labelKey, const char* value) {
        if (value[0]) append("%s: %s\n", ui::tr(labelKey), value);
    };
    field("mod_vcard.organization",   s_parsed.organization);
    field("mod_vcard.position",       s_parsed.title);
    field("mod_vcard.email",          s_parsed.email);
    field("mod_vcard.tel_cell",       s_parsed.tel_cell);
    field("mod_vcard.tel_home",       s_parsed.tel_home);
    field("mod_vcard.tel_work",       s_parsed.tel_work);
    field("mod_vcard.url",            s_parsed.url);
    field("mod_vcard.telegram",       s_parsed.impp_telegram);
    field("core.signal",              s_parsed.impp_signal);
    field("mod_vcard.matrix",         s_parsed.impp_matrix);
    field("mod_vcard.threema",        s_parsed.impp_threema);
    field("mod_vcard.social_profile", s_parsed.social_profile);
    field("mod_vcard.note",           s_parsed.note);

    if (n == 0) snprintf(s_text, sizeof(s_text), "%s", raw);

    static ui::InfoView s_detailView;
    s_detailView.init(title, s_text);
    s_detailView.setOnMenu(withActions ? onReceivedViewMenu : nullptr);
    ui::ViewStack::instance().push(&s_detailView);
}

/**
 * \brief Shows a vCard as a QR code, titled with the contact name.
 * \param raw NUL-terminated vCard text to encode.
 * \param fallbackTitle Title used when the card carries no name.
 */
static void showVcardQr(const char* raw, const char* fallbackTitle) {
    static EXT_RAM_BSS_ATTR vcard_data_t s_parsed;
    static char s_qrTitle[96];
    static char s_qrSubtitle[96];

    memset(&s_parsed, 0, sizeof(s_parsed));
    vcard_parse_to_struct(raw, &s_parsed);

    if (s_parsed.formatted_name[0]) {
        snprintf(s_qrTitle, sizeof(s_qrTitle), "%s", s_parsed.formatted_name);
    } else if (s_parsed.given_name[0] || s_parsed.family_name[0]) {
        snprintf(s_qrTitle, sizeof(s_qrTitle), "%s %s",
                 s_parsed.given_name, s_parsed.family_name);
    } else {
        snprintf(s_qrTitle, sizeof(s_qrTitle), "%s", fallbackTitle);
    }

    const char* sub = s_parsed.organization[0] ? s_parsed.organization
                    : s_parsed.title[0]        ? s_parsed.title
                    : s_parsed.email[0]        ? s_parsed.email
                    : "";
    snprintf(s_qrSubtitle, sizeof(s_qrSubtitle), "%s", sub);

    ui::showQRCode(raw, s_qrTitle, s_qrSubtitle[0] ? s_qrSubtitle : nullptr);
}

/**
 * \brief Rebuilds the vCard main menu.
 */
static void rebuildMainMenu() {
    s_mainMenuItems[MENU_MY_VCARD]      = {ui::tr("mod_vcard.my_vcard"),      0, false, nullptr};
    s_mainMenuItems[MENU_EDIT_MY_VCARD] = {ui::tr("mod_vcard.edit_my_vcard"), 0, false, nullptr};
    s_mainMenuItems[MENU_SEND]          = {ui::tr("mod_vcard.send"),          0, false, nullptr};
    s_mainMenuItems[MENU_RECEIVED]      = {ui::tr("mod_vcard.received_title"), 0, false, nullptr};
    s_mainMenu.init(ui::tr("mod_vcard.title"), s_mainMenuItems, MENU_COUNT);
}

/**
 * \brief Handles main-menu actions for local vCard operations and sharing.
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
                showVcardDetails(ui::tr("mod_vcard.my_vcard"), vcardText, false);
            } else {
                ui::showToastInfo(ui::tr("mod_vcard.no_vcard"));
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

        case MENU_SEND: {
            // Push our own card to a nearby badge; the framework owns the peer
            // picker, consent, encryption and progress UI.
            static EXT_RAM_BSS_ATTR char own[VCARD_MAX_LEN + 1];
            size_t len = vcard_store_get_own(own, sizeof(own));
            if (len == 0) {
                ui::showToastInfo(ui::tr("mod_vcard.no_vcard"));
                break;
            }
            cdc::msg::MessageTransfer::instance().beginInteractiveSend(
                "text/vcard", reinterpret_cast<const uint8_t*>(own),
                static_cast<uint32_t>(len));
            break;
        }

        case MENU_RECEIVED:
            openReceivedList();
            break;
    }
}

// ============================================================================
// Received contacts: list, detail, context menu (view / QR / forward / delete).
// ============================================================================

/**
 * \brief Rebuilds the received-contacts list from the store (sorted by name).
 */
static void rebuildReceivedList() {
    s_recvCount = vcard_store_get_sorted(s_recvSlots, VCARD_MAX_CARDS);
    for (uint16_t i = 0; i < s_recvCount; i++) {
        uint16_t slot = s_recvSlots[i];
        if (!vcard_store_get_display(slot, s_recvLabels[i], sizeof(s_recvLabels[i]))) {
            s_recvLabels[i][0] = '\0';
        }
        s_recvItems[i] = {s_recvLabels[i], 0, false,
                          reinterpret_cast<void*>(static_cast<uintptr_t>(slot))};
    }
    s_receivedMenu.setEmptyText(ui::tr("mod_vcard.no_received"));
    s_receivedMenu.init(ui::tr("mod_vcard.received_title"), s_recvItems, s_recvCount);
}

/**
 * \brief Opens the detail view (with action menu) for the selected contact.
 * \param index Unused on-screen position.
 * \param userData Encoded store slot of the selected contact.
 */
static void onReceivedSelect(uint16_t index, void* userData) {
    (void)index;
    s_activeSlot = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(userData));
    static EXT_RAM_BSS_ATTR char raw[VCARD_MAX_LEN + 1];
    if (vcard_store_get(s_activeSlot, raw, sizeof(raw)) == 0) return;
    showVcardDetails(ui::tr("mod_vcard.received_title"), raw, true);
}

/// Context-menu action: create a new stored contact via the wizard.
static void ctxReceivedAdd() {
    VcardWizard::startReceived(&s_receivedMenu, rebuildReceivedList);
}

/// Context-menu action: edit the active stored contact via the wizard.
static void ctxReceivedEdit() {
    VcardWizard::editReceived(&s_receivedMenu, s_activeSlot, rebuildReceivedList);
}

/// Context-menu action: show the active contact as a QR code.
static void ctxReceivedQr() {
    static EXT_RAM_BSS_ATTR char raw[VCARD_MAX_LEN + 1];
    if (vcard_store_get(s_activeSlot, raw, sizeof(raw)) == 0) return;
    showVcardQr(raw, ui::tr("mod_vcard.received_title"));
}

/// Context-menu action: forward the active contact to a nearby badge.
static void ctxReceivedForward() {
    static EXT_RAM_BSS_ATTR char raw[VCARD_MAX_LEN + 1];
    size_t len = vcard_store_get(s_activeSlot, raw, sizeof(raw));
    if (len == 0) return;
    cdc::msg::MessageTransfer::instance().beginInteractiveSend(
        "text/vcard", reinterpret_cast<const uint8_t*>(raw),
        static_cast<uint32_t>(len));
}

/**
 * \brief Confirm-dialog handler: deletes the contact and refreshes the list.
 * \param userData Pointer to the store slot to delete.
 */
static void onReceivedDeleteConfirm(void* userData) {
    uint16_t slot = *static_cast<uint16_t*>(userData);
    if (vcard_store_delete(slot)) {
        ui::showToastSuccess(ui::tr("core.deleted"));
        s_receivedMenu.preservePosition();
        rebuildReceivedList();
        ui::ViewStack::instance().popToAnchor(&s_receivedMenu);
    } else {
        ui::showToastError(ui::tr("core.failed"));
    }
}

/// Context-menu action: confirm and delete the active contact.
static void ctxReceivedDelete() {
    ui::showConfirm(ui::tr("mod_vcard.confirm_delete"), onReceivedDeleteConfirm, nullptr,
                    ui::ConfirmView::Icon::WARNING, &s_activeSlot);
}

/**
 * \brief Detail-view context menu (key 3): edit / forward / QR / delete the
 *        currently shown contact.
 */
static void onReceivedViewMenu(void* userData) {
    (void)userData;
    const ui::ContextMenuItem items[] = {
        {ui::tr("core.edit"),   ctxReceivedEdit},
        {ui::tr("mod_vcard.forward"),     ctxReceivedForward},
        {ui::tr("mod_vcard.show_qr"),     ctxReceivedQr},
        {ui::tr("core.delete"), ctxReceivedDelete},
    };
    ui::showContextMenu(ui::tr("core.actions"), items, 4);
}

/**
 * \brief List context menu (key 3): add a contact; for a selected entry also
 *        edit / forward / delete it.
 * \param index Unused on-screen position.
 * \param userData Encoded store slot of the selected contact.
 */
static void onReceivedMenu(uint16_t index, void* userData) {
    (void)index;
    ui::ContextMenuItem items[4] = {};
    uint8_t n = 0;
    items[n++] = {ui::tr("core.add"), ctxReceivedAdd};
    if (s_recvCount > 0) {
        s_activeSlot = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(userData));
        items[n++] = {ui::tr("core.edit"),   ctxReceivedEdit};
        items[n++] = {ui::tr("mod_vcard.forward"),     ctxReceivedForward};
        items[n++] = {ui::tr("core.delete"), ctxReceivedDelete};
    }
    ui::showContextMenu(ui::tr("core.actions"), items, n);
}

/**
 * \brief Lazily wires callbacks, rebuilds and pushes the received-contacts list.
 */
static void openReceivedList() {
    if (!s_receivedInitialized) {
        s_receivedMenu.setOnSelect(onReceivedSelect);
        s_receivedMenu.setOnMenu(onReceivedMenu);
        s_receivedInitialized = true;
    }
    rebuildReceivedList();
    ui::ViewStack::instance().push(&s_receivedMenu);
}

// ============================================================================
// Lock-screen quick action: show own vCard as a QR code.
// ============================================================================

/**
 * \brief Returns the localized label for the lock-screen quick action.
 */
static const char* getMyVcardLockscreenLabel() {
    return ui::tr("mod_vcard.my_vcard");
}

/**
 * \brief Lock-screen quick action: shows the own vCard as a QR code.
 *        Falls back to a toast when no vCard has been configured yet.
 */
static void onMyVcardLockscreenSelect() {
    static EXT_RAM_BSS_ATTR char s_qrBuf[VCARD_MAX_LEN + 1];
    size_t len = vcard_store_get_own(s_qrBuf, sizeof(s_qrBuf));
    if (len == 0) {
        ui::showToastError(ui::tr("mod_vcard.no_vcard"));
        return;
    }
    showVcardQr(s_qrBuf, ui::tr("mod_vcard.my_vcard"));
}

// ============================================================================
// Serial Commands (VCARD_SET / VCARD_GET / VCARD_DELETE)
// ============================================================================

EXT_RAM_BSS_ATTR static char s_vcardBuf[VCARD_MAX_LEN + 64];
static int  s_vcardBufPos = 0;
static bool s_vcardInputMode = false;
// Paste target: -1 sets the own card, otherwise the received slot to overwrite.
static int  s_vcardSetSlot = -1;
static esp_timer_handle_t s_vcardIdleTimer = nullptr;
// Cancel a stalled paste session after this many seconds of inactivity so a
// crashed/interrupted client cannot lock the serial console forever.
static constexpr int64_t VCARD_IDLE_LIMIT_US = 30 * 1000000LL;

static void vcard_session_clear() {
    s_vcardInputMode = false;
    s_vcardBufPos = 0;
    s_vcardSetSlot = -1;
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
        bool ok = (s_vcardSetSlot >= 0)
            ? vcard_store_update(static_cast<uint16_t>(s_vcardSetSlot), s_vcardBuf,
                                 static_cast<size_t>(s_vcardBufPos), err, sizeof(err))
            : vcard_store_set_own(s_vcardBuf, static_cast<size_t>(s_vcardBufPos), err, sizeof(err));
        if (ok) {
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
 *
 * Without an argument it sets the own card. With a numeric \p args it overwrites
 * the received card stored under that slot id (as listed by `VCARD LIST`).
 * \param args Optional received-card slot id to overwrite.
 */
static void cmdVcardSet(const char* args) {
    using Console = serial::Console;

    s_vcardSetSlot = -1;
    if (args && *args) {
        int slot = atoi(args);
        char probe[2];
        if (slot < 0 || slot >= VCARD_MAX_CARDS ||
            !vcard_store_get_display(static_cast<uint16_t>(slot), probe, sizeof(probe))) {
            Console::printf("ERROR: no vCard at id %d\r\n", slot);
            return;
        }
        s_vcardSetSlot = slot;
    }

    Console::printf("Paste vCard 4.0, end with '---' on a new line "
                    "(or 'ABORT' to cancel):\r\n");
    s_vcardBufPos = 0;
    s_vcardInputMode = true;
    serial::getCommandRegistry().setLineInterceptor(vcardLineInterceptor);
    vcard_arm_idle_timer();
}

/**
 * \brief Prints a NUL-terminated vCard buffer line by line as CRLF output.
 * \param out Buffer holding the vCard text; consumed in place.
 */
static void vcardPrintLines(char* out) {
    using Console = serial::Console;
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
 * \brief Serial command printing a stored vCard.
 *
 * Without an argument it prints the own card, or an empty 4.0 template if none
 * is set. With a numeric \p args it prints the received card stored under that
 * slot id (as listed by `VCARD LIST`).
 * \param args Optional received-card slot id.
 */
static void cmdVcardGet(const char* args) {
    using Console = serial::Console;
    char out[VCARD_MAX_LEN + 1];

    if (args && *args) {
        int slot = atoi(args);
        if (slot < 0 || slot >= VCARD_MAX_CARDS ||
            vcard_store_get(static_cast<uint16_t>(slot), out, sizeof(out)) == 0) {
            Console::printf("ERROR: no vCard at id %d\r\n", slot);
            return;
        }
        vcardPrintLines(out);
        return;
    }

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

    vcardPrintLines(out);
}

/**
 * \brief Serial command listing received vCards as "<id>  <display name>".
 * \param args Unused command arguments.
 */
static void cmdVcardList(const char* args) {
    (void)args;
    using Console = serial::Console;

    uint16_t slots[VCARD_MAX_CARDS];
    uint16_t count = vcard_store_get_sorted(slots, VCARD_MAX_CARDS);
    if (count == 0) {
        Console::printf("No received vCards\r\n");
        return;
    }

    char name[64];
    for (uint16_t i = 0; i < count; i++) {
        if (!vcard_store_get_display(slots[i], name, sizeof(name))) name[0] = '\0';
        Console::printf("%2u  %s\r\n", slots[i], name);
    }
}

/**
 * \brief Serial command deleting a stored vCard.
 *
 * Without an argument it deletes the own card. With a numeric \p args it deletes
 * the received card stored under that slot id (as listed by `VCARD LIST`).
 * \param args Optional received-card slot id.
 */
static void cmdVcardDelete(const char* args) {
    using Console = serial::Console;

    if (args && *args) {
        int slot = atoi(args);
        if (slot < 0 || slot >= VCARD_MAX_CARDS ||
            !vcard_store_delete(static_cast<uint16_t>(slot))) {
            Console::printf("ERROR: no vCard at id %d\r\n", slot);
            return;
        }
        Console::printf("OK: vCard %d deleted\r\n", slot);
        return;
    }

    if (vcard_store_clear_own()) {
        Console::printf("OK: vCard deleted\r\n");
    } else {
        Console::printf("ERROR: Failed to delete vCard\r\n");
    }
}

static const serial::SubCommand kVcardSubs[] = {
    {"SET",    "[id]", "Set own vCard, or overwrite received vCard <id> (multiline paste, end with '---' or 'ABORT')", cmdVcardSet},
    {"GET",    "[id]", "Show own vCard, or received vCard <id>",                       cmdVcardGet},
    {"LIST",   "", "List received vCards",                                             cmdVcardList},
    {"DELETE", "[id]", "Delete own vCard, or received vCard <id>",                     cmdVcardDelete},
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
                         "vCard storage: SET/GET/LIST/DELETE",
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

    VcardWizard::configure(s_wizardStepKeys, "core.saved", "mod_vcard.exchange_fail");

    // Register as the handler for incoming "text/vcard" message transfers.
    cdc::msg::MessageTransfer::instance().registerHandler(
        "text/vcard", "mod_vcard.received", deliverVcard);

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
    cdc::msg::MessageTransfer::instance().unregisterHandler("text/vcard");
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
        ui::tr("mod_vcard.title"),
        110,
        []() -> ui::IView* {
            if (!s_viewsInitialized) {
                s_mainMenu.setOnSelect(onMainMenuSelect);
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
    (void)nowMs;
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
