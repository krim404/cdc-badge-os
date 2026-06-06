/**
 * T9InputView Implementation
 *
 * Multi-tap text input like classic phones.
 */

#include "cdc_views/T9InputView.h"
#include "cdc_views/KeyCodes.h"
#include "cdc_views/RenderHelpers.h"
#include "cdc_views/ToastView.h"
#include "cdc_ui/ViewStack.h"
#include "cdc_ui/I18n.h"
#include "cdc_hal/IDisplay.h"
#include "cdc_log.h"
#include "esp_timer.h"
#include <goodisplay/gdey029T94.h>
#include <cstring>

static const char* TAG = "T9InputView";

/**
 * \brief T9 digit-to-character mapping table.
 *
 * Order per key: lowercase letters, uppercase letters, digit, then accented variants
 * (CP437 single-byte codes). Adafruit-GFX must be configured with cp437(true) for
 * the accented glyphs to render correctly.
 */
static const char* t9_chars[] = {
    " 0",                                                                                                  // 0
    ".@?!,;:'\"()-_#$%&*+=/\\<>[]{}|^~`1"
        "\x9B\x9C\x9D\xA8\xAD\xAE\xAF\xAB\xAC\xF1\xF8\xFD\xE6\xF6",                                        // 1: ¢ £ ¥ ¿ ¡ « » ½ ¼ ± ° ² µ ÷
    "abcABC2\x84\xA0\x83\x85\x86\x91\x8E\x8F\x92\x87\x80",                                                 // 2: ä á â à å æ Ä Å Æ ç Ç
    "defDEF3\x82\x8A\x88\x89\x90",                                                                          // 3: é è ê ë É
    "ghiGHI4\xA1\x8D\x8C\x8B",                                                                              // 4: í ì î ï
    "jklJKL5",                                                                                              // 5
    "mnoMNO6\xA2\x95\x93\x94\x99\xA4\xA5",                                                                  // 6: ó ò ô ö Ö ñ Ñ
    "pqrsPQRS7\xE1",                                                                                        // 7: ß
    "tuvTUV8\x81\x9A\xA3\x97\x96",                                                                          // 8: ü Ü ú ù û
    "wxyzWXYZ9\x98"                                                                                         // 9: ÿ
};

/**
 * \brief Display layout constants.
 */
static constexpr int TITLE_Y = 5;
static constexpr int TEXT_Y = 50;
static constexpr int TEXT_MARGIN = 10;

namespace cdc::ui {

/**
 * \brief Initializes T9 input state and optional initial text.
 * \param title View title text.
 * \param initialText Initial text value.
 * \param maxLen Maximum input length.
 * \return void
 */
void T9InputView::init(const char* title, const char* initialText, uint16_t maxLen) {
    if (title) {
        strncpy(titleBuf_, title, TITLE_MAX_LEN);
        titleBuf_[TITLE_MAX_LEN] = '\0';
        title_ = titleBuf_;
    } else {
        titleBuf_[0] = '\0';
        title_ = titleBuf_;
    }
    maxLen_ = maxLen > MAX_TEXT_LEN ? MAX_TEXT_LEN : maxLen;

    // Copy initial text
    if (initialText) {
        strncpy(text_, initialText, maxLen_);
        text_[maxLen_] = '\0';
        len_ = strlen(text_);
    } else {
        text_[0] = '\0';
        len_ = 0;
    }

    // Reset T9 state
    lastKey_ = 0;
    charIndex_ = 0;
    lastPressMs_ = 0;
    cursorActive_ = false;
    onSave_ = nullptr;
    onCancel_ = nullptr;
    hintOverride_ = nullptr;
    placeholder_ = nullptr;
    dirty_ = true;

    LOG_D(TAG, "init: title='%s', maxLen=%d", title ? title : "(null)", maxLen_);
}

/**
 * \brief Returns the character for a key/index in the T9 mapping.
 * \param key Numeric key (`'0'`..`'9'`).
 * \param index Character index within that key map.
 * \return Mapped character or `\\0` if key is invalid.
 */
char T9InputView::getChar(char key, uint8_t index) {
    if (key < '0' || key > '9') return '\0';
    const char* chars = t9_chars[key - '0'];
    uint8_t count = strlen(chars);
    return chars[index % count];
}

/**
 * \brief Returns the number of mapped characters for a key.
 * \param key Numeric key (`'0'`..`'9'`).
 * \return Number of mapped characters.
 */
uint8_t T9InputView::getCharCount(char key) {
    if (key < '0' || key > '9') return 0;
    return strlen(t9_chars[key - '0']);
}

/**
 * \brief Processes a numeric key press using multi-tap logic.
 * \param key Numeric key (`'0'`..`'9'`).
 * \return `true` if key was processed, otherwise `false`.
 */
bool T9InputView::processKey(char key) {
    if (key < '0' || key > '9') return false;

    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    bool sameKey = (key == lastKey_);
    bool timeout = (now - lastPressMs_) > TIMEOUT_MS;

    if (sameKey && !timeout && len_ > 0) {
        // Cycle through characters for the same key
        charIndex_++;
        uint8_t charCount = getCharCount(key);
        if (charIndex_ >= charCount) {
            charIndex_ = 0;
        }
        // Replace last character
        text_[len_ - 1] = getChar(key, charIndex_);
        cursorActive_ = true;
    } else {
        // New key or timeout - commit previous and add new
        if (len_ < maxLen_) {
            text_[len_++] = getChar(key, 0);
            text_[len_] = '\0';
            charIndex_ = 0;
            cursorActive_ = true;
        } else {
            ui::showToastError(ui::tr("core.t9_full"), 800);
        }
    }

    lastKey_ = key;
    lastPressMs_ = now;
    dirty_ = true;

    return true;
}

/**
 * \brief Removes the last character from the input buffer.
 * \return void
 */
void T9InputView::backspace() {
    if (len_ > 0) {
        len_--;
        text_[len_] = '\0';
        lastKey_ = 0;
        cursorActive_ = false;
        dirty_ = true;
    }
}

/**
 * \brief Inserts a numeric digit literally, bypassing multi-tap mapping.
 * \param key Numeric key (`'0'`..`'9'`).
 * \return void
 */
void T9InputView::forceDigit(char key) {
    if (key < '0' || key > '9') return;

    // A long-press is always preceded by a short-press of the same key, which
    // already inserted the first T9 multi-tap character for that key. Replace
    // that pending character with the literal digit instead of appending.
    if (lastKey_ == key && len_ > 0) {
        text_[len_ - 1] = key;
        commitCharacter();
        dirty_ = true;
        LOG_D(TAG, "forceDigit (replace): key='%c', text='%s'", key, text_);
        return;
    }

    commitCharacter();

    if (len_ < maxLen_) {
        text_[len_++] = key;
        text_[len_] = '\0';
        dirty_ = true;
        LOG_D(TAG, "forceDigit (append): key='%c', text='%s'", key, text_);
    } else {
        ui::showToastError(ui::tr("core.t9_full"), 800);
    }
}

uint16_t T9InputView::appendRaw(const char* text) {
    if (!text) return 0;
    commitCharacter();
    uint16_t added = 0;
    while (*text && len_ < maxLen_) {
        text_[len_++] = *text++;
        added++;
    }
    text_[len_] = '\0';
    lastKey_ = 0;
    charIndex_ = 0;
    dirty_ = true;
    return added;
}

/**
 * \brief Commits the currently active multi-tap character.
 * \return void
 */
void T9InputView::commitCharacter() {
    if (lastKey_ != 0) {
        lastKey_ = 0;
        cursorActive_ = false;
        dirty_ = true;
    }
}

/**
 * \brief Handles timeout-based commit for active multi-tap input.
 * \param nowMs Current monotonic time in milliseconds.
 * \return void
 */
void T9InputView::onTick(uint32_t nowMs) {
    (void)nowMs;  // Use own timestamp for consistent timing

    // Check for timeout to commit character
    if (lastKey_ != 0) {
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        // Safe comparison: only timeout if now > lastPressMs_ (avoid unsigned wrap)
        if (now >= lastPressMs_ && (now - lastPressMs_) > TIMEOUT_MS) {
            commitCharacter();
        }
    }
}

/**
 * \brief Handles key input for save, backspace, and digit entry.
 * \param key Pressed key code.
 * \return Input handling result for the view stack.
 */
InputResult T9InputView::onKey(char key) {
    switch (key) {
        case KEY_YES:  // Confirm
            commitCharacter();
            if (onSave_) {
                // Pop ourselves FIRST, then call callback
                // This prevents the callback's pushed view from being popped
                ViewStack::instance().pop();
                onSave_(text_);
            }
            return InputResult::CONSUMED;  // Already popped ourselves

        case KEY_NO:  // Backspace
            backspace();
            return InputResult::CONSUMED;

        default:
            if (key >= '0' && key <= '9') {
                processKey(key);
                return InputResult::CONSUMED;
            }
            return InputResult::IGNORED;
    }
}

/**
 * \brief Handles long-press actions for clear and forced digit insertion.
 * \param key Long-pressed key code.
 * \return Input handling result for the view stack.
 */
InputResult T9InputView::onLongPress(char key) {
    if (key == KEY_NO) {
        // Cancel: pop ourselves FIRST, then notify, so a view pushed by the
        // callback is not popped by the dispatcher (mirrors the confirm path).
        ViewStack::instance().pop();
        if (onCancel_) {
            onCancel_();
        }
        return InputResult::CONSUMED;
    }

    if (key >= '0' && key <= '9') {
        // Force insert digit
        forceDigit(key);
        return InputResult::CONSUMED;
    }

    return InputResult::IGNORED;
}

/**
 * \brief Returns localized footer hint text.
 * \return Footer hint string.
 */
const char* T9InputView::getFooterHint() const {
    return hintOverride_ ? hintOverride_ : ui::tr("core.hint_t9_input");
}

/**
 * \brief Renders title, text entry box, cursor state, and footer.
 * \param partial Indicates partial/full redraw mode.
 * \return void
 */
void T9InputView::render(bool partial) {
    hal::IDisplay* display = hal::getDisplayInstance();
    if (!display) return;

    auto* gfx = static_cast<Gdey029T94*>(display->getNativeHandle());
    if (!gfx) return;

    const uint16_t width = display->getWidth();
    const uint16_t height = display->getHeight();

    if (!partial) {
        gfx->fillScreen(EPD_WHITE);
    }

    gfx->setTextColor(EPD_BLACK);
    gfx->setTextSize(1);

    // Title + underline
    render::drawHeaderLeft(gfx, title_, TEXT_MARGIN, TITLE_Y, width);

    // Text input area
    gfx->fillRect(TEXT_MARGIN, TEXT_Y - 5, width - TEXT_MARGIN * 2, 30, EPD_WHITE);
    gfx->drawRect(TEXT_MARGIN - 2, TEXT_Y - 7, width - TEXT_MARGIN * 2 + 4, 34, EPD_BLACK);

    gfx->setCursor(TEXT_MARGIN + 2, TEXT_Y);

    if (len_ == 0 && placeholder_) {
        // Show placeholder when empty
        gfx->setTextColor(EPD_DARKGREY);
        render::printText(gfx, placeholder_);
        gfx->setTextColor(EPD_BLACK);
    } else {
        // Show text with cursor
        for (uint16_t i = 0; i < len_; i++) {
            // If this is the last char and cursor is active, invert it
            if (cursorActive_ && i == len_ - 1) {
                int16_t x = gfx->getCursorX();
                int16_t y = gfx->getCursorY();
                gfx->fillRect(x, y - 2, 8, 14, EPD_BLACK);
                gfx->setTextColor(EPD_WHITE);
                gfx->print(text_[i]);
                gfx->setTextColor(EPD_BLACK);
            } else {
                gfx->print(text_[i]);
            }
        }

        // Show cursor at end if not in T9 cycle
        if (!cursorActive_) {
            gfx->print("|");
        }
    }

    // Footer with hint
    char countStr[16];
    snprintf(countStr, sizeof(countStr), "%u/%u  ", len_, maxLen_);
    const char* hint = getFooterHint();
    render::drawFooterBar(gfx, width, height, countStr, hint, true);

    dirty_ = false;
}

/**
 * \brief Convenience factory/helper function.
 */

static T9InputView s_sharedT9Input;

/**
 * \brief Shows a shared T9 input view instance.
 * \param title View title text.
 * \param initialText Initial text value.
 * \param onSave Save callback invoked on confirm.
 * \param maxLen Maximum input length.
 * \return Pointer to the shared `T9InputView` instance.
 */
T9InputView* showT9Input(const char* title, const char* initialText,
                         T9InputView::SaveCallback onSave, uint16_t maxLen) {
    s_sharedT9Input.init(title, initialText, maxLen);
    s_sharedT9Input.setOnSave(onSave);
    ViewStack::instance().push(&s_sharedT9Input);
    return &s_sharedT9Input;
}

} // namespace cdc::ui
