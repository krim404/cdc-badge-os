#include "mod_totp/TotpModule.h"
#include "mod_totp/TotpStore.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/StringUtils.h"
#include "cdc_core/TropicStorage.h"
#include "cdc_core/IKeyboardProvider.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ListView.h"
#include "cdc_views/T9InputView.h"
#include "cdc_views/ToastView.h"
#include "cdc_hal/IDisplay.h"
#include "serial_cmd/ICommandRegistry.h"
#include "serial_cmd/SubCommand.h"
#include "serial_cmd/Console.h"
#include "cdc_log.h"
#include "esp_attr.h"
#include <goodisplay/gdey029T94.h>
#include <cctype>
#include <cstring>
#include <new>

static const char* TAG = "TOTP";

namespace cdc::mod_totp {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_totp.title",         "TOTP"},
    {"mod_totp.add_account",   "Add Account"},
    {"mod_totp.account_name",  "Account Name"},
    {"mod_totp.secret",        "Secret (Base32)"},
    {"mod_totp.issuer",        "Issuer (optional)"},
    {"mod_totp.digits",        "Digits"},
    {"mod_totp.algorithm",     "Algorithm"},
    {"mod_totp.period",        "Period"},
    {"mod_totp.code",          "TOTP Code"},
    {"mod_totp.time_invalid",  "Time not set"},
    {"mod_totp.invalid_input", "Invalid input"},
    {"mod_totp.hint_edit",     "[3] Edit  [N] Back"},
    {"mod_totp.hint_type",     "[Y] Type  [3] Edit  [N] Back"},
    {"mod_totp.no_keyboard",   "No keyboard connected"},
};

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

/** \brief Serial command handlers for TOTP module. */

static constexpr const char* CMD_MODULE = "totp";
static bool s_commandsRegistered = false;

using cdc::core::skipSpaces;
using cdc::core::nextToken;

/**
 * \brief Parses textual or numeric algorithm identifiers into store values.
 * \param token Algorithm token (`sha1`, `sha256`, `sha512`, or numeric).
 * \return Encoded algorithm value used by `TotpStore`.
 */
static uint8_t parseAlgo(const char* token) {
    if (!token || !*token) return static_cast<uint8_t>(TotpAlgorithm::SHA1);
    char buf[8] = {};
    size_t i = 0;
    for (; token[i] && i + 1 < sizeof(buf); i++) {
        buf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(token[i])));
    }
    buf[i] = '\0';
    if (strcmp(buf, "sha1") == 0) return static_cast<uint8_t>(TotpAlgorithm::SHA1);
    if (strcmp(buf, "sha256") == 0) return static_cast<uint8_t>(TotpAlgorithm::SHA256);
    if (strcmp(buf, "sha512") == 0) return static_cast<uint8_t>(TotpAlgorithm::SHA512);
    // Numeric fallback with bounds check: only accept supported algorithm IDs.
    int value = atoi(buf);
    if (value < 0 || value > static_cast<int>(TotpAlgorithm::SHA512)) {
        return static_cast<uint8_t>(TotpAlgorithm::SHA1);
    }
    return static_cast<uint8_t>(value);
}

/**
 * \brief Resolves a displayed list index to the logical TOTP slot number.
 * \param index UI list index.
 * \param slotOut Output logical slot.
 * \return `true` if a matching slot was found.
 */
static bool findSlotByIndex(uint16_t index, uint16_t* slotOut) {
    if (!slotOut) return false;
    auto& store = TotpStore::instance();
    if (!store.hasSlotRange()) return false;
    struct Ctx {
        uint16_t target;
        uint16_t current;
        uint16_t slot;
        bool found;
    } ctx = { index, 0, 0, false };

    auto cb = [](uint16_t slot, const cdc::core::TropicStorage::CacheEntry&, void* user) {
        auto* c = static_cast<Ctx*>(user);
        if (c->found) return;
        uint16_t logical = 0;
        if (!TotpStore::instance().toLogicalSlot(slot, &logical)) return;
        if (c->current == c->target) {
            c->slot = logical;
            c->found = true;
            return;
        }
        c->current++;
    };

    cdc::core::TropicStorage::instance().forEachSlot(
        store.moduleId(),
        store.rmemStart(),
        store.rmemEnd(),
        cb, &ctx);

    if (!ctx.found) return false;
    *slotOut = ctx.slot;
    return true;
}

/**
 * \brief Serial command handler printing all configured TOTP accounts.
 * \param args Unused command arguments.
 */
static void cmd_totp_list(const char* args) {
    (void)args;
    if (!TotpStore::instance().hasSlotRange()) {
        cdc::serial::Console::printf("ERROR: slot map not configured\r\n");
        return;
    }
    struct ListCtx {
        uint16_t idx;
    } ctx = {0};
    auto cb = [](uint16_t slot, const cdc::core::TropicStorage::CacheEntry& entry, void* user) {
        auto* c = static_cast<ListCtx*>(user);
        uint16_t logical = 0;
        if (!TotpStore::instance().toLogicalSlot(slot, &logical)) return;
        cdc::serial::Console::printf("%u: %s (slot %u)\r\n", c->idx, entry.name, logical);
        c->idx++;
    };

    cdc::core::TropicStorage::instance().forEachSlot(
        TotpStore::instance().moduleId(),
        TotpStore::instance().rmemStart(),
        TotpStore::instance().rmemEnd(),
        cb, &ctx);

    if (ctx.idx == 0) {
        cdc::serial::Console::printf("(no entries)\r\n");
    }
}

/**
 * \brief Serial command handler adding a TOTP account from tokens.
 * \param args Command arguments (`name secret [issuer] [digits] [period] [algo]`).
 */
static void cmd_totp_add(const char* args) {
    char name[TotpStore::NAME_LEN + 1] = {};
    char secret[128] = {};
    char issuer[TotpStore::ISSUER_LEN + 1] = {};
    char digitsBuf[8] = {};
    char periodBuf[8] = {};
    char algoBuf[8] = {};

    const char* p = nextToken(args, name, sizeof(name));
    if (!p || !*name) {
        cdc::serial::Console::printf("Usage: TOTP ADD <name> <secret> [issuer] [digits] [period] [algo]\r\n");
        return;
    }
    p = nextToken(p, secret, sizeof(secret));
    if (!p || !*secret) {
        cdc::serial::Console::printf("Usage: TOTP ADD <name> <secret> [issuer] [digits] [period] [algo]\r\n");
        return;
    }
    p = nextToken(p, issuer, sizeof(issuer));
    p = nextToken(p, digitsBuf, sizeof(digitsBuf));
    p = nextToken(p, periodBuf, sizeof(periodBuf));
    p = nextToken(p, algoBuf, sizeof(algoBuf));

    uint8_t digits = digitsBuf[0] ? static_cast<uint8_t>(atoi(digitsBuf)) : TotpStore::DEFAULT_DIGITS;
    uint32_t period = periodBuf[0] ? static_cast<uint32_t>(atoi(periodBuf)) : TotpStore::DEFAULT_PERIOD;
    uint8_t algo = parseAlgo(algoBuf);

    bool ok = TotpStore::instance().addAccount(
        name,
        issuer[0] ? issuer : nullptr,
        secret,
        digits,
        period,
        algo
    );
    cdc::serial::Console::printf(ok ? "OK\r\n" : "ERROR\r\n");
}

/**
 * \brief Serial command handler deleting a TOTP account by index.
 * \param args Command arguments (`<index>`).
 */
static void cmd_totp_del(const char* args) {
    if (!args || !*args) {
        cdc::serial::Console::printf("Usage: TOTP DEL <index>\r\n");
        return;
    }
    uint16_t index = static_cast<uint16_t>(atoi(args));
    uint16_t slot = 0;
    if (!findSlotByIndex(index, &slot)) {
        cdc::serial::Console::printf("ERROR: index not found\r\n");
        return;
    }
    bool ok = TotpStore::instance().deleteAccount(slot);
    cdc::serial::Console::printf(ok ? "OK\r\n" : "ERROR\r\n");
}

/**
 * \brief Serial command handler generating one TOTP code by index.
 * \param args Command arguments (`<index>`).
 */
static void cmd_totp_get(const char* args) {
    if (!args || !*args) {
        cdc::serial::Console::printf("Usage: TOTP GET <index>\r\n");
        return;
    }
    uint16_t index = static_cast<uint16_t>(atoi(args));
    uint16_t slot = 0;
    if (!findSlotByIndex(index, &slot)) {
        cdc::serial::Console::printf("ERROR: index not found\r\n");
        return;
    }
    char code[9] = {};
    int8_t remaining = TotpStore::instance().generateCode(slot, code, sizeof(code));
    if (remaining < 0) {
        cdc::serial::Console::printf("ERROR: time not valid\r\n");
        return;
    }
    TotpAccount account = {};
    const char* issuer = "";
    if (TotpStore::instance().readAccount(slot, &account)) {
        issuer = account.issuer;
    }
    if (issuer && issuer[0]) {
        cdc::serial::Console::printf("%s (%ds) [%s]\r\n", code, remaining, issuer);
    } else {
        cdc::serial::Console::printf("%s (%ds)\r\n", code, remaining);
    }
}

/**
 * \brief Sub-command table for the TOTP serial command group.
 */
static const cdc::serial::SubCommand kTotpSubs[] = {
    {"LIST", "",                                                "List all TOTP accounts",      cmd_totp_list},
    {"ADD",  "<name> <secret> [issuer] [digits] [period] [algo]","Add TOTP account",            cmd_totp_add},
    {"DEL",  "<index>",                                         "Delete TOTP account by index", cmd_totp_del},
    {"GET",  "<index>",                                         "Generate TOTP code by index", cmd_totp_get},
    {nullptr, nullptr, nullptr, nullptr},
};

static void cmd_totp(const char* args) {
    cdc::serial::dispatchSubCommand("TOTP", args, kTotpSubs);
}

/**
 * \brief Registers serial commands exposed by the TOTP module.
 */
static void registerCommands() {
    if (s_commandsRegistered) return;
    auto& reg = cdc::serial::getCommandRegistry();
    reg.registerCommand({"TOTP",
                         "TOTP authenticator: LIST/ADD/DEL/GET",
                         cmd_totp, CMD_MODULE, true, kTotpSubs});
    s_commandsRegistered = true;
}

/** \brief TOTP code detail view implementation. */

/** \brief Forward declaration for edit wizard entry point. */
static void wizardEdit(uint16_t slot);

class TotpCodeView : public ui::ViewBase {
public:
    /**
     * \brief Initializes the code view for a specific account slot.
     * \param slot Logical TOTP slot.
     * \param name Account display name.
     */
    void init(uint16_t slot, const char* name) {
        slot_ = slot;
        strncpy(name_, name ? name : "", sizeof(name_) - 1);
        name_[sizeof(name_) - 1] = '\0';
        issuer_[0] = '\0';
        updateCode();
    }

    /**
     * \brief Refreshes code state when the view is entered.
     * \param context Optional enter context (unused).
     */
    void onEnter(void* context) override {
        (void)context;
        updateCode();
        if (!timeValid_) {
            ui::showToastError(ui::tr("mod_totp.time_invalid"));
        }
        dirty_ = true;
    }

    /**
     * \brief Refreshes code state when the view resumes.
     */
    void onResume() override {
        updateCode();
        if (!timeValid_) {
            ui::showToastError(ui::tr("mod_totp.time_invalid"));
        }
        dirty_ = true;
    }

    /**
     * \brief Updates countdown and code display once per second.
     * \param nowMs Current uptime in milliseconds.
     */
    void onTick(uint32_t nowMs) override {
        if (nowMs - lastUpdateMs_ >= 1000) {
            lastUpdateMs_ = nowMs;
            updateCode();
            markDirty();
        }
    }

    /**
     * \brief Renders account metadata, TOTP code, and validity/progress UI.
     * \param partial `true` for partial redraw, `false` for full redraw.
     */
    void render(bool partial) override {
        auto* display = hal::getDisplayInstance();
        if (!display) return;

        auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
        if (!gfx) return;

        if (!partial) {
            gfx->fillScreen(EPD_WHITE);
        }

        gfx->setTextColor(EPD_BLACK);
        gfx->setTextSize(1);
        gfx->setCursor(8, 6);
        gfx->print(ui::tr("mod_totp.code"));
        gfx->drawFastHLine(0, 22, display->getWidth(), EPD_BLACK);

        gfx->setCursor(8, 28);
        gfx->print(name_);
        if (issuer_[0]) {
            gfx->setCursor(8, 40);
            gfx->print(issuer_);
        }

        if (!timeValid_) {
            gfx->setTextSize(1);
            gfx->setCursor(8, 60);
            gfx->print(ui::tr("mod_totp.time_invalid"));
            clearDirty();
            return;
        }

        // Large centered code
        gfx->setTextSize(2);
        int16_t x1, y1;
        uint16_t w, h;
        gfx->getTextBounds(code_, 0, 0, &x1, &y1, &w, &h);
        int16_t codeX = (display->getWidth() - w) / 2;
        gfx->setCursor(codeX, 58);
        gfx->print(code_);

        // Progress bar + remaining counter
        gfx->setTextSize(1);
        const int barX = 20;
        const int barY = 92;
        const int barW = display->getWidth() - 40;
        const int barH = 8;
        gfx->drawRect(barX, barY, barW, barH, EPD_BLACK);
        uint32_t period = (period_ == 0) ? 1 : period_;
        uint32_t rem = (remaining_ > period) ? period : remaining_;
        uint16_t fillW = static_cast<uint16_t>((barW - 2) * rem / period);
        gfx->fillRect(barX + 1, barY + 1, fillW, barH - 2, EPD_BLACK);

        gfx->setCursor(barX, barY + 14);
        display->printf("%us", static_cast<unsigned>(remaining_));

        clearDirty();
    }

    /**
     * \brief Handles key actions for back, edit, and keyboard typing.
     * \param key Pressed key code.
     * \return Input handling result for the view stack.
     */
    ui::InputResult onKey(char key) override {
        if (key == 'N') {
            return ui::InputResult::REQUEST_POP;
        }
        if (key == '3') {
            wizardEdit(slot_);
            return ui::InputResult::CONSUMED;
        }
        if (key == 'Y') {
            auto* kb = core::getKeyboard();
            if (kb && kb->isConnected()) {
                if (timeValid_ && code_[0] != '-') {
                    kb->typeString(code_);
                    ui::showToastSuccess("Typed");
                }
            } else {
                ui::showToastError(ui::tr("mod_totp.no_keyboard"));
            }
            return ui::InputResult::CONSUMED;
        }
        return ui::InputResult::IGNORED;
    }

    /**
     * \brief Returns the static view identifier.
     * \return View name string.
     */
    const char* getName() const override { return "TotpCodeView"; }

    /**
     * \brief Returns context-aware footer hint text.
     * \return Footer hint string.
     */
    const char* getFooterHint() const override {
        auto* kb = core::getKeyboard();
        if (kb && kb->isConnected()) {
            return ui::tr("mod_totp.hint_type");
        }
        return ui::tr("mod_totp.hint_edit");
    }

private:
    /**
     * \brief Recomputes the current code, issuer label, and remaining seconds.
     */
    void updateCode() {
        TotpStore& store = TotpStore::instance();
        timeValid_ = store.isTimeValid();
        if (!timeValid_) {
            strncpy(code_, "------", sizeof(code_) - 1);
            remaining_ = 0;
            period_ = 0;
            return;
        }

        TotpAccount account = {};
        if (!store.readAccount(slot_, &account)) {
            timeValid_ = false;
            strncpy(code_, "------", sizeof(code_) - 1);
            remaining_ = 0;
            period_ = 0;
            return;
        }
        strncpy(issuer_, account.issuer, sizeof(issuer_) - 1);
        issuer_[sizeof(issuer_) - 1] = '\0';
        period_ = account.period;
        int8_t rem = store.generateCode(slot_, code_, sizeof(code_));
        if (rem >= 0) {
            remaining_ = static_cast<uint8_t>(rem);
        } else {
            timeValid_ = false;
            strncpy(code_, "------", sizeof(code_) - 1);
            remaining_ = 0;
            period_ = 0;
        }
    }

    uint16_t slot_ = 0;
    char name_[TotpStore::NAME_LEN + 1] = {};
    char issuer_[TotpStore::ISSUER_LEN + 1] = {};
    char code_[9] = {};
    uint8_t remaining_ = 0;
    uint32_t period_ = 0;
    bool timeValid_ = false;
    uint32_t lastUpdateMs_ = 0;
};

/** \brief TOTP module UI state. */

/** \brief Static view instances (no dynamic allocation, no leaks). */
static ui::ListView s_listView;
static ui::T9InputView s_t9Input;
static ui::ListView s_digitsMenu;
static ui::ListView s_algoMenu;
static ui::ListView s_periodMenu;
static TotpCodeView s_codeView;
static bool s_viewsInitialized = false;

/** \brief Dynamic list buffers released by `freeListBuffers`. */
static ui::ListItem* s_listItems = nullptr;
static char (*s_listLabels)[24] = nullptr;
static uint16_t* s_listSlots = nullptr;
static uint16_t s_accountCount = 0;
static uint16_t s_capacity = 0;

struct WizardState {
    char name[TotpStore::NAME_LEN + 1];
    char secret[128];
    char issuer[TotpStore::ISSUER_LEN + 1];
    uint8_t digits;
    uint8_t algorithm;
    uint32_t period;
    bool editMode;
    uint16_t editSlot;
};

EXT_RAM_BSS_ATTR static WizardState s_wizard = {};

/** \brief Pushes T9 input view for current wizard step. */
/**
 * \brief Pushes a configured T9 input step for the account wizard flow.
 * \param title Step title.
 * \param initialText Initial input text.
 * \param maxLen Maximum accepted text length.
 * \param onSave Save callback for the step.
 */
static void pushT9WizardStep(const char* title, const char* initialText,
                              uint16_t maxLen, ui::T9InputView::SaveCallback onSave) {
    s_t9Input.init(title, initialText, maxLen);
    s_t9Input.setOnSave(onSave);
    ui::ViewStack::instance().push(&s_t9Input);
}

/**
 * \brief Frees dynamically allocated list buffers used by the TOTP account list.
 * \return void
 */
static void freeListBuffers() {
    delete[] s_listItems;
    delete[] s_listLabels;
    delete[] s_listSlots;
    s_listItems = nullptr;
    s_listLabels = nullptr;
    s_listSlots = nullptr;
    s_capacity = 0;
    s_accountCount = 0;
}

static void rebuildList();
static void onListSelect(uint16_t index, void* userData);
static void wizardStart();
static void wizardEdit(uint16_t slot);
static void onWizardName(const char* text);
static void onWizardSecret(const char* text);
static void onWizardIssuer(const char* text);
static void onWizardDigits(uint16_t index, void* userData);
static void onWizardAlgo(uint16_t index, void* userData);
static void onWizardPeriod(uint16_t index, void* userData);
static void wizardFinish();

/**
 * \brief Encodes binary secret bytes into unpadded Base32 text.
 * \param data Input binary payload.
 * \param dataLen Input length in bytes.
 * \param out Output Base32 buffer.
 * \param outMax Output buffer size.
 */
static void base32Encode(const uint8_t* data, size_t dataLen, char* out, size_t outMax) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    if (!out || outMax == 0) return;
    size_t outPos = 0;
    uint32_t buffer = 0;
    uint8_t bitsLeft = 0;

    for (size_t i = 0; i < dataLen; i++) {
        buffer = (buffer << 8) | data[i];
        bitsLeft += 8;
        while (bitsLeft >= 5) {
            uint8_t index = (buffer >> (bitsLeft - 5)) & 0x1F;
            bitsLeft -= 5;
            if (outPos + 1 >= outMax) {
                out[outPos] = '\0';
                return;
            }
            out[outPos++] = alphabet[index];
        }
    }

    if (bitsLeft > 0) {
        uint8_t index = (buffer << (5 - bitsLeft)) & 0x1F;
        if (outPos + 1 < outMax) {
            out[outPos++] = alphabet[index];
        }
    }
    out[outPos] = '\0';
}

/**
 * \brief Ensures account list backing buffers are allocated for current capacity.
 * \return `true` if buffers are ready for use.
 */
static bool ensureListBuffers() {
    uint16_t cap = TotpStore::instance().capacity();
    if (cap == 0) return false;
    if (cap == s_capacity && s_listItems && s_listLabels && s_listSlots) return true;

    delete[] s_listItems;
    delete[] s_listLabels;
    delete[] s_listSlots;
    s_listItems = nullptr;
    s_listLabels = nullptr;
    s_listSlots = nullptr;
    s_capacity = 0;

    s_listItems = new (std::nothrow) ui::ListItem[cap + 1];
    s_listLabels = new (std::nothrow) char[cap][24];
    s_listSlots = new (std::nothrow) uint16_t[cap];
    if (!s_listItems || !s_listLabels || !s_listSlots) {
        delete[] s_listItems;
        delete[] s_listLabels;
        delete[] s_listSlots;
        s_listItems = nullptr;
        s_listLabels = nullptr;
        s_listSlots = nullptr;
        s_capacity = 0;
        return false;
    }
    s_capacity = cap;
    return true;
}

/**
 * \brief Rebuilds the TOTP list view content from Tropic storage cache.
 */
static void rebuildList() {
    if (!ensureListBuffers()) {
        cdc::core::ModuleRegistry::instance().reportModuleError(TotpModule::instance().getName(),
                                                               "TOTP list allocation failed");
        return;
    }
    s_accountCount = 0;
    s_listItems[0] = {ui::tr("mod_totp.add_account"), 0, false, nullptr};

    auto cb = [](uint16_t slot, const cdc::core::TropicStorage::CacheEntry& entry, void* user) {
        (void)user;
        if (s_accountCount >= s_capacity) return;
        uint16_t logical = 0;
        if (!TotpStore::instance().toLogicalSlot(slot, &logical)) return;
        strncpy(s_listLabels[s_accountCount], entry.name, sizeof(s_listLabels[s_accountCount]) - 1);
        s_listLabels[s_accountCount][sizeof(s_listLabels[s_accountCount]) - 1] = '\0';
        s_listSlots[s_accountCount] = logical;
        uint16_t idx = static_cast<uint16_t>(s_accountCount + 1);
        s_listItems[idx].label = s_listLabels[s_accountCount];
        s_listItems[idx].icon = 0;
        s_listItems[idx].iconDisabled = false;
        s_listItems[idx].userData = reinterpret_cast<void*>(static_cast<uintptr_t>(logical));
        s_accountCount++;
    };

    cdc::core::TropicStorage::instance().forEachSlot(
        TotpStore::instance().moduleId(),
        TotpStore::instance().rmemStart(),
        TotpStore::instance().rmemEnd(),
        cb, nullptr);

    s_listView.init(ui::tr("mod_totp.title"), s_listItems, static_cast<uint16_t>(s_accountCount + 1));
}

/**
 * \brief Handles selection from the TOTP account list.
 * \param index Selected row index.
 * \param userData Optional user pointer (unused).
 */
static void onListSelect(uint16_t index, void* userData) {
    (void)userData;
    if (index == 0) {
        wizardStart();
        return;
    }
    if (index - 1 >= s_accountCount) return;

    uint16_t slot = s_listSlots[index - 1];
    const char* name = s_listLabels[index - 1];
    s_codeView.init(slot, name);
    ui::ViewStack::instance().push(&s_codeView);
}

/**
 * \brief Starts the add-account wizard with default values.
 */
static void wizardStart() {
    memset(&s_wizard, 0, sizeof(s_wizard));
    s_wizard.digits = TotpStore::DEFAULT_DIGITS;
    s_wizard.algorithm = static_cast<uint8_t>(TotpAlgorithm::SHA1);
    s_wizard.period = TotpStore::DEFAULT_PERIOD;
    s_wizard.editMode = false;
    s_wizard.editSlot = 0;

    pushT9WizardStep(ui::tr("mod_totp.account_name"), nullptr, TotpStore::NAME_LEN, onWizardName);
}

/**
 * \brief Starts edit wizard prefilled with an existing account.
 * \param slot Logical slot to edit.
 */
static void wizardEdit(uint16_t slot) {
    TotpAccount account = {};
    if (!TotpStore::instance().readAccount(slot, &account)) {
        ui::showToastError(ui::tr("core.failed"));
        return;
    }

    memset(&s_wizard, 0, sizeof(s_wizard));
    strncpy(s_wizard.name, account.name, sizeof(s_wizard.name) - 1);
    strncpy(s_wizard.issuer, account.issuer, sizeof(s_wizard.issuer) - 1);
    s_wizard.digits = account.digits;
    s_wizard.algorithm = account.algorithm;
    s_wizard.period = account.period;
    s_wizard.editMode = true;
    s_wizard.editSlot = slot;
    base32Encode(account.secret, account.secretLen, s_wizard.secret, sizeof(s_wizard.secret));

    pushT9WizardStep(ui::tr("mod_totp.account_name"), s_wizard.name, TotpStore::NAME_LEN, onWizardName);
}

/**
 * \brief Saves wizard account name and opens secret step.
 * \param text Entered account name.
 */
static void onWizardName(const char* text) {
    strncpy(s_wizard.name, text ? text : "", sizeof(s_wizard.name) - 1);
    pushT9WizardStep(ui::tr("mod_totp.secret"), s_wizard.secret, 64, onWizardSecret);
}

/**
 * \brief Saves wizard secret and opens issuer step.
 * \param text Entered Base32 secret.
 */
static void onWizardSecret(const char* text) {
    strncpy(s_wizard.secret, text ? text : "", sizeof(s_wizard.secret) - 1);
    pushT9WizardStep(ui::tr("mod_totp.issuer"), s_wizard.issuer, TotpStore::ISSUER_LEN, onWizardIssuer);
}

/**
 * \brief Saves wizard issuer and opens digit-selection step.
 * \param text Entered issuer string.
 */
static void onWizardIssuer(const char* text) {
    strncpy(s_wizard.issuer, text ? text : "", sizeof(s_wizard.issuer) - 1);

    static ui::ListItem digitsItems[3] = {
        {"6", 0, false, nullptr},
        {"7", 0, false, nullptr},
        {"8", 0, false, nullptr}
    };
    s_digitsMenu.setOnSelect(onWizardDigits);
    s_digitsMenu.init(ui::tr("mod_totp.digits"), digitsItems, 3);
    ui::ViewStack::instance().push(&s_digitsMenu);
}

/**
 * \brief Saves selected code length and opens algorithm-selection step.
 * \param index Selected list index.
 * \param userData Optional user pointer (unused).
 */
static void onWizardDigits(uint16_t index, void* userData) {
    (void)userData;
    static const uint8_t digitMap[3] = {6, 7, 8};
    s_wizard.digits = digitMap[index % 3];

    static ui::ListItem algoItems[3] = {
        {"SHA1", 0, false, nullptr},
        {"SHA256", 0, false, nullptr},
        {"SHA512", 0, false, nullptr}
    };
    s_algoMenu.setOnSelect(onWizardAlgo);
    s_algoMenu.init(ui::tr("mod_totp.algorithm"), algoItems, 3);
    ui::ViewStack::instance().push(&s_algoMenu);
}

/**
 * \brief Saves selected algorithm and opens period-selection step.
 * \param index Selected list index.
 * \param userData Optional user pointer (unused).
 */
static void onWizardAlgo(uint16_t index, void* userData) {
    (void)userData;
    s_wizard.algorithm = static_cast<uint8_t>(index % 3);

    static ui::ListItem periodItems[2] = {
        {"30s", 0, false, nullptr},
        {"60s", 0, false, nullptr}
    };
    s_periodMenu.setOnSelect(onWizardPeriod);
    s_periodMenu.init(ui::tr("mod_totp.period"), periodItems, 2);
    ui::ViewStack::instance().push(&s_periodMenu);
}

/**
 * \brief Saves selected period and finalizes add/edit operation.
 * \param index Selected list index.
 * \param userData Optional user pointer (unused).
 */
static void onWizardPeriod(uint16_t index, void* userData) {
    (void)userData;
    s_wizard.period = (index == 0) ? 30 : 60;
    wizardFinish();
}

/**
 * \brief Validates wizard data and persists account changes.
 */
static void wizardFinish() {
    if (strlen(s_wizard.name) == 0 || strlen(s_wizard.secret) == 0) {
        ui::showToastError(ui::tr("mod_totp.invalid_input"));
        ui::ViewStack::instance().popToAnchor(&s_listView);
        return;
    }

    bool ok = false;
    if (s_wizard.editMode) {
        ok = TotpStore::instance().updateAccount(
            s_wizard.editSlot,
            s_wizard.name,
            strlen(s_wizard.issuer) > 0 ? s_wizard.issuer : nullptr,
            s_wizard.secret,
            s_wizard.digits,
            s_wizard.period,
            s_wizard.algorithm
        );
    } else {
        ok = TotpStore::instance().addAccount(
            s_wizard.name,
            strlen(s_wizard.issuer) > 0 ? s_wizard.issuer : nullptr,
            s_wizard.secret,
            s_wizard.digits,
            s_wizard.period,
            s_wizard.algorithm
        );
    }

    if (ok) {
        ui::showToastSuccess(ui::tr("core.ok"));
        rebuildList();
        ui::ViewStack::instance().popToAnchor(&s_listView);
    } else {
        ui::showToastError(ui::tr("core.failed"));
    }
}

/**
 * \brief Returns singleton TOTP module instance.
 * \return Module singleton reference.
 */
TotpModule& TotpModule::instance() {
    static TotpModule inst;
    return inst;
}

/**
 * \brief Initializes module resources, translations, commands, and slot mapping.
 * \return `true` if module initialization succeeded.
 */
bool TotpModule::init() {
    LOG_I(TAG, "Initializing TOTP module");
    registerStrings();
    registerCommands();

    core::ModuleRegistry::instance().registerModule(this);
    if (slotRange_.hasRmem) {
        TotpStore::instance().setSlotRange(slotRange_);
        core::ModuleRegistry::instance().clearModuleErrorByName(getName());
    } else {
        core::ModuleRegistry::instance().reportModuleError(getName(), "TOTP slot range missing");
        state_ = core::ServiceState::ERROR;
        return false;
    }
    state_ = core::ServiceState::INITIALIZED;
    return true;
}

/**
 * \brief Stops the TOTP module and releases list buffers.
 */
void TotpModule::stop() {
    freeListBuffers();
    ModuleBase::stop();
}

/**
 * \brief Stores assigned Tropic slot range for the module.
 * \param range Slot assignment from module registry.
 */
void TotpModule::setSlotRange(const core::IModule::SlotRange& range) {
    slotRange_ = range;
}

/**
 * \brief Declares minimum slot requirements for the TOTP module.
 * \return Slot request structure for registry planning.
 */
core::IModule::SlotRequest TotpModule::getSlotRequest() const {
    core::IModule::SlotRequest req = {};
    req.mapName = getName();
    req.minRmemSlots = 1;
    return req;
}

/**
 * \brief Provides main-menu entry for the TOTP module.
 * \param items Output array for menu items.
 * \param maxItems Maximum number of writable entries in `items`.
 * \return Number of populated menu items.
 */
uint8_t TotpModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    items[0] = {ui::tr("mod_totp.title"), 50, []() -> ui::IView* {
        if (!s_viewsInitialized) {
            s_listView.setOnSelect(onListSelect);
            s_viewsInitialized = true;
        }
        rebuildList();
        return &s_listView;
    }, nullptr, getName(), core::MenuLocation::MAIN_MENU, nullptr};

    return 1;
}

} // namespace cdc::mod_totp

/**
 * \brief Registers TOTP module initializer in the global module registry.
 */
extern "C" void mod_totp_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_totp::TotpModule::instance();
        if (module.init()) {
            module.start();
        }
    });
}
