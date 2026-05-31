#include "mod_password/PasswordModule.h"
#include "mod_password/PasswordStore.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/StringUtils.h"
#include "cdc_core/TropicStorage.h"
#include "cdc_core/IKeyboardProvider.h"
#include "cdc_hal/ISecureElement.h"
#include "esp_random.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ListView.h"
#include "cdc_views/ContextMenuView.h"
#include "cdc_views/T9InputView.h"
#include "cdc_views/InfoView.h"
#include "cdc_views/ConfirmView.h"
#include "cdc_views/ToastView.h"
#include "serial_cmd/ICommandRegistry.h"
#include "serial_cmd/SubCommand.h"
#include "serial_cmd/Console.h"
#include "cdc_log.h"
#include "esp_attr.h"
#include <cctype>
#include <cstring>
#include <strings.h>
#include <new>
#include <memory>
#include <cstdio>

static const char* TAG = "PASSWORD";

namespace cdc::mod_password {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_password.title",          "Passwords"},
    {"mod_password.new_entry",      "New Entry"},
    {"mod_password.field_title",    "Title"},
    {"mod_password.username",       "Username"},
    {"mod_password.password",       "Password"},
    {"mod_password.url",            "URL"},
    {"mod_password.totp_slot",      "TOTP Slot (optional)"},
    {"mod_password.notes",          "Notes"},
    {"mod_password.view",           "View"},
    {"mod_password.edit",           "Edit"},
    {"mod_password.delete",         "Delete"},
    {"mod_password.actions",        "Actions"},
    {"mod_password.saved",          "Saved"},
    {"mod_password.deleted",        "Deleted"},
    {"mod_password.invalid_input",  "Invalid input"},
    {"mod_password.slot_error",     "Slot map error"},
    {"mod_password.details",        "Details"},
    {"mod_password.hint_list",      "[Y] View  [3] Menu  [N] Back"},
    {"mod_password.confirm_delete", "Delete entry?"},
    {"mod_password.hint_type",      "[Y] Type  [2/8] Scroll  [N] Back"},
    {"mod_password.no_keyboard",    "No keyboard connected"},
};

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

/** \brief Serial command handlers for password module. */

static constexpr const char* CMD_MODULE = "password";
static bool s_commandsRegistered = false;

using cdc::core::skipSpaces;
using cdc::core::nextToken;

/**
 * \brief Validates that a slot number is within the configured password range.
 * \param slot Logical slot number.
 * \return `true` if slot index is in range.
 */
static bool isValidSlot(uint16_t slot) {
    auto& store = PasswordStore::instance();
    return store.hasSlotRange() && slot < store.capacity();
}

/**
 * \brief Serial command handler listing all password entries.
 * \param args Unused command arguments.
 */
static void cmd_password_list(const char* args) {
    (void)args;
    auto& store = PasswordStore::instance();
    if (!store.hasSlotRange()) {
        cdc::serial::Console::printf("ERROR: slot map not configured\r\n");
        return;
    }
    uint16_t cap = store.capacity();
    if (cap == 0) {
        cdc::serial::Console::printf("(no entries)\r\n");
        return;
    }
    auto list = std::unique_ptr<PasswordStore::EntryIndex[]>(new (std::nothrow) PasswordStore::EntryIndex[cap]);
    if (!list) {
        cdc::serial::Console::printf("ERROR: out of memory\r\n");
        return;
    }
    uint16_t count = 0;
    if (!store.listEntriesSorted(list.get(), cap, &count)) {
        cdc::serial::Console::printf("ERROR: list failed\r\n");
        return;
    }
    if (count == 0) {
        cdc::serial::Console::printf("(no entries)\r\n");
        return;
    }
    for (uint16_t i = 0; i < count; i++) {
        cdc::serial::Console::printf("slot %u: %s\r\n", list[i].slot, list[i].title);
    }
}

/**
 * \brief Serial command handler printing one password entry by index.
 * \param args Command arguments (`<index>`).
 */
static void cmd_password_get(const char* args) {
    char slotBuf[8] = {};
    const char* p = nextToken(args, slotBuf, sizeof(slotBuf));
    if (!p || !slotBuf[0]) {
        cdc::serial::Console::printf("Usage: PASSWORD GET <slot>\r\n");
        return;
    }
    uint16_t slot = static_cast<uint16_t>(atoi(slotBuf));
    if (!isValidSlot(slot)) {
        cdc::serial::Console::printf("ERROR: slot out of range\r\n");
        return;
    }
    PasswordEntry entry = {};
    if (!PasswordStore::instance().readEntry(slot, &entry)) {
        cdc::serial::Console::printf("ERROR: empty slot or read failed\r\n");
        return;
    }
    cdc::serial::Console::printf("Title: %s\r\n", entry.title);
    cdc::serial::Console::printf("Username: %s\r\n", entry.username);
    cdc::serial::Console::printf("Password: %s\r\n", entry.password);
    cdc::serial::Console::printf("URL: %s\r\n", entry.url);
    if (entry.totpSlot == PasswordStore::TOTP_SLOT_NONE) {
        cdc::serial::Console::printf("TOTP Slot: none\r\n");
    } else {
        cdc::serial::Console::printf("TOTP Slot: %u\r\n", entry.totpSlot);
    }
    cdc::serial::Console::printf("Notes: %s\r\n", entry.notes);
}

/**
 * \brief Serial command handler adding one password entry.
 * \param args Command arguments (`<title> <username|- > <password> <url|- > [totpSlot] [notes]`).
 */
static bool isPlaceholder(const char* s) {
    return s && s[0] && s[1] == '\0' && (s[0] == 'x' || s[0] == 'X' || s[0] == '-');
}

/**
 * \brief Generates a 16-character random password from charset a-zA-Z0-9$!%=.
 * \param out Output buffer (must hold at least 17 bytes).
 */
static void generateRandomPassword(char* out, size_t outSize) {
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        "$!%=";
    constexpr size_t charsetLen = sizeof(charset) - 1;
    constexpr uint8_t charCount = 16;
    if (outSize < charCount + 1) return;

    uint8_t rand[charCount] = {};
    auto* se = cdc::hal::getSecureElementInstance();
    bool gotRand = se && se->getRandom(rand, sizeof(rand));
    if (!gotRand) {
        for (uint8_t i = 0; i < charCount; i++) {
            rand[i] = static_cast<uint8_t>(esp_random() & 0xFF);
        }
    }

    for (uint8_t i = 0; i < charCount; i++) {
        out[i] = charset[rand[i] % charsetLen];
    }
    out[charCount] = '\0';
}

static void cmd_password_add(const char* args) {
    PasswordEntry entry = {};
    entry.totpSlot = PasswordStore::TOTP_SLOT_NONE;

    char slotBuf[8] = {};
    char title[PasswordStore::TITLE_LEN + 1] = {};
    char username[PasswordStore::USERNAME_LEN + 1] = {};
    char password[PasswordStore::PASSWORD_LEN + 1] = {};
    char url[PasswordStore::URL_LEN + 1] = {};
    char totpBuf[8] = {};

    static constexpr const char* USAGE =
        "Usage: PASSWORD ADD <slot|x> <title> <username|x> <password|x> <url|x> <totp|-> [notes]\r\n"
        "  <slot>: target RMEM slot, or 'x' for next free. Overwrites if occupied.\r\n"
        "  <password>: value, or 'x' to generate a random 16-char password.\r\n"
        "  <totp>: linked TOTP slot number, or '-' for no link.\r\n"
        "  Use 'x' for username/url to skip them. Use '\\\\ ' for spaces inside fields.\r\n";

    const char* p = nextToken(args, slotBuf, sizeof(slotBuf));
    if (!p || !slotBuf[0]) {
        cdc::serial::Console::printf("%s", USAGE);
        return;
    }
    p = nextToken(p, title, sizeof(title));
    p = nextToken(p, username, sizeof(username));
    p = nextToken(p, password, sizeof(password));
    p = nextToken(p, url, sizeof(url));
    p = nextToken(p, totpBuf, sizeof(totpBuf));
    if (!title[0] || !username[0] || !password[0] || !url[0] || !totpBuf[0]) {
        cdc::serial::Console::printf("%s", USAGE);
        return;
    }

    const char* notes = skipSpaces(p);

    auto& store = PasswordStore::instance();

    uint16_t slot = 0;
    if (isPlaceholder(slotBuf)) {
        if (!store.findFreeLogicalSlot(&slot)) {
            cdc::serial::Console::printf("ERROR: no free slots\r\n");
            return;
        }
    } else {
        slot = static_cast<uint16_t>(atoi(slotBuf));
        if (!isValidSlot(slot)) {
            cdc::serial::Console::printf("ERROR: slot out of range\r\n");
            return;
        }
    }

    strncpy(entry.title, title, sizeof(entry.title) - 1);
    if (!isPlaceholder(username)) {
        strncpy(entry.username, username, sizeof(entry.username) - 1);
    }

    char generatedPassword[40] = {};
    if (password[0] == 'x' && password[1] == '\0') {
        generateRandomPassword(generatedPassword, sizeof(generatedPassword));
        strncpy(entry.password, generatedPassword, sizeof(entry.password) - 1);
    } else if (!isPlaceholder(password)) {
        strncpy(entry.password, password, sizeof(entry.password) - 1);
    }

    if (!isPlaceholder(url)) {
        strncpy(entry.url, url, sizeof(entry.url) - 1);
    }

    if (totpBuf[0] != '-' || totpBuf[1] != '\0') {
        int totp = atoi(totpBuf);
        if (totp < 0 || totp > 254) {
            cdc::serial::Console::printf("ERROR: totp slot out of range (0-254 or '-')\r\n");
            return;
        }
        entry.totpSlot = static_cast<uint8_t>(totp);
    }

    if (notes && notes[0]) {
        strncpy(entry.notes, notes, sizeof(entry.notes) - 1);
        cdc::core::unescapeSpaces(entry.notes);
    }

    bool ok = store.updateEntry(slot, entry);
    if (ok) {
        if (generatedPassword[0]) {
            cdc::serial::Console::printf("Generated password: %s\r\n", generatedPassword);
        }
        cdc::serial::Console::printf("OK (slot %u)\r\n", slot);
    } else {
        cdc::serial::Console::printf("ERROR\r\n");
    }
}

/**
 * \brief Serial command handler editing one field of a password entry.
 *        Usage: PASSWORD_EDIT <index> <field> <new value...>
 *        Field: title | username | password | url | totp | notes
 */
static void cmd_password_edit(const char* args) {
    char slotBuf[8] = {};
    char field[16] = {};
    const char* usage =
        "Usage: PASSWORD EDIT <slot> <title|username|password|url|totp|notes> <value>\r\n"
        "  Use '\\\\ ' for spaces inside values.\r\n";

    const char* p = nextToken(args, slotBuf, sizeof(slotBuf));
    if (!p || !slotBuf[0]) {
        cdc::serial::Console::printf("%s", usage);
        return;
    }
    p = nextToken(p, field, sizeof(field));
    if (!field[0]) {
        cdc::serial::Console::printf("%s", usage);
        return;
    }

    uint16_t slot = static_cast<uint16_t>(atoi(slotBuf));
    if (!isValidSlot(slot)) {
        cdc::serial::Console::printf("ERROR: slot out of range\r\n");
        return;
    }

    PasswordEntry entry = {};
    if (!PasswordStore::instance().readEntry(slot, &entry)) {
        cdc::serial::Console::printf("ERROR: empty slot or read failed\r\n");
        return;
    }

    const char* value = skipSpaces(p);
    if (!value || !value[0]) {
        cdc::serial::Console::printf("ERROR: empty value\r\n");
        return;
    }

    if (strcasecmp(field, "title") == 0) {
        if (strlen(value) > PasswordStore::TITLE_LEN) {
            cdc::serial::Console::printf("ERROR: title too long (max %u)\r\n", PasswordStore::TITLE_LEN);
            return;
        }
        memset(entry.title, 0, sizeof(entry.title));
        strncpy(entry.title, value, sizeof(entry.title) - 1);
        cdc::core::unescapeSpaces(entry.title);
    } else if (strcasecmp(field, "username") == 0) {
        if (strlen(value) > PasswordStore::USERNAME_LEN) {
            cdc::serial::Console::printf("ERROR: username too long (max %u)\r\n", PasswordStore::USERNAME_LEN);
            return;
        }
        memset(entry.username, 0, sizeof(entry.username));
        strncpy(entry.username, value, sizeof(entry.username) - 1);
        cdc::core::unescapeSpaces(entry.username);
    } else if (strcasecmp(field, "password") == 0) {
        if (strlen(value) > PasswordStore::PASSWORD_LEN) {
            cdc::serial::Console::printf("ERROR: password too long (max %u)\r\n", PasswordStore::PASSWORD_LEN);
            return;
        }
        memset(entry.password, 0, sizeof(entry.password));
        strncpy(entry.password, value, sizeof(entry.password) - 1);
        cdc::core::unescapeSpaces(entry.password);
    } else if (strcasecmp(field, "url") == 0) {
        if (strlen(value) > PasswordStore::URL_LEN) {
            cdc::serial::Console::printf("ERROR: url too long (max %u)\r\n", PasswordStore::URL_LEN);
            return;
        }
        memset(entry.url, 0, sizeof(entry.url));
        strncpy(entry.url, value, sizeof(entry.url) - 1);
        cdc::core::unescapeSpaces(entry.url);
    } else if (strcasecmp(field, "totp") == 0) {
        int totp = atoi(value);
        if (totp < 0 || totp > 254) {
            cdc::serial::Console::printf("ERROR: totp slot out of range (0-254)\r\n");
            return;
        }
        entry.totpSlot = static_cast<uint8_t>(totp);
    } else if (strcasecmp(field, "notes") == 0) {
        if (strlen(value) > PasswordStore::NOTES_LEN) {
            cdc::serial::Console::printf("ERROR: notes too long (max %u)\r\n", static_cast<unsigned>(PasswordStore::NOTES_LEN));
            return;
        }
        memset(entry.notes, 0, sizeof(entry.notes));
        strncpy(entry.notes, value, sizeof(entry.notes) - 1);
        cdc::core::unescapeSpaces(entry.notes);
    } else {
        cdc::serial::Console::printf("%s", usage);
        return;
    }

    bool ok = PasswordStore::instance().updateEntry(slot, entry);
    cdc::serial::Console::printf(ok ? "OK\r\n" : "ERROR\r\n");
}

/**
 * \brief Serial command handler deleting one password entry by index.
 * \param args Command arguments (`<index>`).
 */
static void cmd_password_del(const char* args) {
    char slotBuf[8] = {};
    const char* p = nextToken(args, slotBuf, sizeof(slotBuf));
    if (!p || !slotBuf[0]) {
        cdc::serial::Console::printf("Usage: PASSWORD DEL <slot>\r\n");
        return;
    }
    uint16_t slot = static_cast<uint16_t>(atoi(slotBuf));
    if (!isValidSlot(slot)) {
        cdc::serial::Console::printf("ERROR: slot out of range\r\n");
        return;
    }
    bool ok = PasswordStore::instance().deleteEntry(slot);
    cdc::serial::Console::printf(ok ? "OK\r\n" : "ERROR\r\n");
}

static const cdc::serial::SubCommand kPasswordSubs[] = {
    {"LIST", "",                                                                        "List password entries (sorted by title)", cmd_password_list},
    {"GET",  "<slot>",                                                                  "Show one entry by slot",                  cmd_password_get},
    {"ADD",  "<slot|x> <title> <user|x> <pw|x> <url|x> <totp|-> [notes]",                "Add entry; 'x' for fields skips them",    cmd_password_add},
    {"EDIT", "<slot> <field> <value>",                                                   "Edit one field of an existing entry",     cmd_password_edit},
    {"DEL",  "<slot>",                                                                  "Delete entry by slot",                    cmd_password_del},
    {nullptr, nullptr, nullptr, nullptr},
};

static void cmd_password(const char* args) {
    cdc::serial::dispatchSubCommand("PASSWORD", args, kPasswordSubs);
}

/**
 * \brief Registers serial commands exposed by the password module.
 */
static void registerCommands() {
    if (s_commandsRegistered) return;
    s_commandsRegistered = true;

    auto& reg = cdc::serial::getCommandRegistry();
    reg.registerCommand({"PASSWORD",
                         "Password vault: LIST/GET/ADD/EDIT/DEL",
                         cmd_password, CMD_MODULE, true, kPasswordSubs});
}

/** \brief Password module UI state and reusable view instances. */

static ui::ListView s_listView;
static ui::T9InputView s_t9Input;
static ui::InfoView s_infoView;
static bool s_viewsInitialized = false;

static ui::ListItem* s_listItems = nullptr;
static PasswordStore::EntryIndex* s_entries = nullptr;
static uint16_t s_entryCount = 0;
static uint16_t s_capacity = 0;

static uint16_t s_activeSlot = 0;

struct WizardState {
    PasswordEntry entry;
    bool editMode;
    uint16_t editSlot;
};

EXT_RAM_BSS_ATTR static WizardState s_wizard = {};

static constexpr uint16_t NOTES_INPUT_MAX =
    (PasswordStore::NOTES_LEN < ui::T9InputView::MAX_TEXT_LEN)
        ? static_cast<uint16_t>(PasswordStore::NOTES_LEN)
        : static_cast<uint16_t>(ui::T9InputView::MAX_TEXT_LEN);

/**
 * \brief Releases dynamic buffers used by the password list view.
 */
static void freeListBuffers() {
    delete[] s_listItems;
    delete[] s_entries;
    s_listItems = nullptr;
    s_entries = nullptr;
    s_capacity = 0;
    s_entryCount = 0;
}

/**
 * \brief Ensures list and entry buffers are allocated for current store capacity.
 * \return `true` when buffers are ready for use.
 */
static bool ensureListBuffers() {
    uint16_t cap = PasswordStore::instance().capacity();
    if (cap == 0) return false;
    if (cap == s_capacity && s_listItems && s_entries) return true;

    delete[] s_listItems;
    delete[] s_entries;
    s_listItems = nullptr;
    s_entries = nullptr;
    s_capacity = 0;

    s_listItems = new (std::nothrow) ui::ListItem[cap + 1];
    s_entries = new (std::nothrow) PasswordStore::EntryIndex[cap];
    if (!s_listItems || !s_entries) {
        delete[] s_listItems;
        delete[] s_entries;
        s_listItems = nullptr;
        s_entries = nullptr;
        s_capacity = 0;
        return false;
    }
    s_capacity = cap;
    return true;
}

/**
 * \brief Rebuilds password list items from sorted store entries.
 */
static void rebuildList() {
    if (!PasswordStore::instance().hasSlotRange()) {
        ui::showToastError(ui::tr("mod_password.slot_error"));
        return;
    }
    if (!ensureListBuffers()) {
        cdc::core::ModuleRegistry::instance().reportModuleError(PasswordModule::instance().getName(),
                                                               "Password list allocation failed");
        return;
    }
    s_entryCount = 0;
    s_listItems[0] = {ui::tr("mod_password.new_entry"), 0, false, nullptr};

    uint16_t count = 0;
    PasswordStore::instance().listEntriesSorted(s_entries, s_capacity, &count);
    s_entryCount = count;

    for (uint16_t i = 0; i < s_entryCount; i++) {
        uint16_t idx = static_cast<uint16_t>(i + 1);
        s_listItems[idx].label = s_entries[i].title;
        s_listItems[idx].icon = 0;
        s_listItems[idx].iconDisabled = false;
        s_listItems[idx].userData = reinterpret_cast<void*>(static_cast<uintptr_t>(s_entries[i].slot));
    }

    s_listView.init(ui::tr("mod_password.title"), s_listItems, static_cast<uint16_t>(s_entryCount + 1));
s_listView.setHint(ui::tr("mod_password.hint_list"));
}

/** \brief Shared output buffer used for keyboard typing callback payload. */
static char s_passwordToType[PasswordStore::PASSWORD_LEN + 1] = {};

/**
 * \brief Types currently selected password through attached keyboard provider.
 * \param userData Optional user pointer (unused).
 */
static void onTypePassword(void* userData) {
    (void)userData;
    auto* kb = core::getKeyboard();
    if (kb && kb->isConnected()) {
        if (s_passwordToType[0]) {
            kb->typeString(s_passwordToType);
            ui::showToastSuccess("Typed");
        }
    } else {
        ui::showToastError(ui::tr("mod_password.no_keyboard"));
    }
}

/**
 * \brief Shows full entry details in the info view for a slot.
 * \param slot Logical password slot.
 */
static void showDetails(uint16_t slot) {
    PasswordEntry entry = {};
    if (!PasswordStore::instance().readEntry(slot, &entry)) {
        ui::showToastError(ui::tr("core.failed"));
        return;
    }

    // Store password for type callback
    strncpy(s_passwordToType, entry.password, sizeof(s_passwordToType) - 1);
    s_passwordToType[sizeof(s_passwordToType) - 1] = '\0';

    static EXT_RAM_BSS_ATTR char detailText[ui::InfoView::MAX_TEXT_LEN];
    char totpBuf[16] = {};
    const char* emptyText = ui::tr("core.empty");
    char emptyWrapped[16] = {};
    snprintf(emptyWrapped, sizeof(emptyWrapped), "(%s)", emptyText);
    const char* totpText = emptyWrapped;
    if (entry.totpSlot != PasswordStore::TOTP_SLOT_NONE) {
        snprintf(totpBuf, sizeof(totpBuf), "%u", entry.totpSlot);
        totpText = totpBuf;
    }
    const char* usernameText = entry.username[0] ? entry.username : emptyWrapped;
    const char* passwordText = entry.password[0] ? entry.password : emptyWrapped;
    const char* urlText = entry.url[0] ? entry.url : emptyWrapped;
    const char* notesText = entry.notes[0] ? entry.notes : emptyWrapped;

    snprintf(detailText, sizeof(detailText),
             "Title: %s\n"
             "Username: %s\n"
             "Password: %s\n"
             "URL: %s\n"
             "TOTP Slot: %s\n"
             "Notes: %s",
             entry.title,
             usernameText,
             passwordText,
             urlText,
             totpText,
             notesText);

    s_infoView.init(ui::tr("mod_password.details"), detailText);

    // Set up Type callback if keyboard is available
    auto* kb = core::getKeyboard();
    if (kb && kb->isConnected() && s_passwordToType[0]) {
        s_infoView.setYesNoCallbacks(onTypePassword, nullptr, nullptr);
        s_infoView.setHint(ui::tr("mod_password.hint_type"));
    } else {
        s_infoView.setYesNoCallbacks(nullptr, nullptr, nullptr);
        s_infoView.setHint(nullptr);
    }

    ui::ViewStack::instance().push(&s_infoView);
}

/**
 * \brief Persists wizard add/edit changes and returns to list view.
 */
static void wizardFinish() {
    bool ok = false;
    if (s_wizard.editMode) {
        ok = PasswordStore::instance().updateEntry(s_wizard.editSlot, s_wizard.entry);
    } else {
        ok = PasswordStore::instance().addEntry(s_wizard.entry);
    }

    if (ok) {
        ui::showToastSuccess(ui::tr("mod_password.saved"));
        s_listView.preservePosition();
        rebuildList();
        ui::ViewStack::instance().popToAnchor(&s_listView);
    } else {
        ui::showToastError(ui::tr("core.failed"));
    }
}

/**
 * \brief Pushes a configured T9 input step for wizard flow.
 * \param title Step title.
 * \param initialText Initial input text.
 * \param maxLen Maximum accepted text length.
 * \param onSave Save callback for this step.
 */
static void pushT9WizardStep(const char* title, const char* initialText,
                             uint16_t maxLen, ui::T9InputView::SaveCallback onSave) {
    s_t9Input.init(title, initialText, maxLen);
    s_t9Input.setOnSave(onSave);
    ui::ViewStack::instance().push(&s_t9Input);
}

static void onWizardTitle(const char* text);
static void onWizardUsername(const char* text);
static void onWizardPassword(const char* text);
static void onWizardUrl(const char* text);
static void onWizardTotp(const char* text);
static void onWizardNotes(const char* text);

/**
 * \brief Starts add-entry wizard with empty fields.
 */
static void wizardStart() {
    memset(&s_wizard, 0, sizeof(s_wizard));
    s_wizard.entry.totpSlot = PasswordStore::TOTP_SLOT_NONE;
    s_wizard.editMode = false;
    s_wizard.editSlot = 0;

    pushT9WizardStep(ui::tr("mod_password.field_title"), nullptr, PasswordStore::TITLE_LEN, onWizardTitle);
}

/**
 * \brief Starts edit-entry wizard prefilled with existing slot data.
 * \param slot Logical slot to edit.
 */
static void wizardEdit(uint16_t slot) {
    PasswordEntry entry = {};
    if (!PasswordStore::instance().readEntry(slot, &entry)) {
        ui::showToastError(ui::tr("core.failed"));
        return;
    }

    memset(&s_wizard, 0, sizeof(s_wizard));
    s_wizard.entry = entry;
    s_wizard.editMode = true;
    s_wizard.editSlot = slot;

    pushT9WizardStep(ui::tr("mod_password.field_title"), s_wizard.entry.title, PasswordStore::TITLE_LEN, onWizardTitle);
}

/**
 * \brief Saves title field and advances to username step.
 * \param text Entered title text.
 */
static void onWizardTitle(const char* text) {
    strncpy(s_wizard.entry.title, text ? text : "", sizeof(s_wizard.entry.title) - 1);
    s_wizard.entry.title[sizeof(s_wizard.entry.title) - 1] = '\0';
    pushT9WizardStep(ui::tr("mod_password.username"), s_wizard.entry.username, PasswordStore::USERNAME_LEN, onWizardUsername);
}

/**
 * \brief Saves username field and advances to password step.
 * \param text Entered username text.
 */
static void onWizardUsername(const char* text) {
    strncpy(s_wizard.entry.username, text ? text : "", sizeof(s_wizard.entry.username) - 1);
    s_wizard.entry.username[sizeof(s_wizard.entry.username) - 1] = '\0';
    s_t9Input.init(ui::tr("mod_password.password"), s_wizard.entry.password, PasswordStore::PASSWORD_LEN);
    s_t9Input.setHint("x=Random Y=OK N=Back");
    s_t9Input.setOnSave(onWizardPassword);
    ui::ViewStack::instance().push(&s_t9Input);
}

/**
 * \brief Saves password field; an "x" input generates a random 16-char password
 *        via the shared generator. Then advances to URL step.
 * \param text Entered password text.
 */
static void onWizardPassword(const char* text) {
    if (text && text[0] == 'x' && text[1] == '\0') {
        char generated[40] = {};
        generateRandomPassword(generated, sizeof(generated));
        strncpy(s_wizard.entry.password, generated, sizeof(s_wizard.entry.password) - 1);
    } else {
        strncpy(s_wizard.entry.password, text ? text : "", sizeof(s_wizard.entry.password) - 1);
    }
    s_wizard.entry.password[sizeof(s_wizard.entry.password) - 1] = '\0';
    pushT9WizardStep(ui::tr("mod_password.url"), s_wizard.entry.url, PasswordStore::URL_LEN, onWizardUrl);
}

/**
 * \brief Saves URL field and advances to optional TOTP slot step.
 * \param text Entered URL text.
 */
static void onWizardUrl(const char* text) {
    strncpy(s_wizard.entry.url, text ? text : "", sizeof(s_wizard.entry.url) - 1);
    s_wizard.entry.url[sizeof(s_wizard.entry.url) - 1] = '\0';

    char totpBuf[8] = {};
    if (s_wizard.entry.totpSlot != PasswordStore::TOTP_SLOT_NONE) {
        snprintf(totpBuf, sizeof(totpBuf), "%u", s_wizard.entry.totpSlot);
    }
    pushT9WizardStep(ui::tr("mod_password.totp_slot"), totpBuf, 3, onWizardTotp);
}

/**
 * \brief Validates and saves optional TOTP slot, then advances to notes step.
 * \param text Entered TOTP slot text.
 */
static void onWizardTotp(const char* text) {
    if (!text || !text[0]) {
        s_wizard.entry.totpSlot = PasswordStore::TOTP_SLOT_NONE;
    } else {
        int value = atoi(text);
        if (value < 0 || value > 254) {
            ui::showToastError(ui::tr("mod_password.invalid_input"));
            pushT9WizardStep(ui::tr("mod_password.totp_slot"), text, 3, onWizardTotp);
            return;
        }
        s_wizard.entry.totpSlot = static_cast<uint8_t>(value);
    }

    pushT9WizardStep(ui::tr("mod_password.notes"), s_wizard.entry.notes, NOTES_INPUT_MAX, onWizardNotes);
}

/**
 * \brief Saves notes field and completes wizard persistence.
 * \param text Entered notes text.
 */
static void onWizardNotes(const char* text) {
    strncpy(s_wizard.entry.notes, text ? text : "", sizeof(s_wizard.entry.notes) - 1);
    s_wizard.entry.notes[sizeof(s_wizard.entry.notes) - 1] = '\0';
    wizardFinish();
}

/**
 * \brief Opens details view for currently active entry.
 */
static void onMenuView() {
    showDetails(s_activeSlot);
}

/**
 * \brief Opens edit wizard for currently active entry.
 */
static void onMenuEdit() {
    wizardEdit(s_activeSlot);
}

/**
 * \brief Confirmation callback deleting selected entry slot.
 * \param userData Pointer to selected slot value.
 */
static void onMenuDeleteConfirm(void* userData) {
    uint16_t slot = *static_cast<uint16_t*>(userData);
    bool ok = PasswordStore::instance().deleteEntry(slot);
    if (ok) {
        ui::showToastSuccess(ui::tr("mod_password.deleted"));
        s_listView.preservePosition();
        rebuildList();
        ui::ViewStack::instance().popToAnchor(&s_listView);
    } else {
        ui::showToastError(ui::tr("core.failed"));
    }
}

/**
 * \brief Opens delete confirmation dialog for currently active entry.
 */
static void onMenuDelete() {
    static uint16_t slot = 0;
    slot = s_activeSlot;
    ui::showConfirm(ui::tr("mod_password.confirm_delete"), onMenuDeleteConfirm, nullptr,
                    ui::ConfirmView::Icon::WARNING, &slot);
}

/**
 * \brief Opens contextual action menu for selected list entry.
 * \param index Selected row index.
 * \param userData Optional user pointer (unused).
 */
static void onListMenu(uint16_t index, void* userData) {
    (void)userData;
    if (index == 0) {
        static ui::ContextMenuItem items[] = {
            {ui::tr("mod_password.new_entry"), []() { wizardStart(); }}
        };
        ui::showContextMenu(ui::tr("mod_password.actions"), items, 1);
        return;
    }
    if (index - 1 >= s_entryCount) return;
    s_activeSlot = s_entries[index - 1].slot;

    static ui::ContextMenuItem items[] = {
        {ui::tr("mod_password.view"), onMenuView},
        {ui::tr("mod_password.edit"), onMenuEdit},
        {ui::tr("mod_password.delete"), onMenuDelete}
    };
    ui::showContextMenu(ui::tr("mod_password.actions"), items, 3);
}

/**
 * \brief Handles direct selection from list view (view existing or add new).
 * \param index Selected row index.
 * \param userData Optional user pointer (unused).
 */
static void onListSelect(uint16_t index, void* userData) {
    (void)userData;
    if (index == 0) {
        wizardStart();
        return;
    }
    if (index - 1 >= s_entryCount) return;
    s_activeSlot = s_entries[index - 1].slot;
    showDetails(s_activeSlot);
}

/**
 * \brief Returns singleton password module instance.
 * \return Module singleton reference.
 */
PasswordModule& PasswordModule::instance() {
    static PasswordModule inst;
    return inst;
}

/**
 * \brief Initializes module resources, translations, commands, and slot mapping.
 * \return `true` if module initialization succeeded.
 */
bool PasswordModule::init() {
    LOG_I(TAG, "Initializing Password module");
    registerStrings();
    registerCommands();

    core::ModuleRegistry::instance().registerModule(this);
    if (slotRange_.hasRmem) {
        PasswordStore::instance().setSlotRange(slotRange_);
        core::ModuleRegistry::instance().clearModuleErrorByName(getName());
    } else {
        core::ModuleRegistry::instance().reportModuleError(getName(), "Password slot range missing");
        state_ = core::ServiceState::ERROR;
        return false;
    }
    state_ = core::ServiceState::INITIALIZED;
    return true;
}

/**
 * \brief Stops the password module and frees list resources.
 */
void PasswordModule::stop() {
    freeListBuffers();
    ModuleBase::stop();
}

/**
 * \brief Stores assigned Tropic slot range for this module.
 * \param range Slot assignment provided by registry.
 */
void PasswordModule::setSlotRange(const core::IModule::SlotRange& range) {
    slotRange_ = range;
}

/**
 * \brief Declares slot requirements for password storage.
 * \return Slot request structure.
 */
core::IModule::SlotRequest PasswordModule::getSlotRequest() const {
    core::IModule::SlotRequest req = {};
    req.mapName = getName();
    req.minRmemSlots = 1;
    return req;
}

/**
 * \brief Provides main-menu entry for password module UI.
 * \param items Output array for menu items.
 * \param maxItems Maximum writable entries in `items`.
 * \return Number of populated menu items.
 */
uint8_t PasswordModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    items[0] = {ui::tr("mod_password.title"), 55, []() -> ui::IView* {
        if (!s_viewsInitialized) {
            s_listView.setOnSelect(onListSelect);
            s_listView.setOnMenu(onListMenu);
            s_viewsInitialized = true;
        }
        if (!PasswordStore::instance().hasSlotRange()) {
            ui::showToastError(ui::tr("mod_password.slot_error"));
            return nullptr;
        }
        rebuildList();
        return &s_listView;
    }, nullptr, getName(), core::MenuLocation::MAIN_MENU, nullptr};

    return 1;
}

} // namespace cdc::mod_password

/**
 * \brief Registers password module initializer in global module registry.
 */
extern "C" void mod_password_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_password::PasswordModule::instance();
        if (module.init()) {
            module.start();
        }
    });
}
