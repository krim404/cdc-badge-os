#include "mod_gpg/GpgModule.h"
#include "mod_gpg/GpgStorage.h"
#include "mod_gpg/GpgRecvStore.h"
#include "mod_gpg/ble_gpg_xsig.h"
#include "openpgp/xsig.h"
#include "cdc_core/KeyFingerprint.h"
#include "cdc_core/Raii.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/UsbManager.h"
#include "cdc_ui/I18n.h"
#include "mod_gpg/openpgp/ccid.h"
#include "mod_gpg/openpgp/openpgp.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ListView.h"
#include "cdc_views/T9InputView.h"
#include "cdc_views/InfoView.h"
#include "cdc_views/QRCodeView.h"
#include "cdc_views/ConfirmView.h"
#include "cdc_views/ToastView.h"
#include "cdc_os_ui/views/PinChangeView.h"
#include "cdc_core/PinManager.h"
#include "cdc_core/pin_storage_c.h"
#include "serial_cmd/ICommandRegistry.h"
#include "serial_cmd/SubCommand.h"
#include "serial_cmd/Console.h"
#include "mod_gpg/gpg.h"
#include "cdc_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_attr.h"
#include <new>
#include <cstring>
#include <cstdio>
#include <cctype>

static const char* TAG = "GPG";

namespace cdc::mod_gpg {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_gpg.title",            "GPG"},
    {"mod_gpg.status",           "Status"},
    {"mod_gpg.generate",         "Generate Keys"},
    {"mod_gpg.export",           "Export Public"},
    {"mod_gpg.reset",            "Reset"},
    {"mod_gpg.settings",         "Settings"},
    {"mod_gpg.user_pin",         "User PIN"},
    {"mod_gpg.admin_pin",        "Admin PIN"},
    {"mod_gpg.slot_error",       "Slot map error"},
    {"mod_gpg.name",             "Name"},
    {"mod_gpg.email",            "Email (optional)"},
    {"mod_gpg.curve",            "Curve"},
    {"mod_gpg.curve_ed25519",    "Ed25519"},
    {"mod_gpg.curve_p256",       "P-256"},
    {"mod_gpg.no_key",           "No key configured"},
    {"mod_gpg.confirm_reset",    "Reset all GPG keys?"},
    {"mod_gpg.export_title",     "GPG Public Key"},
    {"mod_gpg.send",             "Send Key"},
    {"mod_gpg.received",         "Received Keys"},
    {"mod_gpg.recv_none",        "No received keys"},
    {"mod_gpg.recv_title",       "Received Keys"},
    {"mod_gpg.recv_details",     "Key Details"},
    {"mod_gpg.recv_sign",        "Cross-Sign"},
    {"mod_gpg.recv_export",      "Export"},
    {"mod_gpg.recv_delete",      "Delete"},
    {"mod_gpg.recv_confirm_sign","Sign this key?"},
    {"mod_gpg.recv_confirm_del", "Delete this key?"},
    {"mod_gpg.recv_signed",      "Signed"},
    {"mod_gpg.recv_unsigned",    "Unsigned"},
    {"mod_gpg.recv_export_title","Signed Key Export"},
    {"mod_gpg.toast_signed",     "Cross-signed"},
    {"mod_gpg.toast_sign_fail",  "Sign failed"},
    {"mod_gpg.toast_deleted",    "Deleted"},
};

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

static constexpr const char* CMD_MODULE = "gpg";
static bool s_commandsRegistered = false;

static void cmd_gpg_status(const char* args);
static void cmd_gpg_generate(const char* args);
static void cmd_gpg_export(const char* args);
static void cmd_gpg_reset(const char* args);
static void cmd_gpg_recv_list(const char* args);
static void cmd_gpg_recv_info(const char* args);
static void cmd_gpg_recv_delete(const char* args);
static void cmd_gpg_cross_sign(const char* args);
static void cmd_gpg_export_signed(const char* args);

static const cdc::serial::SubCommand kGpgSubs[] = {
    {"STATUS",       "",                      "Show keys, fingerprints, counters",                            cmd_gpg_status},
    {"GENERATE",     "<curve> <user_id>",     "Generate SIG+DEC+AUT keys (curve 1=Ed25519, 2=P-256)",         cmd_gpg_generate},
    {"EXPORT",       "",                      "Print primary + subkey public keys as PEM",                    cmd_gpg_export},
    {"RESET",        "[token]",               "Two-step destructive reset of all GPG keys",                   cmd_gpg_reset},
    {"RECV_LIST",    "",                      "List received cross-sign keys",                                cmd_gpg_recv_list},
    {"RECV_INFO",    "<index>",               "Show received key details",                                    cmd_gpg_recv_info},
    {"RECV_DELETE",  "<index>",               "Delete received key",                                          cmd_gpg_recv_delete},
    {"CROSS_SIGN",   "<index>",               "Cross-sign a received key",                                    cmd_gpg_cross_sign},
    {"EXPORT_SIGNED","<index>",               "Export signed key as ASCII-armored OpenPGP block",             cmd_gpg_export_signed},
    {nullptr, nullptr, nullptr, nullptr},
};

static void cmd_gpg(const char* args) {
    cdc::serial::dispatchSubCommand("GPG", args, kGpgSubs);
}

/**
 * \brief Registers serial commands exposed by GPG module.
 */
static void registerCommands() {
    if (s_commandsRegistered) return;
    s_commandsRegistered = true;
    auto& registry = cdc::serial::getCommandRegistry();
    registry.registerCommand({"GPG",
                              "GPG card: STATUS/GENERATE/EXPORT/RESET",
                              cmd_gpg, CMD_MODULE, true, kGpgSubs});
}

/**
 * \brief Serial command printing current GPG key status.
 * \param args Unused command arguments.
 */
static void cmd_gpg_status(const char* args) {
    (void)args;
    gpg_status_t status = {};
    if (!gpg_get_status(&status)) {
        cdc::serial::Console::printf("ERROR: No key configured\r\n");
        return;
    }
    cdc::serial::Console::printf("User-ID: %s\r\n", status.user_id);
    cdc::serial::Console::printf("Curve: %s\r\n",
                                 status.curve == CDC_CURVE_ED25519 ? "Ed25519" : "P-256");
    cdc::serial::Console::printf("Created: %lu\r\n", static_cast<unsigned long>(status.created_at));
    cdc::serial::Console::printf("Sign Count: %lu\r\n", static_cast<unsigned long>(status.sign_count));
}

/**
 * \brief Serial command generating GPG key with selected curve and user-id.
 * \param args Command arguments (`<curve> <user_id>`).
 */
static void cmd_gpg_generate(const char* args) {
    char curveBuf[8] = {};
    char userId[GPG_USER_ID_MAX] = {};

    const char* p = args;
    while (p && *p && std::isspace(static_cast<unsigned char>(*p))) p++;
    if (!p || !*p) {
        cdc::serial::Console::printf("Usage: GPG GENERATE <curve> <user_id>\r\n");
        return;
    }

    size_t i = 0;
    while (p[i] && !std::isspace(static_cast<unsigned char>(p[i])) && i + 1 < sizeof(curveBuf)) {
        curveBuf[i] = p[i];
        i++;
    }
    curveBuf[i] = '\0';
    p += i;
    while (p && *p && std::isspace(static_cast<unsigned char>(*p))) p++;
    if (!p || !*p) {
        cdc::serial::Console::printf("Usage: GPG GENERATE <curve> <user_id>\r\n");
        return;
    }
    strncpy(userId, p, sizeof(userId) - 1);

    uint8_t curve = (atoi(curveBuf) == 2) ? CDC_CURVE_P256 : CDC_CURVE_ED25519;
    gpg_set_pending_user_id(userId);
    bool ok = gpg_generate_key(curve);
    cdc::serial::Console::printf(ok ? "OK\r\n" : "ERROR\r\n");
}

/**
 * \brief Serial command exporting GPG public key in PEM format.
 * \param args Unused command arguments.
 */
static void cmd_gpg_export(const char* args) {
    (void)args;
    char pem_buf[2048];
    size_t out_len = 0;
    if (!gpg_export_pubkey_pem(pem_buf, sizeof(pem_buf), &out_len)) {
        cdc::serial::Console::printf("ERROR\r\n");
        return;
    }
    cdc::serial::Console::printf("%s\r\n", pem_buf);
}

/**
 * \brief Serial command resetting GPG key material.
 * \param args Unused command arguments.
 */
static char s_reset_token[7] = {};
static uint64_t s_reset_token_ts_us = 0;
static constexpr uint64_t RESET_TOKEN_TIMEOUT_US = 30ULL * 1000ULL * 1000ULL;

static void cmd_gpg_reset(const char* args) {
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    const bool token_active = (s_reset_token[0] != '\0') &&
                              ((now - s_reset_token_ts_us) < RESET_TOKEN_TIMEOUT_US);

    if (args && *args && token_active && strcmp(args, s_reset_token) == 0) {
        memset(s_reset_token, 0, sizeof(s_reset_token));
        s_reset_token_ts_us = 0;
        bool ok = gpg_reset();
        cdc::serial::Console::printf(ok ? "OK\r\n" : "ERROR\r\n");
        return;
    }

    uint8_t r[3];
    esp_fill_random(r, sizeof(r));
    snprintf(s_reset_token, sizeof(s_reset_token), "%02X%02X%02X", r[0], r[1], r[2]);
    s_reset_token_ts_us = now;
    cdc::serial::Console::printf(
        "WARNING: this wipes ALL GPG keys (SIG/DEC/AUT), the DEC backup, and PINs.\r\n"
        "Confirm within 30s: GPG RESET %s\r\n",
        s_reset_token);
}

static void fp_to_hex(const uint8_t* fp, size_t len, char* out, size_t out_size) {
    if (out_size < len * 2 + 1) {
        if (out_size > 0) out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        snprintf(out + i * 2, 3, "%02X", fp[i]);
    }
    out[len * 2] = '\0';
}

static void cmd_gpg_recv_list(const char* args) {
    (void)args;
    auto& store = GpgRecvStore::instance();
    auto buf = ::cdc::core::psramAlloc<gpg_recv_index_entry_t>(GpgRecvStore::kMaxKeys);
    if (!buf) {
        cdc::serial::Console::printf("ERROR: out of memory\r\n");
        return;
    }
    uint8_t n = store.listIndex(buf.get(), GpgRecvStore::kMaxKeys);
    cdc::serial::Console::printf("OK: %u received keys\r\n", static_cast<unsigned>(n));
    for (uint8_t i = 0; i < n; ++i) {
        gpg_recv_key_t key = {};
        if (!store.getKey(i, &key)) continue;
        char fp_short[9];
        fp_to_hex(key.fingerprint_v4, 4, fp_short, sizeof(fp_short));
        cdc::serial::Console::printf("[%u] %s\r\n", i, key.user_id);
        cdc::serial::Console::printf("    FP: %s...\r\n", fp_short);
        cdc::serial::Console::printf("    Signed: %s\r\n",
                                     key.sig_len > 0 ? "Yes" : "No");
    }
}

static bool parse_index(const char* args, uint8_t* out) {
    if (!args || !*args) return false;
    char* end = nullptr;
    long v = strtol(args, &end, 10);
    if (end == args || v < 0 || v > 255) return false;
    *out = static_cast<uint8_t>(v);
    return true;
}

static void cmd_gpg_recv_info(const char* args) {
    uint8_t idx = 0;
    if (!parse_index(args, &idx)) {
        cdc::serial::Console::printf("Usage: GPG RECV_INFO <index>\r\n");
        return;
    }
    gpg_recv_key_t key = {};
    if (!GpgRecvStore::instance().getKey(idx, &key)) {
        cdc::serial::Console::printf("ERROR: index not found\r\n");
        return;
    }
    char fp_v4[41];
    char fp_v5[65];
    fp_to_hex(key.fingerprint_v4, 20, fp_v4, sizeof(fp_v4));
    fp_to_hex(key.fingerprint_v5, 32, fp_v5, sizeof(fp_v5));

    char ts_buf[32];
    time_t t = static_cast<time_t>(key.received_at);
    struct tm tm_v;
    gmtime_r(&t, &tm_v);
    strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S UTC", &tm_v);

    cdc::serial::Console::printf("OK: Key details\r\n");
    cdc::serial::Console::printf("User-ID:        %s\r\n", key.user_id);
    cdc::serial::Console::printf("Curve:          %s\r\n",
                                 key.curve == CDC_CURVE_ED25519 ? "Ed25519" : "P-256");
    cdc::serial::Console::printf("Fingerprint V4: %s\r\n", fp_v4);
    cdc::serial::Console::printf("Fingerprint V5: %s\r\n", fp_v5);
    cdc::serial::Console::printf("Received:       %s\r\n", ts_buf);
    cdc::serial::Console::printf("Signed:         %s\r\n", key.sig_len > 0 ? "Yes" : "No");
    cdc::serial::Console::printf("Verified:       %s\r\n",
                                 (key.flags & kGpgRecvFlagVerified) ? "Yes" : "No");
    if (key.sig_len > 0) {
        char sig_hex[2 * 64 + 1];
        fp_to_hex(key.my_signature, key.sig_len, sig_hex, sizeof(sig_hex));
        cdc::serial::Console::printf("Signature:      %s\r\n", sig_hex);
    }
}

static void cmd_gpg_recv_delete(const char* args) {
    uint8_t idx = 0;
    if (!parse_index(args, &idx)) {
        cdc::serial::Console::printf("Usage: GPG RECV_DELETE <index>\r\n");
        return;
    }
    cdc::serial::Console::printf(GpgRecvStore::instance().deleteKey(idx)
                                 ? "OK\r\n"
                                 : "ERROR: delete failed\r\n");
}

static void cmd_gpg_cross_sign(const char* args) {
    uint8_t idx = 0;
    if (!parse_index(args, &idx)) {
        cdc::serial::Console::printf("Usage: GPG CROSS_SIGN <index>\r\n");
        return;
    }
    gpg_recv_key_t key = {};
    if (!GpgRecvStore::instance().getKey(idx, &key)) {
        cdc::serial::Console::printf("ERROR: index not found\r\n");
        return;
    }
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    uint8_t sig[64] = {0};
    if (!gpgCrossSign(key, now, sig)) {
        cdc::serial::Console::printf("ERROR: signing failed\r\n");
        return;
    }
    if (!GpgRecvStore::instance().setSignature(idx, sig, 64,
                                               key.flags | kGpgRecvFlagVerified)) {
        cdc::serial::Console::printf("ERROR: store update failed\r\n");
        return;
    }
    cdc::serial::Console::printf("OK\r\n");
}

static void cmd_gpg_export_signed(const char* args) {
    uint8_t idx = 0;
    if (!parse_index(args, &idx)) {
        cdc::serial::Console::printf("Usage: GPG EXPORT_SIGNED <index>\r\n");
        return;
    }
    gpg_recv_key_t key = {};
    if (!GpgRecvStore::instance().getKey(idx, &key)) {
        cdc::serial::Console::printf("ERROR: index not found\r\n");
        return;
    }
    if (key.sig_len == 0) {
        cdc::serial::Console::printf("ERROR: key not yet signed (use GPG CROSS_SIGN first)\r\n");
        return;
    }

    auto buf = ::cdc::core::psramAlloc<char>(4096);
    if (!buf) {
        cdc::serial::Console::printf("ERROR: out of memory\r\n");
        return;
    }
    size_t out_len = 0;
    if (!gpgBuildSignedKeyArmored(key, buf.get(), 4096, &out_len)) {
        cdc::serial::Console::printf("ERROR: export failed\r\n");
        return;
    }
    cdc::serial::Console::printf("%s", buf.get());
}

static ui::ListView s_menuView;
static ui::ListView s_settingsView;
static ui::PinChangeView s_pinChangeView;
static ui::T9InputView s_t9Input;
static ui::ListView s_curveView;
static ui::InfoView s_infoView;
static ui::QRCodeView s_qrView;
static bool s_viewsInitialized = false;
static ui::ListItem s_menuItems[8] = {};
static ui::ListItem s_settingsItems[] = {
    { nullptr, 0, false, nullptr },
    { nullptr, 0, false, nullptr },
};

struct WizardState {
    char name[64];
    char email[64];
    uint8_t curve;
};

EXT_RAM_BSS_ATTR static WizardState s_wizard = {};

enum GpgMenuAction : uintptr_t {
    GPG_MENU_STATUS = 1,
    GPG_MENU_GENERATE,
    GPG_MENU_EXPORT,
    GPG_MENU_SEND,
    GPG_MENU_RECEIVED,
    GPG_MENU_SETTINGS,
    GPG_MENU_RESET,
};

static void showStatus();
static void wizardStart();
static void showExport();
static void confirmReset();
static void showSettings();
static void onSettingsSelect(uint16_t index, void*);
static void showSendKey();
static void showReceivedKeys();

/**
 * \brief Handles GPG main-menu selections via the entry's userData tag.
 */
static void onMenuSelect(uint16_t, void* userData) {
    switch (static_cast<GpgMenuAction>(reinterpret_cast<uintptr_t>(userData))) {
        case GPG_MENU_STATUS:    showStatus(); break;
        case GPG_MENU_GENERATE:  wizardStart(); break;
        case GPG_MENU_EXPORT:    showExport(); break;
        case GPG_MENU_SEND:      showSendKey(); break;
        case GPG_MENU_RECEIVED:  showReceivedKeys(); break;
        case GPG_MENU_SETTINGS:  showSettings(); break;
        case GPG_MENU_RESET:     confirmReset(); break;
        default: break;
    }
}

static inline ui::ListItem makeMenuItem(const char* label, GpgMenuAction action) {
    return { label, 0, false, reinterpret_cast<void*>(static_cast<uintptr_t>(action)) };
}

/**
 * \brief Rebuilds GPG main menu labels and populates userData tags.
 */
static void rebuildMenu() {
    const bool hasKeys = openpgp_has_any_key();
    uint8_t count = 0;
    s_menuItems[count++] = makeMenuItem(ui::tr("mod_gpg.status"),   GPG_MENU_STATUS);
    if (!hasKeys) {
        s_menuItems[count++] = makeMenuItem(ui::tr("mod_gpg.generate"), GPG_MENU_GENERATE);
    } else {
        s_menuItems[count++] = makeMenuItem(ui::tr("mod_gpg.export"),   GPG_MENU_EXPORT);
        s_menuItems[count++] = makeMenuItem(ui::tr("mod_gpg.send"),     GPG_MENU_SEND);
    }
    s_menuItems[count++] = makeMenuItem(ui::tr("mod_gpg.received"), GPG_MENU_RECEIVED);
    s_menuItems[count++] = makeMenuItem(ui::tr("mod_gpg.settings"), GPG_MENU_SETTINGS);
    s_menuItems[count++] = makeMenuItem(ui::tr("mod_gpg.reset"),    GPG_MENU_RESET);
    s_menuView.init(ui::tr("mod_gpg.title"), s_menuItems, count);
}

/**
 * \brief Verifies OpenPGP PW1 using persistent pin-storage backend.
 * \param pin Candidate PW1 value.
 * \return `true` when valid.
 */
static bool gpg_verify_pw1(const char* pin) {
    return pin_storage_openpgp_verify_pw1(pin);
}

/**
 * \brief Verifies OpenPGP PW3 using persistent pin-storage backend.
 * \param pin Candidate PW3 value.
 * \return `true` when valid.
 */
static bool gpg_verify_pw3(const char* pin) {
    return pin_storage_openpgp_verify_pw3(pin);
}

/**
 * \brief Changes OpenPGP PW1 value.
 * \param oldPin Ignored old PIN parameter from generic callback signature.
 * \param newPin New PW1 value.
 * \return `true` on success.
 */
static bool gpg_change_pw1(const char*, const char* newPin) {
    return pin_storage_openpgp_change_pw1(newPin);
}

/**
 * \brief Changes OpenPGP PW3 value.
 * \param oldPin Ignored old PIN parameter from generic callback signature.
 * \param newPin New PW3 value.
 * \return `true` on success.
 */
static bool gpg_change_pw3(const char*, const char* newPin) {
    return pin_storage_openpgp_change_pw3(newPin);
}

/**
 * \brief Returns remaining retries for OpenPGP PW1.
 * \return Retry counter.
 */
static uint8_t gpg_retries_pw1() {
    return pin_storage_openpgp_pw1_retries();
}

/**
 * \brief Returns remaining retries for OpenPGP PW3.
 * \return Retry counter.
 */
static uint8_t gpg_retries_pw3() {
    return pin_storage_openpgp_pw3_retries();
}

/**
 * \brief Returns whether OpenPGP PW1 is blocked.
 * \return `true` when blocked.
 */
static bool gpg_blocked_pw1() {
    return pin_storage_openpgp_pw1_blocked();
}

/**
 * \brief Returns whether OpenPGP PW3 is blocked.
 * \return `true` when blocked.
 */
static bool gpg_blocked_pw3() {
    return pin_storage_openpgp_pw3_blocked();
}

/**
 * \brief Pin-change completion callback returning to previous view.
 * \param changed Result flag (unused).
 */
static void onGpgPinComplete(bool) {
    ui::ViewStack::instance().pop();
}

/**
 * \brief Handles settings-menu selection for PW1/PW3 change flow.
 * \param index Selected settings row.
 * \param userData Optional callback context (unused).
 */
static void onSettingsSelect(uint16_t index, void*) {
    s_pinChangeView.setOnComplete(onGpgPinComplete);
    s_pinChangeView.setTitle(index == 0 ? ui::tr("mod_gpg.user_pin") : ui::tr("mod_gpg.admin_pin"));
    if (index == 0) {
        s_pinChangeView.setVerifyCallback(gpg_verify_pw1);
        s_pinChangeView.setChangeCallback(gpg_change_pw1);
        s_pinChangeView.setRetriesCallback(gpg_retries_pw1);
        s_pinChangeView.setBlockedCallback(gpg_blocked_pw1);
        s_pinChangeView.init(cdc::core::PinManager::PW1_MIN, cdc::core::PinManager::PIN_MAX);
    } else {
        s_pinChangeView.setVerifyCallback(gpg_verify_pw3);
        s_pinChangeView.setChangeCallback(gpg_change_pw3);
        s_pinChangeView.setRetriesCallback(gpg_retries_pw3);
        s_pinChangeView.setBlockedCallback(gpg_blocked_pw3);
        s_pinChangeView.init(cdc::core::PinManager::PW3_MIN, cdc::core::PinManager::PIN_MAX);
    }
    ui::ViewStack::instance().push(&s_pinChangeView);
}

/**
 * \brief Shows GPG settings menu.
 */
static void showSettings() {
    s_settingsItems[0].label = ui::tr("mod_gpg.user_pin");
    s_settingsItems[1].label = ui::tr("mod_gpg.admin_pin");
    s_settingsView.init(ui::tr("mod_gpg.settings"), s_settingsItems, 2);
    s_settingsView.setOnSelect(onSettingsSelect);
    ui::ViewStack::instance().push(&s_settingsView);
}

/**
 * \brief Displays current GPG key status and metadata.
 */
static void showStatus() {
    gpg_status_t status = {};
    if (!gpg_get_status(&status)) {
        s_infoView.init(ui::tr("mod_gpg.status"), ui::tr("mod_gpg.no_key"));
        ui::ViewStack::instance().push(&s_infoView);
        return;
    }

    char fp_hex[GPG_FINGERPRINT_LEN * 2 + 1] = {};
    for (size_t i = 0; i < GPG_FINGERPRINT_LEN; i++) {
        snprintf(fp_hex + i * 2, 3, "%02X", status.fingerprint[i]);
    }
    const char* curveName = status.curve == CDC_CURVE_ED25519 ? ui::tr("mod_gpg.curve_ed25519")
                                                             : ui::tr("mod_gpg.curve_p256");
    static char detail[512];
    snprintf(detail, sizeof(detail),
             "User-ID: %s\nCurve: %s\nFP: %s\nCreated: %lu\nSign Count: %lu",
             status.user_id, curveName, fp_hex,
             static_cast<unsigned long>(status.created_at),
             static_cast<unsigned long>(status.sign_count));
    s_infoView.init(ui::tr("mod_gpg.status"), detail);
    ui::ViewStack::instance().push(&s_infoView);
}

static void onWizardName(const char* text);
static void onWizardEmail(const char* text);
static void onWizardCurve(uint16_t index, void*);

/**
 * \brief Starts key-generation wizard flow.
 */
static void wizardStart() {
    memset(&s_wizard, 0, sizeof(s_wizard));
    s_t9Input.init(ui::tr("mod_gpg.name"), nullptr, 63);
    s_t9Input.setOnSave(onWizardName);
    ui::ViewStack::instance().push(&s_t9Input);
}

/**
 * \brief Saves wizard name and opens email step.
 * \param text Entered name.
 */
static void onWizardName(const char* text) {
    strncpy(s_wizard.name, text ? text : "", sizeof(s_wizard.name) - 1);
    s_t9Input.init(ui::tr("mod_gpg.email"), nullptr, 63);
    s_t9Input.setOnSave(onWizardEmail);
    ui::ViewStack::instance().push(&s_t9Input);
}

/**
 * \brief Saves wizard email and opens curve selection.
 * \param text Entered email.
 */
static void onWizardEmail(const char* text) {
    strncpy(s_wizard.email, text ? text : "", sizeof(s_wizard.email) - 1);
    static ui::ListItem curveItems[] = {
        { nullptr, 0, false, nullptr },
        { nullptr, 0, false, nullptr },
    };
    curveItems[0].label = ui::tr("mod_gpg.curve_ed25519");
    curveItems[1].label = ui::tr("mod_gpg.curve_p256");
    s_curveView.init(ui::tr("mod_gpg.curve"), curveItems, 2);
    s_curveView.setOnSelect(onWizardCurve);
    ui::ViewStack::instance().push(&s_curveView);
}

/**
 * \brief Finalizes wizard curve selection and triggers key generation.
 * \param index Selected curve index.
 * \param userData Optional callback context (unused).
 */
static void onWizardCurve(uint16_t index, void*) {
    s_wizard.curve = (index == 0) ? CDC_CURVE_ED25519 : CDC_CURVE_P256;
    char user_id[GPG_USER_ID_MAX] = {};
    size_t name_len = strnlen(s_wizard.name, sizeof(s_wizard.name) - 1);
    size_t email_len = strnlen(s_wizard.email, sizeof(s_wizard.email) - 1);
    if (email_len == 0) {
        snprintf(user_id, sizeof(user_id), "%.*s", static_cast<int>(sizeof(user_id) - 1), s_wizard.name);
    } else {
        size_t max_len = sizeof(user_id) - 1;
        size_t name_fit = name_len > max_len ? max_len : name_len;
        size_t email_fit = 0;
        if (name_fit < max_len) {
            size_t remaining = max_len - name_fit;
            if (remaining > 3) {
                email_fit = remaining - 3;
            }
        }
        if (email_fit == 0) {
            memcpy(user_id, s_wizard.name, name_fit);
            user_id[name_fit] = '\0';
        } else {
            size_t pos = 0;
            memcpy(user_id + pos, s_wizard.name, name_fit);
            pos += name_fit;
            user_id[pos++] = ' ';
            user_id[pos++] = '<';
            memcpy(user_id + pos, s_wizard.email, email_fit);
            pos += email_fit;
            user_id[pos++] = '>';
            user_id[pos] = '\0';
        }
    }
    gpg_set_pending_user_id(user_id);
    if (gpg_generate_key(s_wizard.curve)) {
        ui::showToastSuccess(ui::tr("core.ok"));
    } else {
        ui::showToastError(ui::tr("core.failed"));
    }
    while (ui::ViewStack::instance().depth() > 1) {
        ui::ViewStack::instance().pop();
    }
}

/**
 * \brief Exports public key to serial output and QR view.
 */
static void showExport() {
    EXT_RAM_BSS_ATTR static char pem_buf[2048];
    static char title_buf[GPG_USER_ID_MAX + 8];
    static char detail_buf[160];
    size_t out_len = 0;
    if (!gpg_export_pubkey_pem(pem_buf, sizeof(pem_buf), &out_len)) {
        ui::showToastError(ui::tr("core.failed"));
        return;
    }
    cdc::serial::Console::printf("%s\r\n", pem_buf);

    gpg_status_t status = {};
    title_buf[0]  = '\0';
    detail_buf[0] = '\0';
    if (gpg_get_status(&status) && status.initialized) {
        strlcpy(title_buf,
                status.user_id[0] ? status.user_id : "(card)",
                sizeof(title_buf));

        char alchemy[KEY_FINGERPRINT_MAX_LEN] = {};
        gpg_alchemy_fingerprint(alchemy, sizeof(alchemy));
        const char* curveName = (status.curve == CDC_CURVE_ED25519) ? "Ed25519" : "P-256";
        snprintf(detail_buf, sizeof(detail_buf), "%s\n%s", curveName, alchemy);
    }

    s_qrView.init(pem_buf,
                  title_buf[0]  ? title_buf  : nullptr,
                  detail_buf[0] ? detail_buf : nullptr);
    ui::ViewStack::instance().push(&s_qrView);
}

/**
 * \brief Confirm callback resetting all GPG key material.
 * \param userData Optional callback context (unused).
 */
static void onResetConfirm(void*) {
    if (gpg_reset()) {
        ui::showToastSuccess(ui::tr("core.ok"));
    } else {
        ui::showToastError(ui::tr("core.failed"));
    }
}

/**
 * \brief Opens reset confirmation dialog.
 */
static void confirmReset() {
    ui::showConfirm(ui::tr("mod_gpg.confirm_reset"), onResetConfirm, nullptr,
                    ui::ConfirmView::Icon::WARNING, nullptr);
}

/**
 * \brief Received-keys list UI state.
 */
static ui::ListView    s_recvListView;
static ui::ListView    s_recvActionView;
EXT_RAM_BSS_ATTR static ui::ListItem s_recvListItems[GpgRecvStore::kMaxKeys + 1];
static ui::ListItem    s_recvActionItems[3];
EXT_RAM_BSS_ATTR static char s_recvListLabels[GpgRecvStore::kMaxKeys][72];
static uint8_t         s_recvSelectedIndex = 0;
static gpg_recv_key_t  s_recvSelectedKey   = {};
EXT_RAM_BSS_ATTR static char s_recvDetailText[640];
EXT_RAM_BSS_ATTR static char s_recvExportBuf[4096];

static void rebuildReceivedList();
static void onReceivedListSelect(uint16_t index, void*);
static void showReceivedDetail();
static void onReceivedActionSelect(uint16_t index, void*);
static void onReceivedSignConfirm(void*);
static void onReceivedDeleteConfirm(void*);

static void showSendKey() {
    // BLE handler is wired in a later step. For now show a placeholder toast so
    // the menu entry is discoverable but not misleading.
    ui::showToast("Send Key (BLE) - WIP");
}

static void showReceivedKeys() {
    rebuildReceivedList();
    if (s_recvListView.getItemCount() == 0) {
        ui::showInfo(ui::tr("mod_gpg.recv_title"), ui::tr("mod_gpg.recv_none"));
        return;
    }
    ui::ViewStack::instance().push(&s_recvListView);
}

static void rebuildReceivedList() {
    auto& store = GpgRecvStore::instance();
    auto idxBuf = ::cdc::core::psramAlloc<gpg_recv_index_entry_t>(GpgRecvStore::kMaxKeys);
    if (!idxBuf) {
        s_recvListView.init(ui::tr("mod_gpg.recv_title"), s_recvListItems, 0);
        return;
    }
    uint8_t n = store.listIndex(idxBuf.get(), GpgRecvStore::kMaxKeys);
    for (uint8_t i = 0; i < n; ++i) {
        gpg_recv_key_t k = {};
        if (!store.getKey(i, &k)) {
            std::snprintf(s_recvListLabels[i], sizeof(s_recvListLabels[i]), "?");
        } else {
            const char* tag = (k.sig_len > 0) ? "[x]" : "[ ]";
            std::snprintf(s_recvListLabels[i], sizeof(s_recvListLabels[i]),
                          "%s %s", tag, k.user_id);
        }
        s_recvListItems[i] = { s_recvListLabels[i], 0, false,
                               reinterpret_cast<void*>(static_cast<uintptr_t>(i)) };
    }
    s_recvListView.init(ui::tr("mod_gpg.recv_title"), s_recvListItems, n);
    s_recvListView.setOnSelect(onReceivedListSelect);
}

static void onReceivedListSelect(uint16_t, void* userData) {
    s_recvSelectedIndex = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(userData));
    if (!GpgRecvStore::instance().getKey(s_recvSelectedIndex, &s_recvSelectedKey)) {
        return;
    }
    showReceivedDetail();
}

static void showReceivedDetail() {
    char fp_v4[41];
    fp_to_hex(s_recvSelectedKey.fingerprint_v4, 20, fp_v4, sizeof(fp_v4));

    char fp_grouped[64] = {};
    {
        size_t pos = 0;
        for (size_t i = 0; i < 40 && pos < sizeof(fp_grouped) - 6; i += 4) {
            std::memcpy(fp_grouped + pos, fp_v4 + i, 4);
            pos += 4;
            if (i + 4 < 40) fp_grouped[pos++] = ' ';
        }
        fp_grouped[pos] = '\0';
    }

    const char* curveName = (s_recvSelectedKey.curve == CDC_CURVE_ED25519)
                                ? ui::tr("mod_gpg.curve_ed25519")
                                : ui::tr("mod_gpg.curve_p256");

    char timeBuf[32];
    time_t t = static_cast<time_t>(s_recvSelectedKey.received_at);
    struct tm tm_v;
    gmtime_r(&t, &tm_v);
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &tm_v);

    std::snprintf(s_recvDetailText, sizeof(s_recvDetailText),
                  "%s\n%s\nFP: %s\nRcvd: %s\n%s",
                  s_recvSelectedKey.user_id,
                  curveName,
                  fp_grouped,
                  timeBuf,
                  s_recvSelectedKey.sig_len > 0
                      ? ui::tr("mod_gpg.recv_signed")
                      : ui::tr("mod_gpg.recv_unsigned"));

    s_recvActionItems[0] = { ui::tr("mod_gpg.recv_sign"),   0, false, reinterpret_cast<void*>(1) };
    s_recvActionItems[1] = { ui::tr("mod_gpg.recv_export"), 0,
                             s_recvSelectedKey.sig_len == 0, reinterpret_cast<void*>(2) };
    s_recvActionItems[2] = { ui::tr("mod_gpg.recv_delete"), 0, false, reinterpret_cast<void*>(3) };
    s_recvActionView.init(ui::tr("mod_gpg.recv_details"), s_recvActionItems, 3);
    s_recvActionView.setOnSelect(onReceivedActionSelect);

    // Show details in a static InfoView, then push the action list on top.
    s_infoView.init(ui::tr("mod_gpg.recv_details"), s_recvDetailText);
    ui::ViewStack::instance().push(&s_infoView);
    ui::ViewStack::instance().push(&s_recvActionView);
}

static void onReceivedActionSelect(uint16_t, void* userData) {
    const uintptr_t action = reinterpret_cast<uintptr_t>(userData);
    switch (action) {
        case 1:
            ui::showConfirm(ui::tr("mod_gpg.recv_confirm_sign"),
                            onReceivedSignConfirm, nullptr,
                            ui::ConfirmView::Icon::QUESTION, nullptr);
            break;
        case 2: {
            size_t out_len = 0;
            if (!gpgBuildSignedKeyArmored(s_recvSelectedKey, s_recvExportBuf,
                                          sizeof(s_recvExportBuf), &out_len)) {
                ui::showToast(ui::tr("mod_gpg.toast_sign_fail"));
                return;
            }
            s_infoView.init(ui::tr("mod_gpg.recv_export_title"), s_recvExportBuf);
            ui::ViewStack::instance().push(&s_infoView);
            break;
        }
        case 3:
            ui::showConfirm(ui::tr("mod_gpg.recv_confirm_del"),
                            onReceivedDeleteConfirm, nullptr,
                            ui::ConfirmView::Icon::WARNING, nullptr);
            break;
        default: break;
    }
}

static void onReceivedSignConfirm(void*) {
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    uint8_t sig[64] = {0};
    if (!gpgCrossSign(s_recvSelectedKey, now, sig)) {
        ui::showToast(ui::tr("mod_gpg.toast_sign_fail"));
        return;
    }
    if (!GpgRecvStore::instance().setSignature(
            s_recvSelectedIndex, sig, 64,
            s_recvSelectedKey.flags | kGpgRecvFlagVerified)) {
        ui::showToast(ui::tr("mod_gpg.toast_sign_fail"));
        return;
    }
    // Refresh local snapshot + list label.
    GpgRecvStore::instance().getKey(s_recvSelectedIndex, &s_recvSelectedKey);
    rebuildReceivedList();
    ui::showToast(ui::tr("mod_gpg.toast_signed"));
}

static void onReceivedDeleteConfirm(void*) {
    GpgRecvStore::instance().deleteKey(s_recvSelectedIndex);
    rebuildReceivedList();
    // Pop ActionView + InfoView back to the list.
    ui::ViewStack::instance().pop();
    ui::ViewStack::instance().pop();
    ui::showToast(ui::tr("mod_gpg.toast_deleted"));
}

/**
 * \brief Returns singleton GPG module instance.
 * \return Module singleton reference.
 */
GpgModule& GpgModule::instance() {
    static GpgModule inst;
    return inst;
}

/**
 * \brief Initializes GPG module resources and slot assignments.
 * \return `true` if initialization succeeded.
 */
bool GpgModule::init() {
    LOG_I(TAG, "Initializing GPG module");
    registerStrings();
    registerCommands();

    core::ModuleRegistry::instance().registerModule(this);
    if (!slotRange_.hasEcc) {
        core::ModuleRegistry::instance().reportModuleError(getName(), "GPG slot range missing");
        state_ = core::ServiceState::ERROR;
        return false;
    }
    gpg_storage_set_slot_range(slotRange_.eccStart, slotRange_.eccEnd);
    if (slotRange_.hasRmem) {
        gpg_storage_set_rmem_range(slotRange_.rmemStart, slotRange_.rmemEnd);
    }
    if (!gpg_storage_ready()) {
        core::ModuleRegistry::instance().reportModuleError(getName(), "GPG slot range invalid");
        state_ = core::ServiceState::ERROR;
        return false;
    }
    core::ModuleRegistry::instance().clearModuleErrorByName(getName());
    state_ = core::ServiceState::INITIALIZED;
    return true;
}

/**
 * \brief Starts GPG module and registers USB CCID interface.
 * \return `true` if start transition succeeded.
 */
bool GpgModule::start() {
    if (state_ != core::ServiceState::INITIALIZED &&
        state_ != core::ServiceState::STOPPED) {
        return false;
    }

    core::UsbInterfaceSpec spec = {};
    spec.cls = core::UsbInterfaceClass::Ccid;
    spec.name = "OpenPGP SmartCard";
    spec.epInSize = 64;
    spec.epOutSize = 64;
    // Call ccid_init() rather than openpgp_init() directly: it brings up
    // OpenPGP and is the only external reference into ccid.cpp / ccid_driver.cpp.
    // Without it the linker drops the entire CCID translation unit (including
    // our strong usbd_app_driver_get_cb override), leaving tinyusb's weak
    // default in place and the smart-card interface unenumerated.
    if (!ccid_init()) {
        core::ModuleRegistry::instance().reportModuleError(getName(), "CCID init failed");
    }
    if (!core::UsbManager::instance().registerInterface(core::UsbHidInterface::Ccid, getName(), spec)) {
        LOG_W(TAG, "Failed to register CCID interface");
    }

    // Idempotent; logs and returns false if BLE controller isn't ready yet,
    // in which case nothing else here changes (CCID is independent).
    ble_gpg_xsig_init();
    ble_gpg_xsig_set_received_callback([](const gpg_recv_key_t&) {
        // Future: invalidate the cached received-keys ListView. The UI rebuilds
        // on demand each time it is opened so no immediate action is required.
    });

    state_ = core::ServiceState::STARTED;
    return true;
}

/**
 * \brief Stops GPG module and unregisters CCID interface.
 */
void GpgModule::stop() {
    core::UsbManager::instance().unregisterInterface(core::UsbHidInterface::Ccid, getName());
    state_ = core::ServiceState::STOPPED;
}

/**
 * \brief Stores slot range assigned by module registry.
 * \param range Slot assignment.
 */
void GpgModule::setSlotRange(const core::IModule::SlotRange& range) {
    slotRange_ = range;
}

/**
 * \brief Declares slot requirements for GPG module.
 * \return Slot request descriptor.
 */
core::IModule::SlotRequest GpgModule::getSlotRequest() const {
    core::IModule::SlotRequest req = {};
    req.mapName = getName();
    req.minEccSlots = 3;
    req.minRmemSlots = 1;
    return req;
}

/**
 * \brief Provides main-menu entry for GPG module.
 * \param items Output menu item array.
 * \param maxItems Maximum writable entries.
 * \return Number of populated menu items.
 */
uint8_t GpgModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;
    items[0] = {ui::tr("mod_gpg.title"), 60, []() -> ui::IView* {
        if (!s_viewsInitialized) {
            s_menuView.setOnSelect(onMenuSelect);
            s_viewsInitialized = true;
        }
        if (!GpgModule::instance().slotRange_.hasEcc || !GpgModule::instance().slotRange_.hasRmem) {
            ui::showToastError(ui::tr("mod_gpg.slot_error"));
            return nullptr;
        }
        rebuildMenu();
        return &s_menuView;
    }, nullptr, getName(), core::MenuLocation::MAIN_MENU, nullptr};
    return 1;
}

} // namespace cdc::mod_gpg

/**
 * \brief Registers GPG module initializer in global registry.
 */
extern "C" void mod_gpg_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_gpg::GpgModule::instance();
        module.init();
    });
}
