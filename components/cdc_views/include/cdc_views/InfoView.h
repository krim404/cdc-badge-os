#pragma once

#include "cdc_ui/IView.h"
#include "cdc_core/Raii.h"
#include <cstdint>

namespace cdc::ui {

/**
 * InfoView - Scrollable text display
 *
 * Displays multi-line text with vertical scrolling.
 * Good for help screens, about pages, long messages.
 *
 * Keys:
 *   2 = Scroll up
 *   8 = Scroll down
 *   N = Back (REQUEST_POP)
 */
class InfoView : public ViewBase {
public:
    static constexpr uint16_t MAX_TEXT_LEN = 2048;
    static constexpr uint8_t VISIBLE_LINES = 6;
    static constexpr uint8_t LINE_HEIGHT = 14;

    /**
     * Initialize info view
     * @param title View title
     * @param text Text to display (can be multi-line with \n)
     */
    void init(const char* title, const char* text);

    /**
     * Set custom footer hint
     */
    void setHint(const char* hint) { customHint_ = hint; }

    /**
     * Set optional Y/N callbacks (for approve/deny prompts)
     * If set, Y/N will trigger callbacks instead of popping the view.
     */
    using YesNoCallback = void(*)(void* userData);
    void setYesNoCallbacks(YesNoCallback onYes, YesNoCallback onNo, void* userData = nullptr) {
        onYes_ = onYes;
        onNo_ = onNo;
        callbackUserData_ = userData;
    }

    // IView implementation
    void render(bool partial) override;
    InputResult onKey(char key) override;
    InputResult onLongPress(char key) override;
    const char* getName() const override { return "InfoView"; }
    const char* getFooterHint() const override;

private:
    static constexpr uint16_t MAX_TITLE_LEN = 64;

    char titleBuf_[MAX_TITLE_LEN];
    // Body text lives in PSRAM (allocated lazily on first init); the buffer is
    // large and only touched from the main/UI task, so it must not sit in the
    // scarce internal heap.
    cdc::core::PsramUniquePtr<char> textBuf_;
    const char* customHint_ = nullptr;
    uint16_t scrollLine_ = 0;
    uint16_t totalLines_ = 0;
    YesNoCallback onYes_ = nullptr;
    YesNoCallback onNo_ = nullptr;
    void* callbackUserData_ = nullptr;

    void scroll(bool down);
    uint16_t countLines() const;
};

// ============================================================================
// Convenience Functions
// ============================================================================

/**
 * Show an info view and push it to the ViewStack.
 * Simplest API for displaying scrollable text.
 *
 * @param title View title
 * @param text Text to display (multi-line with \n)
 * @param hint Optional custom footer hint
 * @return Pointer to the InfoView
 *
 * Example:
 *   showInfo("Help", "Line 1\nLine 2\nLine 3...");
 */
InfoView* showInfo(const char* title, const char* text, const char* hint = nullptr);

} // namespace cdc::ui
