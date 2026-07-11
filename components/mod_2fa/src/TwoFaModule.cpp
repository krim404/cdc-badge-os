#include "mod_2fa/TwoFaModule.h"
#include "mod_2fa/OathStore.h"
#include "mod_2fa/ble_chalresp.h"
#include "OathApplet.h"
#include "cdc_scard/applet.h"
#include "cdc_scard/scard_usb.h"
#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/ServiceRegistry.h"
#include "cdc_core/StringUtils.h"
#include "cdc_core/TropicStorage.h"
#include "cdc_ui/BackupImport.h"
#include "cdc_core/IKeyboardProvider.h"
#include "cdc_ui/I18n.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_views/ListView.h"
#include "cdc_views/T9InputView.h"
#include "cdc_views/ToastView.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_hal/IDisplay.h"
#include "serial_cmd/ICommandRegistry.h"
#include "serial_cmd/SubCommand.h"
#include "serial_cmd/Console.h"
#include "cdc_log.h"
#include "esp_attr.h"
#include "cJSON.h"
#include <goodisplay/gdey029T94.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <new>

static const char* TAG = "2FA";

namespace cdc::mod_2fa {
// Defined in OathBackend.cpp: wires the YKOATH applet to OathStore.
void oath_backend_install();
}

namespace cdc::mod_2fa {

constexpr ui::I18nEntry kStrings[] = {
    {"mod_2fa.title",         "2FA"},
    {"mod_2fa.add_account",   "Add Account"},
    {"mod_2fa.account_name",  "Account Name"},
    {"mod_2fa.secret",        "Secret (Base32)"},
    {"mod_2fa.issuer",        "Issuer (optional)"},
    {"mod_2fa.digits",        "Digits"},
    {"mod_2fa.algorithm",     "Algorithm"},
    {"mod_2fa.period",        "Period"},
    {"mod_2fa.code",          "Code"},
    {"mod_2fa.time_invalid",  "Time not set"},
    {"mod_2fa.invalid_input", "Invalid input"},
    {"mod_2fa.hint_edit",     "[3] Edit  [N] Back"},
    {"mod_2fa.hint_type",     "[Y] Type  [3] Edit  [N] Back"},
    {"mod_2fa.hint_hotp",     "[5] Next  [Y] Type  [3] Edit  [N] Back"},
    {"mod_2fa.no_keyboard",   "No keyboard connected"},
    {"mod_2fa.type",          "Type"},
    {"mod_2fa.counter",       "Counter"},
    {"mod_2fa.touch",         "Touch confirm"},
    {"mod_2fa.touch_on",      "Required"},
    {"mod_2fa.touch_off",     "Not required"},
    {"mod_2fa.cr_confirm",    "Allow challenge-response?"},
    {"mod_2fa.cr_entry",      "Challenge-Response"},
    {"mod_2fa.usb_cr",        "USB slot 2"},
    {"mod_2fa.usb_cr_on",     "Designate"},
    {"mod_2fa.usb_cr_off",    "No"},
};

static void registerStrings() {
    ui::I18n::instance().registerEnglishTable(kStrings, std::size(kStrings));
}

/** \brief Serial command handlers for 2FA module. */

static constexpr const char* CMD_MODULE = "totp";
static constexpr size_t SECRET_B32_LEN = 128;
static bool s_commandsRegistered = false;

using cdc::core::skipSpaces;
using cdc::core::nextToken;

/**
 * \brief Parses textual or numeric algorithm identifiers into store values.
 * \param token Algorithm token (`sha1`, `sha256`, `sha512`, or numeric).
 * \return Encoded algorithm value used by `OathStore`.
 */
static uint8_t parseAlgo(const char* token) {
    if (!token || !*token) return static_cast<uint8_t>(OathAlgorithm::SHA1);
    char buf[8] = {};
    size_t i = 0;
    for (; token[i] && i + 1 < sizeof(buf); i++) {
        buf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(token[i])));
    }
    buf[i] = '\0';
    if (strcmp(buf, "sha1") == 0) return static_cast<uint8_t>(OathAlgorithm::SHA1);
    if (strcmp(buf, "sha256") == 0) return static_cast<uint8_t>(OathAlgorithm::SHA256);
    if (strcmp(buf, "sha512") == 0) return static_cast<uint8_t>(OathAlgorithm::SHA512);
    // Numeric fallback with bounds check: only accept supported algorithm IDs.
    int value = atoi(buf);
    if (value < 0 || value > static_cast<int>(OathAlgorithm::SHA512)) {
        return static_cast<uint8_t>(OathAlgorithm::SHA1);
    }
    return static_cast<uint8_t>(value);
}

/**
 * \brief Parses a textual or numeric entry-type token.
 * \param token Type token (`totp`, `hotp`, or numeric).
 * \return Encoded `OathType` value (defaults to TOTP).
 */
static uint8_t parseType(const char* token) {
    if (!token || !*token) return static_cast<uint8_t>(OathType::TOTP);
    char buf[8] = {};
    size_t i = 0;
    for (; token[i] && i + 1 < sizeof(buf); i++) {
        buf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(token[i])));
    }
    buf[i] = '\0';
    if (strcmp(buf, "totp") == 0) return static_cast<uint8_t>(OathType::TOTP);
    if (strcmp(buf, "hotp") == 0) return static_cast<uint8_t>(OathType::HOTP);
    if (strcmp(buf, "cr") == 0) return static_cast<uint8_t>(OathType::CR);
    int value = atoi(buf);
    if (value == static_cast<int>(OathType::HOTP)) return static_cast<uint8_t>(OathType::HOTP);
    if (value == static_cast<int>(OathType::CR)) return static_cast<uint8_t>(OathType::CR);
    return static_cast<uint8_t>(OathType::TOTP);
}

/**
 * \brief Resolves a displayed list index to the logical OATH slot number.
 * \param index UI list index.
 * \param slotOut Output logical slot.
 * \return `true` if a matching slot was found.
 */
static bool findSlotByIndex(uint16_t index, uint16_t* slotOut) {
    if (!slotOut) return false;
    auto& store = OathStore::instance();
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
        if (!OathStore::instance().toLogicalSlot(slot, &logical)) return;
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
 * \brief Serial command handler printing all configured OATH entries.
 * \param args Unused command arguments.
 */
static void cmd_totp_list(const char* args) {
    (void)args;
    if (!OathStore::instance().hasSlotRange()) {
        cdc::serial::Console::printf("ERROR: slot map not configured\r\n");
        return;
    }
    struct ListCtx {
        uint16_t idx;
    } ctx = {0};
    auto cb = [](uint16_t slot, const cdc::core::TropicStorage::CacheEntry& entry, void* user) {
        auto* c = static_cast<ListCtx*>(user);
        uint16_t logical = 0;
        if (!OathStore::instance().toLogicalSlot(slot, &logical)) return;
        OathEntry account = {};
        const char* typeStr = "?";
        if (OathStore::instance().readAccount(logical, &account)) {
            switch (static_cast<OathType>(account.type)) {
                case OathType::HOTP: typeStr = "HOTP"; break;
                case OathType::CR:   typeStr = "CR";   break;
                default:             typeStr = "TOTP"; break;
            }
        }
        cdc::serial::Console::printf("%u: %s [%s] (slot %u)\r\n", c->idx, entry.name, typeStr, logical);
        c->idx++;
    };

    cdc::core::TropicStorage::instance().forEachSlot(
        OathStore::instance().moduleId(),
        OathStore::instance().rmemStart(),
        OathStore::instance().rmemEnd(),
        cb, &ctx);

    if (ctx.idx == 0) {
        cdc::serial::Console::printf("(no entries)\r\n");
    }
}

/**
 * \brief Serial command handler adding an OATH entry from tokens.
 * \param args Command arguments
 *        (`<type> <name> <secret> [issuer] [digits] [period] [algo] [counter]`).
 */
static void cmd_totp_add(const char* args) {
    static const char* usage =
        "Usage: TOTP ADD <type:totp|hotp> <name> <secret> [issuer] [digits] [period] [algo] [counter]\r\n";
    char typeBuf[8] = {};
    char name[OathStore::NAME_LEN + 1] = {};
    char secret[SECRET_B32_LEN] = {};
    char issuer[OathStore::ISSUER_LEN + 1] = {};
    char digitsBuf[8] = {};
    char periodBuf[8] = {};
    char algoBuf[8] = {};
    char counterBuf[24] = {};

    const char* p = nextToken(args, typeBuf, sizeof(typeBuf));
    if (!p || !*typeBuf) {
        cdc::serial::Console::printf("%s", usage);
        return;
    }
    p = nextToken(p, name, sizeof(name));
    if (!p || !*name) {
        cdc::serial::Console::printf("%s", usage);
        return;
    }
    p = nextToken(p, secret, sizeof(secret));
    if (!p || !*secret) {
        cdc::serial::Console::printf("%s", usage);
        return;
    }
    p = nextToken(p, issuer, sizeof(issuer));
    p = nextToken(p, digitsBuf, sizeof(digitsBuf));
    p = nextToken(p, periodBuf, sizeof(periodBuf));
    p = nextToken(p, algoBuf, sizeof(algoBuf));
    p = nextToken(p, counterBuf, sizeof(counterBuf));

    uint8_t type = parseType(typeBuf);
    uint8_t digits = digitsBuf[0] ? static_cast<uint8_t>(atoi(digitsBuf)) : OathStore::DEFAULT_DIGITS;
    uint32_t period = periodBuf[0] ? static_cast<uint32_t>(atoi(periodBuf)) : OathStore::DEFAULT_PERIOD;
    uint8_t algo = parseAlgo(algoBuf);
    uint64_t counter = counterBuf[0] ? strtoull(counterBuf, nullptr, 10) : 0;

    bool ok = OathStore::instance().addAccount(
        type,
        name,
        issuer[0] ? issuer : nullptr,
        secret,
        digits,
        period,
        algo,
        counter
    );
    cdc::serial::Console::printf(ok ? "OK\r\n" : "ERROR\r\n");
}

/**
 * \brief Serial command handler deleting an OATH entry by index.
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
    bool ok = OathStore::instance().deleteAccount(slot);
    cdc::serial::Console::printf(ok ? "OK\r\n" : "ERROR\r\n");
}

/**
 * \brief Serial command handler generating one code by index.
 *
 * For HOTP entries this advances and persists the moving counter.
 *
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
    OathEntry account = {};
    if (!OathStore::instance().readAccount(slot, &account)) {
        cdc::serial::Console::printf("ERROR: read failed\r\n");
        return;
    }
    char code[9] = {};
    int8_t remaining = OathStore::instance().generateCode(slot, code, sizeof(code));
    if (remaining < 0) {
        cdc::serial::Console::printf("ERROR: time not valid\r\n");
        return;
    }
    const char* issuer = account.issuer;
    if (account.type == static_cast<uint8_t>(OathType::HOTP)) {
        // account.counter is the pre-increment value (read before generateCode ran);
        // it is the moving factor that produced the displayed code.
        if (issuer && issuer[0]) {
            cdc::serial::Console::printf("%s (counter %llu) [%s]\r\n", code,
                                         static_cast<unsigned long long>(account.counter), issuer);
        } else {
            cdc::serial::Console::printf("%s (counter %llu)\r\n", code,
                                         static_cast<unsigned long long>(account.counter));
        }
        return;
    }
    if (issuer && issuer[0]) {
        cdc::serial::Console::printf("%s (%ds) [%s]\r\n", code, remaining, issuer);
    } else {
        cdc::serial::Console::printf("%s (%ds)\r\n", code, remaining);
    }
}

/**
 * \brief Decodes a hex string into bytes.
 * \param hex Null-terminated hex text (even number of nibbles, no separators).
 * \param out Output byte buffer.
 * \param outMax Output capacity.
 * \return Number of decoded bytes, or `-1` on malformed input or overflow.
 */
static int hexDecode(const char* hex, uint8_t* out, size_t outMax) {
    if (!hex || !out) return -1;
    size_t len = strlen(hex);
    if (len == 0 || (len % 2) != 0) return -1;
    size_t count = len / 2;
    if (count > outMax) return -1;

    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < count; i++) {
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return static_cast<int>(count);
}

/**
 * \brief Serial command computing a raw challenge-response for a named CR entry.
 *
 * Looks up the CR entry by name, computes `HMAC(secret, challenge)` using the
 * entry's algorithm (SHA1 or SHA256), and prints the hex response. The serial
 * path is trusted (AUTH-gated) and does not require a touch confirmation.
 *
 * \param args Command arguments (`<name> <hex-challenge>`).
 */
static void cmd_chalresp(const char* args) {
    char name[OathStore::NAME_LEN + 1] = {};
    char hexBuf[2 * 128 + 2] = {};

    const char* p = nextToken(args, name, sizeof(name));
    if (!p || !*name) {
        cdc::serial::Console::printf("Usage: CHALRESP <name> <hex-challenge>\r\n");
        return;
    }
    p = nextToken(p, hexBuf, sizeof(hexBuf));
    if (!*hexBuf) {
        cdc::serial::Console::printf("Usage: CHALRESP <name> <hex-challenge>\r\n");
        return;
    }
    // nextToken stops at the buffer limit but keeps consuming the rest of the
    // token, silently dropping it. The buffer holds one slot beyond the 256 hex
    // chars of a maximum 128-byte challenge, so a longer token is detectable and
    // rejected instead of computing over a truncated input.
    if (strlen(hexBuf) > 2 * 128) {
        cdc::serial::Console::printf("ERROR: challenge too long (max 128 bytes)\r\n");
        return;
    }

    uint8_t challenge[128] = {};
    int clen = hexDecode(hexBuf, challenge, sizeof(challenge));
    if (clen < 0) {
        cdc::serial::Console::printf("ERROR: invalid hex challenge\r\n");
        return;
    }

    uint8_t response[cdc::core::IChallengeResponder::MAX_RESPONSE_LEN] = {};
    int rlen = OathStore::instance().challengeResponse(
        name, challenge, static_cast<size_t>(clen), response, nullptr);
    if (rlen <= 0) {
        cdc::serial::Console::printf("ERROR: no CR entry named '%s'\r\n", name);
        return;
    }

    char hexOut[2 * cdc::core::IChallengeResponder::MAX_RESPONSE_LEN + 1] = {};
    for (int i = 0; i < rlen; i++) {
        snprintf(hexOut + i * 2, 3, "%02x", response[i]);
    }
    cdc::serial::Console::printf("%s\r\n", hexOut);
}

/**
 * \brief Sub-command table for the TOTP serial command group.
 */
static const cdc::serial::SubCommand kTotpSubs[] = {
    {"LIST", "",                                                "List all 2FA entries",        cmd_totp_list},
    {"ADD",  "<type> <name> <secret> [issuer] [digits] [period] [algo] [counter]","Add 2FA entry", cmd_totp_add},
    {"DEL",  "<index>",                                         "Delete 2FA entry by index",   cmd_totp_del},
    {"GET",  "<index>",                                         "Generate code by index",      cmd_totp_get},
    {nullptr, nullptr, nullptr, nullptr},
};

static void cmd_totp(const char* args) {
    cdc::serial::dispatchSubCommand("TOTP", args, kTotpSubs);
}

/**
 * \brief Registers serial commands exposed by the 2FA module.
 */
static void registerCommands() {
    if (s_commandsRegistered) return;
    auto& reg = cdc::serial::getCommandRegistry();
    reg.registerCommand({"TOTP",
                         "2FA authenticator: LIST/ADD/DEL/GET (TOTP+HOTP)",
                         cmd_totp, CMD_MODULE, true, kTotpSubs});
    reg.registerCommand({"CHALRESP",
                         "Challenge-response: <name> <hex-challenge> (CR entries)",
                         cmd_chalresp, CMD_MODULE, true, nullptr});
    s_commandsRegistered = true;
}

/** \brief OATH code detail view implementation. */

/** \brief Forward declaration for edit wizard entry point. */
static void wizardEdit(uint16_t slot);

class OathCodeView : public ui::ViewBase {
public:
    /**
     * \brief Initializes the code view for a specific account slot.
     * \param slot Logical OATH slot.
     * \param name Account display name.
     */
    void init(uint16_t slot, const char* name) {
        slot_ = slot;
        strncpy(name_, name ? name : "", sizeof(name_) - 1);
        name_[sizeof(name_) - 1] = '\0';
        issuer_[0] = '\0';
        generated_ = false;
        updateCode();
    }

    /**
     * \brief Refreshes code state when the view is entered.
     * \param context Optional enter context (unused).
     */
    void onEnter(void* context) override {
        (void)context;
        generated_ = false;
        updateCode();
        if (isTotp_ && !timeValid_) {
            ui::showToastError(ui::tr("mod_2fa.time_invalid"));
        }
        dirty_ = true;
    }

    /**
     * \brief Refreshes code state when the view resumes.
     *
     * Invariant: onResume is also invoked by modal dismissal, so it must not
     * show a modal (toast) itself. The time-invalid state is rendered inline.
     */
    void onResume() override {
        generated_ = false;
        updateCode();
        dirty_ = true;
    }

    /**
     * \brief Updates the TOTP countdown/code once per second.
     *
     * HOTP entries are not regenerated on tick: their code is counter-based and
     * advancing it would consume counters silently.
     *
     * \param nowMs Current uptime in milliseconds.
     */
    void onTick(uint32_t nowMs) override {
        if (!isTotp_) return;
        if (nowMs - lastUpdateMs_ >= 1000) {
            lastUpdateMs_ = nowMs;
            updateCode();
            markDirty();
        }
    }

    /**
     * \brief Renders account metadata, code, and validity/progress UI.
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
        cdc::ui::render::printText(gfx, ui::tr("mod_2fa.code"));
        gfx->drawFastHLine(0, 22, display->getWidth(), EPD_BLACK);

        gfx->setCursor(8, 28);
        cdc::ui::render::printText(gfx, name_);
        if (issuer_[0]) {
            gfx->setCursor(8, 40);
            cdc::ui::render::printText(gfx, issuer_);
        }

        if (isCr_) {
            gfx->setCursor(8, 60);
            cdc::ui::render::printText(gfx, ui::tr("mod_2fa.cr_entry"));
            clearDirty();
            return;
        }

        if (isTotp_ && !timeValid_) {
            gfx->setTextSize(1);
            gfx->setCursor(8, 60);
            cdc::ui::render::printText(gfx, ui::tr("mod_2fa.time_invalid"));
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

        gfx->setTextSize(1);
        if (isTotp_) {
            // Progress bar + remaining counter
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
        } else {
            // HOTP: show the counter that produced the current code.
            char line[32];
            snprintf(line, sizeof(line), "%s: %llu", ui::tr("mod_2fa.counter"),
                     static_cast<unsigned long long>(counter_));
            gfx->setCursor(8, 92);
            cdc::ui::render::printText(gfx, line);
        }

        clearDirty();
    }

    /**
     * \brief Handles key actions for back, edit, next (HOTP), and typing.
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
        if (key == '5' && !isTotp_ && !isCr_) {
            // Advance to the next HOTP code on demand.
            generated_ = false;
            updateCode();
            markDirty();
            return ui::InputResult::CONSUMED;
        }
        if (key == 'Y' && !isCr_) {
            auto* kb = core::getKeyboard();
            if (kb && kb->isConnected()) {
                bool valid = isTotp_ ? (timeValid_ && code_[0] != '-') : code_[0] != '\0';
                if (valid) {
                    kb->typeString(code_);
                    ui::showToastSuccess("Typed");
                }
            } else {
                ui::showToastError(ui::tr("mod_2fa.no_keyboard"));
            }
            return ui::InputResult::CONSUMED;
        }
        return ui::InputResult::IGNORED;
    }

    /**
     * \brief Returns the static view identifier.
     * \return View name string.
     */
    const char* getName() const override { return "OathCodeView"; }

    /**
     * \brief Returns context-aware footer hint text.
     * \return Footer hint string.
     */
    const char* getFooterHint() const override {
        if (isCr_) {
            return ui::tr("mod_2fa.hint_edit");
        }
        if (!isTotp_) {
            return ui::tr("mod_2fa.hint_hotp");
        }
        auto* kb = core::getKeyboard();
        if (kb && kb->isConnected()) {
            return ui::tr("mod_2fa.hint_type");
        }
        return ui::tr("mod_2fa.hint_edit");
    }

private:
    /**
     * \brief Recomputes the current code, issuer label, and progress state.
     *
     * For HOTP, generation is guarded so a single display refresh consumes only
     * one counter value (the user advances explicitly with key `5`).
     */
    void updateCode() {
        OathStore& store = OathStore::instance();

        OathEntry account = {};
        if (!store.readAccount(slot_, &account)) {
            isTotp_ = true;
            isCr_ = false;
            timeValid_ = false;
            strncpy(code_, "------", sizeof(code_) - 1);
            remaining_ = 0;
            period_ = 0;
            return;
        }

        isCr_ = account.type == static_cast<uint8_t>(OathType::CR);
        isTotp_ = account.type == static_cast<uint8_t>(OathType::TOTP);
        strncpy(issuer_, account.issuer, sizeof(issuer_) - 1);
        issuer_[sizeof(issuer_) - 1] = '\0';
        period_ = account.period;

        if (isCr_) {
            // CR has no displayable code; it is consumed via the transports.
            code_[0] = '\0';
            remaining_ = 0;
            return;
        }

        if (!isTotp_) {
            // HOTP: generate exactly once per explicit user action.
            if (generated_) return;
            counter_ = account.counter;
            int8_t rc = store.generateCode(slot_, code_, sizeof(code_));
            if (rc < 0) {
                strncpy(code_, "------", sizeof(code_) - 1);
            }
            generated_ = true;
            remaining_ = 0;
            return;
        }

        timeValid_ = store.isTimeValid();
        if (!timeValid_) {
            strncpy(code_, "------", sizeof(code_) - 1);
            remaining_ = 0;
            return;
        }
        int8_t rem = store.generateCode(slot_, code_, sizeof(code_));
        if (rem >= 0) {
            remaining_ = static_cast<uint8_t>(rem);
        } else {
            timeValid_ = false;
            strncpy(code_, "------", sizeof(code_) - 1);
            remaining_ = 0;
        }
    }

    uint16_t slot_ = 0;
    char name_[OathStore::NAME_LEN + 1] = {};
    char issuer_[OathStore::ISSUER_LEN + 1] = {};
    char code_[9] = {};
    uint8_t remaining_ = 0;
    uint32_t period_ = 0;
    uint64_t counter_ = 0;
    bool isTotp_ = true;
    bool isCr_ = false;
    bool timeValid_ = false;
    bool generated_ = false;
    uint32_t lastUpdateMs_ = 0;
};

/** \brief 2FA module UI state. */

/** \brief Static view instances (no dynamic allocation, no leaks). */
static ui::ListView s_listView;
static ui::T9InputView s_t9Input;
static ui::ListView s_typeMenu;
static ui::ListView s_digitsMenu;
static ui::ListView s_algoMenu;
static ui::ListView s_periodMenu;
static ui::ListView s_touchMenu;
static ui::ListView s_usbCrMenu;
static OathCodeView s_codeView;
static bool s_viewsInitialized = false;

/** \brief Dynamic list buffers released by `freeListBuffers`. */
static ui::ListItem* s_listItems = nullptr;
static char (*s_listLabels)[24] = nullptr;
static uint16_t* s_listSlots = nullptr;
static uint16_t s_accountCount = 0;
static uint16_t s_capacity = 0;

struct WizardState {
    uint8_t type;
    char name[OathStore::NAME_LEN + 1];
    char secret[SECRET_B32_LEN];
    char issuer[OathStore::ISSUER_LEN + 1];
    uint8_t digits;
    uint8_t algorithm;
    uint32_t period;
    uint64_t counter;
    uint8_t flags;
    bool editMode;
    uint16_t editSlot;
};

EXT_RAM_BSS_ATTR static WizardState s_wizard = {};

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
 * \brief Frees dynamically allocated list buffers used by the account list.
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
static void onWizardType(uint16_t index, void* userData);
static void onWizardName(const char* text);
static void onWizardSecret(const char* text);
static void onWizardIssuer(const char* text);
static void onWizardDigits(uint16_t index, void* userData);
static void onWizardAlgo(uint16_t index, void* userData);
static void onWizardPeriod(uint16_t index, void* userData);
static void onWizardTouch(uint16_t index, void* userData);
static void onWizardUsbCr(uint16_t index, void* userData);
static void pushAlgoStep();
static void pushTouchStep();
static void pushUsbCrStep();
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
    uint16_t cap = OathStore::instance().capacity();
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
 * \brief Rebuilds the account list view content from Tropic storage cache.
 */
static void rebuildList() {
    if (!ensureListBuffers()) {
        cdc::core::ModuleRegistry::instance().reportModuleError(TwoFaModule::instance().getName(),
                                                               "2FA list allocation failed");
        return;
    }
    s_accountCount = 0;
    s_listItems[0] = {ui::tr("mod_2fa.add_account"), 0, false, nullptr};

    auto cb = [](uint16_t slot, const cdc::core::TropicStorage::CacheEntry& entry, void* user) {
        (void)user;
        if (s_accountCount >= s_capacity) return;
        uint16_t logical = 0;
        if (!OathStore::instance().toLogicalSlot(slot, &logical)) return;
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
        OathStore::instance().moduleId(),
        OathStore::instance().rmemStart(),
        OathStore::instance().rmemEnd(),
        cb, nullptr);

    s_listView.init(ui::tr("mod_2fa.title"), s_listItems, static_cast<uint16_t>(s_accountCount + 1));
}

/**
 * \brief Handles selection from the account list.
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
    s_wizard.type = static_cast<uint8_t>(OathType::TOTP);
    s_wizard.digits = OathStore::DEFAULT_DIGITS;
    s_wizard.algorithm = static_cast<uint8_t>(OathAlgorithm::SHA1);
    s_wizard.period = OathStore::DEFAULT_PERIOD;
    s_wizard.counter = 0;
    s_wizard.flags = 0;
    s_wizard.editMode = false;
    s_wizard.editSlot = 0;

    static ui::ListItem typeItems[3] = {
        {"TOTP", 0, false, nullptr},
        {"HOTP", 0, false, nullptr},
        {"CR", 0, false, nullptr}
    };
    s_typeMenu.setOnSelect(onWizardType);
    s_typeMenu.init(ui::tr("mod_2fa.type"), typeItems, 3);
    ui::ViewStack::instance().push(&s_typeMenu);
}

/**
 * \brief Starts edit wizard prefilled with an existing account.
 * \param slot Logical slot to edit.
 */
static void wizardEdit(uint16_t slot) {
    OathEntry account = {};
    if (!OathStore::instance().readAccount(slot, &account)) {
        ui::showToastError(ui::tr("core.failed"));
        return;
    }

    memset(&s_wizard, 0, sizeof(s_wizard));
    s_wizard.type = account.type;
    strncpy(s_wizard.name, account.name, sizeof(s_wizard.name) - 1);
    strncpy(s_wizard.issuer, account.issuer, sizeof(s_wizard.issuer) - 1);
    s_wizard.digits = account.digits;
    s_wizard.algorithm = account.algorithm;
    s_wizard.period = account.period;
    s_wizard.counter = account.counter;
    s_wizard.flags = account.flags;
    s_wizard.editMode = true;
    s_wizard.editSlot = slot;
    base32Encode(account.secret, account.secretLen, s_wizard.secret, sizeof(s_wizard.secret));

    // The entry type stays fixed when editing; jump straight to the name step.
    pushT9WizardStep(ui::tr("mod_2fa.account_name"), s_wizard.name, OathStore::NAME_LEN, onWizardName);
}

/**
 * \brief Saves selected entry type and opens the name step.
 * \param index Selected list index.
 * \param userData Optional user pointer (unused).
 */
static void onWizardType(uint16_t index, void* userData) {
    (void)userData;
    switch (index) {
        case 1:  s_wizard.type = static_cast<uint8_t>(OathType::HOTP); break;
        case 2:  s_wizard.type = static_cast<uint8_t>(OathType::CR);   break;
        default: s_wizard.type = static_cast<uint8_t>(OathType::TOTP); break;
    }
    pushT9WizardStep(ui::tr("mod_2fa.account_name"), nullptr, OathStore::NAME_LEN, onWizardName);
}

/**
 * \brief Saves wizard account name and opens secret step.
 * \param text Entered account name.
 */
static void onWizardName(const char* text) {
    strncpy(s_wizard.name, text ? text : "", sizeof(s_wizard.name) - 1);
    pushT9WizardStep(ui::tr("mod_2fa.secret"), s_wizard.secret, SECRET_B32_LEN - 1, onWizardSecret);
}

/**
 * \brief Saves wizard secret and opens issuer step.
 * \param text Entered Base32 secret.
 */
static void onWizardSecret(const char* text) {
    strncpy(s_wizard.secret, text ? text : "", sizeof(s_wizard.secret) - 1);
    // CR has no issuer/digits/period: jump straight to the algorithm step.
    if (s_wizard.type == static_cast<uint8_t>(OathType::CR)) {
        pushAlgoStep();
        return;
    }
    pushT9WizardStep(ui::tr("mod_2fa.issuer"), s_wizard.issuer, OathStore::ISSUER_LEN, onWizardIssuer);
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
    s_digitsMenu.init(ui::tr("mod_2fa.digits"), digitsItems, 3);
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
    pushAlgoStep();
}

/**
 * \brief Pushes the algorithm-selection step.
 *
 * CR offers only SHA1/SHA256 (the algorithms the transports carry); TOTP/HOTP
 * additionally offer SHA512. The selected list index maps 1:1 to the
 * `OathAlgorithm` enum value.
 */
static void pushAlgoStep() {
    static ui::ListItem algoItems[3] = {
        {"SHA1", 0, false, nullptr},
        {"SHA256", 0, false, nullptr},
        {"SHA512", 0, false, nullptr}
    };
    bool isCr = s_wizard.type == static_cast<uint8_t>(OathType::CR);
    s_algoMenu.setOnSelect(onWizardAlgo);
    s_algoMenu.init(ui::tr("mod_2fa.algorithm"), algoItems, isCr ? 2 : 3);
    ui::ViewStack::instance().push(&s_algoMenu);
}

/**
 * \brief Saves selected algorithm; opens period step (TOTP) or finishes (HOTP).
 * \param index Selected list index.
 * \param userData Optional user pointer (unused).
 */
static void onWizardAlgo(uint16_t index, void* userData) {
    (void)userData;
    s_wizard.algorithm = static_cast<uint8_t>(index % 3);

    if (s_wizard.type == static_cast<uint8_t>(OathType::CR)) {
        // CR has no period; offer the touch-confirm toggle, then finalize.
        pushTouchStep();
        return;
    }

    if (s_wizard.type == static_cast<uint8_t>(OathType::HOTP)) {
        // HOTP has no period; finalize directly (counter stays from edit/default).
        wizardFinish();
        return;
    }

    static ui::ListItem periodItems[2] = {
        {"30s", 0, false, nullptr},
        {"60s", 0, false, nullptr}
    };
    s_periodMenu.setOnSelect(onWizardPeriod);
    s_periodMenu.init(ui::tr("mod_2fa.period"), periodItems, 2);
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
 * \brief Pushes the CR touch-confirm toggle step.
 *
 * Touch confirmation defaults to on; the first list entry (index 0) sets the
 * touch flag, the second clears it.
 */
static void pushTouchStep() {
    static ui::ListItem touchItems[2] = {};
    touchItems[0] = {ui::tr("mod_2fa.touch_on"), 0, false, nullptr};
    touchItems[1] = {ui::tr("mod_2fa.touch_off"), 0, false, nullptr};
    s_touchMenu.setOnSelect(onWizardTouch);
    s_touchMenu.init(ui::tr("mod_2fa.touch"), touchItems, 2);
    ui::ViewStack::instance().push(&s_touchMenu);
}

/**
 * \brief Saves the CR touch-confirm choice and finalizes the entry.
 * \param index Selected list index (0 = required, 1 = not required).
 * \param userData Unused.
 */
static void onWizardTouch(uint16_t index, void* userData) {
    (void)userData;
    if (index == 0) {
        s_wizard.flags |= OathFlag::TOUCH_REQUIRED;
    } else {
        s_wizard.flags &= ~OathFlag::TOUCH_REQUIRED;
    }
    pushUsbCrStep();
}

/**
 * \brief Pushes the CR USB-slot-2 designation step.
 *
 * Defaults to off; the first entry (index 0) marks this CR entry as the single
 * USB OTP-HID slot-2 responder, the second leaves it undesignated.
 */
static void pushUsbCrStep() {
    static ui::ListItem usbCrItems[2] = {};
    usbCrItems[0] = {ui::tr("mod_2fa.usb_cr_on"), 0, false, nullptr};
    usbCrItems[1] = {ui::tr("mod_2fa.usb_cr_off"), 0, false, nullptr};
    s_usbCrMenu.setOnSelect(onWizardUsbCr);
    s_usbCrMenu.init(ui::tr("mod_2fa.usb_cr"), usbCrItems, 2);
    ui::ViewStack::instance().push(&s_usbCrMenu);
}

/**
 * \brief Saves the USB-CR-slot designation and finalizes the entry.
 * \param index Selected list index (0 = designate, 1 = leave undesignated).
 * \param userData Unused.
 */
static void onWizardUsbCr(uint16_t index, void* userData) {
    (void)userData;
    if (index == 0) {
        s_wizard.flags |= OathFlag::USB_CR_SLOT;
    } else {
        s_wizard.flags &= ~OathFlag::USB_CR_SLOT;
    }
    wizardFinish();
}

/**
 * \brief Validates wizard data and persists account changes.
 */
static void wizardFinish() {
    if (strlen(s_wizard.name) == 0 || strlen(s_wizard.secret) == 0) {
        ui::showToastError(ui::tr("mod_2fa.invalid_input"));
        ui::ViewStack::instance().popToAnchor(&s_listView);
        return;
    }

    bool ok = false;
    if (s_wizard.editMode) {
        ok = OathStore::instance().updateAccount(
            s_wizard.editSlot,
            s_wizard.type,
            s_wizard.name,
            strlen(s_wizard.issuer) > 0 ? s_wizard.issuer : nullptr,
            s_wizard.secret,
            s_wizard.digits,
            s_wizard.period,
            s_wizard.algorithm,
            s_wizard.counter,
            s_wizard.flags
        );
    } else {
        ok = OathStore::instance().addAccount(
            s_wizard.type,
            s_wizard.name,
            strlen(s_wizard.issuer) > 0 ? s_wizard.issuer : nullptr,
            s_wizard.secret,
            s_wizard.digits,
            s_wizard.period,
            s_wizard.algorithm,
            s_wizard.counter,
            s_wizard.flags
        );
    }

    if (ok) {
        // Enforce a single USB-CR responder: when this entry was designated,
        // demote any previously designated entry.
        if (s_wizard.flags & OathFlag::USB_CR_SLOT) {
            uint16_t slot = 0;
            if (OathStore::instance().findByName(s_wizard.name, &slot)) {
                OathStore::instance().clearUsbCrFlagExcept(slot);
            }
        }
        ui::showToastSuccess(ui::tr("core.ok"));
        rebuildList();
        ui::ViewStack::instance().popToAnchor(&s_listView);
    } else {
        ui::showToastError(ui::tr("core.failed"));
    }
}

/**
 * \brief Returns singleton 2FA module instance.
 * \return Module singleton reference.
 */
TwoFaModule& TwoFaModule::instance() {
    static TwoFaModule inst;
    return inst;
}

/**
 * \brief Initializes module resources, translations, commands, and slot mapping.
 * \return `true` if module initialization succeeded.
 */
bool TwoFaModule::init() {
    LOG_I(TAG, "Initializing 2FA module");
    registerStrings();
    registerCommands();

    core::ModuleRegistry::instance().registerModule(this);
    if (slotRange_.hasRmem) {
        OathStore::instance().setSlotRange(slotRange_);
        core::ModuleRegistry::instance().clearModuleErrorByName(getName());
    } else {
        core::ModuleRegistry::instance().reportModuleError(getName(), "2FA slot range missing");
        state_ = core::ServiceState::ERROR;
        return false;
    }

    // Offer the challenge-response service for transport modules (USB OTP-HID,
    // BLE). The BLE CR transport itself lives in this module.
    core::ServiceRegistry::instance().provide<core::IChallengeResponder>(
        core::ServiceType::CHALLENGE_RESPONDER, this);
    if (!ble_chalresp_init()) {
        LOG_W(TAG, "BLE CR init failed (BLE might not be available)");
    }

    state_ = core::ServiceState::INITIALIZED;
    return true;
}

/**
 * \brief Starts the 2FA module service.
 * \return `true` if the start transition succeeded.
 */
bool TwoFaModule::start() {
    if (state_ != core::ServiceState::INITIALIZED && state_ != core::ServiceState::STOPPED) {
        return false;
    }
    // Expose the accounts over CCID (YKOATH). The interface may be unavailable
    // (service disabled / USB budget); the module still starts and the on-device
    // TOTP UI keeps working regardless.
    oath_backend_install();
    usbAcquired_ = scard_usb_acquire();
    if (!usbAcquired_) {
        LOG_W(TAG, "CCID interface unavailable, OATH applet inactive");
    } else if (!scard_register_applet(oath_applet(), false)) {
        LOG_W(TAG, "OATH applet registration failed");
    }
    state_ = core::ServiceState::STARTED;
    return true;
}

/**
 * \brief Forwards the BLE CR state machine on the main task.
 * \param nowMs Current uptime in milliseconds.
 */
void TwoFaModule::onTick(uint32_t nowMs) {
    ble_chalresp_tick(nowMs);
}

/**
 * \brief Computes the raw HMAC challenge-response for a named CR entry.
 *
 * Delegates to `OathStore`, which owns the HMAC engine and entry lookup. The
 * touch/PIN gate is enforced by the transport, not here.
 *
 * \param entryName CR credential name.
 * \param challenge Challenge bytes.
 * \param clen Challenge length.
 * \param out Output buffer (>= IChallengeResponder::MAX_RESPONSE_LEN).
 * \return Response length, or `-1` on failure.
 */
int TwoFaModule::challengeResponse(const char* entryName, const uint8_t* challenge,
                                   size_t clen, uint8_t* out) {
    return OathStore::instance().challengeResponse(entryName, challenge, clen, out, nullptr);
}

/**
 * \brief Computes the raw HMAC response for the designated USB-CR slot entry.
 *
 * Delegates to `OathStore`, which resolves the single entry flagged
 * `OathFlag::USB_CR_SLOT` and reports its touch requirement.
 *
 * \param challenge Challenge bytes.
 * \param clen Challenge length.
 * \param out Output buffer (>= IChallengeResponder::MAX_RESPONSE_LEN).
 * \param touchRequiredOut Optional; receives the entry's touch-required flag.
 * \return Response length, or `-1` when no entry is designated.
 */
int TwoFaModule::challengeResponseUsbSlot(const uint8_t* challenge, size_t clen,
                                          uint8_t* out, bool* touchRequiredOut) {
    return OathStore::instance().challengeResponseUsbSlot(challenge, clen, out, touchRequiredOut);
}

/**
 * \brief Stops the 2FA module and releases list buffers.
 */
void TwoFaModule::stop() {
    if (usbAcquired_) {
        scard_unregister_applet("oath");
        scard_usb_release();
        usbAcquired_ = false;
    }
    ble_chalresp_deinit();
    freeListBuffers();
    ModuleBase::stop();
}

/**
 * \brief Stores assigned Tropic slot range for the module.
 * \param range Slot assignment from module registry.
 */
void TwoFaModule::setSlotRange(const core::IModule::SlotRange& range) {
    slotRange_ = range;
}

/**
 * \brief Declares minimum slot requirements for the 2FA module.
 * \return Slot request structure for registry planning.
 */
core::IModule::SlotRequest TwoFaModule::getSlotRequest() const {
    core::IModule::SlotRequest req = {};
    req.mapName = getName();
    req.minRmemSlots = 1;
    return req;
}

/**
 * \brief Provides main-menu entry for the 2FA module.
 * \param items Output array for menu items.
 * \param maxItems Maximum number of writable entries in `items`.
 * \return Number of populated menu items.
 */
uint8_t TwoFaModule::getMenuItems(core::ModuleMenuItem* items, uint8_t maxItems) {
    if (!items || maxItems == 0) return 0;

    items[0] = {ui::tr("mod_2fa.title"), 50, []() -> ui::IView* {
        if (!s_viewsInitialized) {
            s_listView.setOnSelect(onListSelect);
            s_viewsInitialized = true;
        }
        rebuildList();
        return &s_listView;
    }, nullptr, getName(), core::MenuLocation::MAIN_MENU, nullptr};

    return 1;
}

/// Schema version written to and expected from the 2FA backup section.
static constexpr int kSchemaVer = 1;

/// Maximum Base32 string length for the longest supported secret (64 bytes raw).
static constexpr size_t kBase32BufLen = 103 + 1;  // ceil(64*8/5) = 103 chars + null

/**
 * \brief Exports all stored OATH entries into the module's backup section.
 *
 * Writes `schema_ver` and an `entries` array; each element carries the full
 * set of fields needed to reconstruct the entry. Secrets are Base32-encoded
 * so they round-trip through the existing decoder. Returns `false` when there
 * are no entries to export.
 *
 * \param out cJSON object that forms the module's section in the backup file.
 * \return `true` if at least one entry was exported.
 */
bool TwoFaModule::exportBackup(cJSON* out) {
    if (!out) return false;

    auto& store = OathStore::instance();
    if (!store.hasSlotRange()) return false;

    cJSON_AddNumberToObject(out, "schema_ver", kSchemaVer);
    cJSON* entries = cJSON_AddArrayToObject(out, "entries");
    if (!entries) return false;

    struct ExportCtx {
        cJSON* arr;
        uint16_t count;
    } ctx = { entries, 0 };

    auto cb = [](uint16_t slot, const cdc::core::TropicStorage::CacheEntry&, void* user) {
        auto* c = static_cast<ExportCtx*>(user);
        auto& store = OathStore::instance();

        uint16_t logical = 0;
        if (!store.toLogicalSlot(slot, &logical)) return;

        OathEntry entry = {};
        if (!store.readAccount(logical, &entry)) return;

        char b32[kBase32BufLen] = {};
        base32Encode(entry.secret, entry.secretLen, b32, sizeof(b32));

        cJSON* obj = cJSON_CreateObject();
        if (!obj) return;

        cJSON_AddStringToObject(obj, "name",      entry.name);
        cJSON_AddStringToObject(obj, "issuer",    entry.issuer);
        cJSON_AddNumberToObject(obj, "type",      entry.type);
        cJSON_AddNumberToObject(obj, "algorithm", entry.algorithm);
        cJSON_AddNumberToObject(obj, "digits",    entry.digits);
        cJSON_AddNumberToObject(obj, "period",    entry.period);
        // counter stored as a double; at 53 bits of mantissa this covers
        // all meaningful HOTP counters without loss.
        cJSON_AddNumberToObject(obj, "counter",   static_cast<double>(entry.counter));
        cJSON_AddNumberToObject(obj, "flags",     entry.flags);
        cJSON_AddStringToObject(obj, "secret",    b32);

        cJSON_AddItemToArray(c->arr, obj);
        c->count++;
    };

    cdc::core::TropicStorage::instance().forEachSlot(
        store.moduleId(),
        store.rmemStart(),
        store.rmemEnd(),
        cb, &ctx);

    return ctx.count > 0;
}

/**
 * \brief Maps and upserts one OATH entry from its JSON representation.
 *
 * If an entry with the same name already exists it is overwritten (backup
 * wins); otherwise a new slot is allocated. Malformed or unstorable entries
 * return `false` so the caller can tally them as failed.
 *
 * \param entry JSON array element.
 * \param user Unused.
 * \return `true` if the entry was stored.
 */
static bool importOathEntry(const cJSON* entry, void* user) {
    (void)user;
    if (!cJSON_IsObject(entry)) return false;

    const cJSON* jName    = cJSON_GetObjectItemCaseSensitive(entry, "name");
    const cJSON* jIssuer  = cJSON_GetObjectItemCaseSensitive(entry, "issuer");
    const cJSON* jType    = cJSON_GetObjectItemCaseSensitive(entry, "type");
    const cJSON* jAlgo    = cJSON_GetObjectItemCaseSensitive(entry, "algorithm");
    const cJSON* jDigits  = cJSON_GetObjectItemCaseSensitive(entry, "digits");
    const cJSON* jPeriod  = cJSON_GetObjectItemCaseSensitive(entry, "period");
    const cJSON* jCounter = cJSON_GetObjectItemCaseSensitive(entry, "counter");
    const cJSON* jFlags   = cJSON_GetObjectItemCaseSensitive(entry, "flags");
    const cJSON* jSecret  = cJSON_GetObjectItemCaseSensitive(entry, "secret");

    if (!cJSON_IsString(jName)   || !jName->valuestring   || jName->valuestring[0] == '\0' ||
        !cJSON_IsString(jSecret) || !jSecret->valuestring || jSecret->valuestring[0] == '\0' ||
        !cJSON_IsNumber(jType)   ||
        !cJSON_IsNumber(jAlgo)   ||
        !cJSON_IsNumber(jDigits) ||
        !cJSON_IsNumber(jPeriod)) {
        LOG_W(TAG, "2FA import: skipping malformed entry");
        return false;
    }

    const char* name    = jName->valuestring;
    const char* issuer  = (cJSON_IsString(jIssuer) && jIssuer->valuestring) ? jIssuer->valuestring : nullptr;
    const char* secret  = jSecret->valuestring;
    uint8_t type        = static_cast<uint8_t>(jType->valuedouble);
    uint8_t algorithm   = static_cast<uint8_t>(jAlgo->valuedouble);
    uint8_t digits      = static_cast<uint8_t>(jDigits->valuedouble);
    uint32_t period     = static_cast<uint32_t>(jPeriod->valuedouble);
    uint64_t counter    = cJSON_IsNumber(jCounter)
                              ? static_cast<uint64_t>(jCounter->valuedouble)
                              : 0;
    uint8_t flags       = cJSON_IsNumber(jFlags)
                              ? static_cast<uint8_t>(jFlags->valuedouble)
                              : 0;

    auto& store = OathStore::instance();
    uint16_t existingSlot = 0;
    bool ok;
    if (store.findByName(name, &existingSlot)) {
        ok = store.updateAccount(existingSlot, type, name, issuer,
                                 secret, digits, period, algorithm, counter, flags);
    } else {
        ok = store.addAccount(type, name, issuer, secret, digits, period, algorithm, counter, flags);
    }

    if (!ok) {
        LOG_W(TAG, "2FA import: failed to store entry '%s'", name);
    }
    return ok;
}

/**
 * \brief Restores OATH entries from the module's backup section.
 *
 * Best-effort upsert by name; malformed or unstorable entries are skipped and
 * counted, never aborting the restore.
 *
 * \param in cJSON object holding the previously exported section.
 * \return Tally of imported and failed records.
 */
cdc::core::IModule::BackupResult TwoFaModule::importBackup(const cJSON* in) {
    if (!in) return {};

    const cJSON* schemaVer = cJSON_GetObjectItemCaseSensitive(in, "schema_ver");
    if (cJSON_IsNumber(schemaVer) && static_cast<int>(schemaVer->valuedouble) != kSchemaVer) {
        LOG_W(TAG, "2FA backup schema_ver %d != expected %d, skipping",
              static_cast<int>(schemaVer->valuedouble), kSchemaVer);
        return {};
    }

    const cJSON* entries = cJSON_GetObjectItemCaseSensitive(in, "entries");
    return cdc::ui::importJsonArray(entries, importOathEntry, nullptr);
}

} // namespace cdc::mod_2fa

/**
 * \brief Registers 2FA module initializer in the global module registry.
 */
extern "C" void mod_2fa_register() {
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& module = cdc::mod_2fa::TwoFaModule::instance();
        module.init();
    });
}
