#pragma once

#include "cdc_ui/IView.h"
#include <cstdint>

namespace cdc::ui {

/**
 * T9InputView - Multi-tap text input
 *
 * Classic phone-style T9 text entry.
 * Each number key cycles through characters on repeated press.
 *
 * Keys:
 *   0-9 = Character input (multi-tap)
 *   Long-press 0-9 = Insert digit directly
 *   N = Backspace (short) / Cancel (long)
 *   Y = Confirm (triggers callback)
 */
class T9InputView : public ViewBase {
public:
    static constexpr uint16_t MAX_TEXT_LEN = 320;
    static constexpr uint32_t TIMEOUT_MS = 2000;  // Time before character is committed

    /**
     * Save callback (called when Y is pressed)
     * @param text Final text
     */
    using SaveCallback = void(*)(const char* text);

    /**
     * Cancel callback (called when the user dismisses the view via long-press N).
     * The view pops itself before this fires, so do not call pop() in the handler.
     */
    using CancelCallback = void(*)();

    /**
     * Initialize T9 input view
     * @param title View title
     * @param initialText Initial text (optional)
     * @param maxLen Maximum text length (default MAX_TEXT_LEN)
     */
    void init(const char* title, const char* initialText = nullptr, uint16_t maxLen = MAX_TEXT_LEN);

    /**
     * Set save callback (called on Y key)
     */
    void setOnSave(SaveCallback callback) { onSave_ = callback; }

    /**
     * Set cancel callback (called when the view is dismissed without confirming)
     */
    void setOnCancel(CancelCallback callback) { onCancel_ = callback; }

    /**
     * Get current text
     */
    const char* getText() const { return text_; }

    /**
     * Get current text length
     */
    uint16_t getLength() const { return len_; }

    /**
     * Set placeholder text (shown when empty)
     */
    void setPlaceholder(const char* placeholder) { placeholder_ = placeholder; }

    /**
     * Override footer hint for this instance. Pass nullptr to fall back to default.
     */
    void setHint(const char* hint) { hintOverride_ = hint; }

    /**
     * Force insert a digit (for long-press handling)
     */
    void forceDigit(char key);

    /**
     * Append a raw string to the buffer (used by `T9_PASTE` serial command).
     * Truncates at `maxLen_`. Returns number of bytes actually appended.
     */
    uint16_t appendRaw(const char* text);

    // IView implementation
    void render(bool partial) override;
    InputResult onKey(char key) override;
    InputResult onLongPress(char key) override;
    void onTick(uint32_t nowMs) override;
    const char* getName() const override { return "T9InputView"; }
    const char* getFooterHint() const override;

protected:
    static constexpr uint16_t TITLE_MAX_LEN = 48;
    char titleBuf_[TITLE_MAX_LEN + 1] = {0};
    const char* title_ = nullptr;
    const char* placeholder_ = nullptr;
    const char* hintOverride_ = nullptr;
    char text_[MAX_TEXT_LEN + 1] = {0};
    uint16_t len_ = 0;
    uint16_t maxLen_ = MAX_TEXT_LEN;
    SaveCallback onSave_ = nullptr;
    CancelCallback onCancel_ = nullptr;

    // T9 state
    char lastKey_ = 0;
    uint8_t charIndex_ = 0;
    uint32_t lastPressMs_ = 0;
    bool cursorActive_ = false;

    bool processKey(char key);
    void backspace();
    void commitCharacter();

    // T9 helpers
    static char getChar(char key, uint8_t index);
    static uint8_t getCharCount(char key);
};

// ============================================================================
// Convenience Functions
// ============================================================================

/**
 * Show a T9 input view and push it to the ViewStack.
 * Simplest possible API for text input.
 *
 * @param title View title
 * @param initialText Initial text (nullptr for empty)
 * @param onSave Called when Y is pressed (save)
 * @param maxLen Maximum text length
 * @return Pointer to the T9InputView (for further configuration)
 *
 * Example:
 *   showT9Input("Enter Name", nullptr,
 *               [](const char* text) { saveToNVS(text); });
 */
T9InputView* showT9Input(const char* title, const char* initialText,
                         T9InputView::SaveCallback onSave, uint16_t maxLen = 128);

} // namespace cdc::ui
