#pragma once

#include "cdc_ui/IView.h"
#include <cstddef>
#include <cstdint>

class Gdey029T94;

namespace cdc::ui {

/**
 * \brief Generic canvas view exposed to WASM plugins for custom UIs.
 *
 * The plugin draws into the body area via host_view_canvas_* draw primitives.
 * Inline widgets (slider, text-input, button) have host-owned input state
 * while their visual representation is rendered by the plugin reading the
 * widget value/text on each frame.
 *
 * Coordinates are body-local: (0,0) is the top-left of the body area
 * (display rows after the header up to the footer bar).
 */
class CanvasView : public ViewBase {
public:
    enum class WidgetType : uint8_t {
        None   = 0,
        Slider = 1,
        Text   = 2,
        Button = 3,
    };

    enum class WidgetEvent : uint8_t {
        Changed   = 1,
        Committed = 2,
        Cancelled = 3,
    };

    static constexpr uint8_t MAX_WIDGETS    = 8;
    static constexpr uint16_t MAX_TEXT_LEN  = 256;
    static constexpr uint16_t T9_SETTLE_MS  = 800;

    /// Retained display list: draw primitives are recorded here and replayed in
    /// render() after the framework clears the screen. Fixed-size, no heap.
    static constexpr uint16_t MAX_CMDS   = 96;
    static constexpr uint16_t TEXT_ARENA = 2048;

    using KeyCallback       = void(*)(char key, uint32_t focused_widget);
    using WidgetCallback    = void(*)(uint32_t widget_id, WidgetEvent event);
    using LongPressCallback = void(*)(char key);

    void init(const char* title);

    void setKeyCallback(KeyCallback cb)        { keyCb_ = cb; }
    void setWidgetCallback(WidgetCallback cb)  { widgetCb_ = cb; }
    /// Register a plugin long-press handler. Setting a non-null callback opts
    /// this canvas into deferred short-press keypad mode while it is active.
    void setLongPressCallback(LongPressCallback cb);
    void setFooter(const char* hint);
    void setKeyRepeat(uint16_t initial_ms, uint16_t repeat_ms);
    void getBodySize(uint16_t* w, uint16_t* h) const;

    void clearBody();
    void setTextSize(uint8_t size)             { textSize_ = size > 0 ? size : 1; }
    void setTextInverted(bool inverted)        { textInverted_ = inverted; }
    /// Select one of the canonical font ids (see cdc_views/Fonts.h).
    void setFontId(uint8_t font_id)            { fontId_ = font_id; }
    void drawText(int16_t x, int16_t y, const char* text);
    void drawTextAligned(int16_t x, int16_t y, int16_t w, const char* text, uint8_t align);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, bool filled);
    void invertRect(int16_t x, int16_t y, int16_t w, int16_t h);
    void drawHLine(int16_t x, int16_t y, int16_t w);
    void drawVLine(int16_t x, int16_t y, int16_t h);
    void commit(bool full_refresh);

    bool addSlider(uint32_t id, int32_t min, int32_t max, int32_t initial, int32_t step);
    bool addText(uint32_t id, uint16_t max_len, const char* initial);
    bool addButton(uint32_t id);
    bool removeWidget(uint32_t id);

    bool setValue(uint32_t id, int32_t value);
    bool getValue(uint32_t id, int32_t* out) const;
    bool setText(uint32_t id, const char* text);
    int  getText(uint32_t id, char* out, size_t cap) const;

    bool setFocus(uint32_t id);
    uint32_t getFocus() const                  { return focused_; }

    void render(bool partial) override;
    InputResult onKey(char key) override;
    InputResult onLongPress(char key) override;
    void onEnter(void* context) override;
    void onResume() override;
    void onExit() override;
    const char* getName() const override       { return "CanvasView"; }

private:
    enum class CmdType : uint8_t { Text, TextAligned, Rect, HLine, VLine };

    struct DrawCmd {
        CmdType  type     = CmdType::Text;
        int16_t  x        = 0;
        int16_t  y        = 0;
        int16_t  w        = 0;
        int16_t  h        = 0;
        uint16_t strOff   = 0;   // byte offset into textArena_
        uint16_t strLen   = 0;   // string length (excluding NUL)
        uint8_t  align    = 0;
        uint8_t  fontId   = 0;
        uint8_t  textSize = 1;
        bool     filled   = false;
        bool     inverted = false;
    };

    struct Widget {
        uint32_t   id        = 0;
        WidgetType type      = WidgetType::None;
        int32_t    value     = 0;
        int32_t    min       = 0;
        int32_t    max       = 0;
        int32_t    step      = 1;
        uint16_t   max_len   = 0;
        uint16_t   text_len  = 0;
        char       text[MAX_TEXT_LEN] = {0};
        char       t9_last_key   = 0;
        uint8_t    t9_press_count = 0;
        uint32_t   t9_last_time  = 0;
    };

    Widget*       findWidget(uint32_t id);
    const Widget* findWidget(uint32_t id) const;
    Widget*       focusedWidget();
    InputResult   dispatchKeyToWidget(Widget& w, char key);
    void          t9_commit_pending(Widget& w);
    void          t9_apply_key(Widget& w, char key, uint32_t now);

    Gdey029T94* gfx() const;
    void        applyKeypadConfig();
    uint16_t    internText(const char* text, uint16_t* outLen);
    void        replayDisplayList();
    void        paintText(int16_t x, int16_t y, int16_t w, const char* text,
                          uint8_t align, uint8_t fontId, uint8_t textSize,
                          bool inverted);
    int         bodyTop() const                { return headerHeight_; }
    int         bodyBottom() const;

    Widget    widgets_[MAX_WIDGETS] {};
    uint8_t   widgetCount_ = 0;
    uint32_t  focused_     = 0;

    const char*       footer_ = nullptr;
    KeyCallback       keyCb_ = nullptr;
    WidgetCallback    widgetCb_ = nullptr;
    LongPressCallback longPressCb_ = nullptr;

    DrawCmd   cmds_[MAX_CMDS] {};
    uint16_t  cmdCount_ = 0;
    char      textArena_[TEXT_ARENA] {};
    uint16_t  textArenaUsed_ = 0;
    bool      overflowLogged_ = false;

    uint8_t   textSize_ = 1;
    uint8_t   fontId_ = 0;
    bool      textInverted_ = false;
    int       headerHeight_ = 18;
    bool      needsFullRefresh_ = true;

    uint16_t  keyRepeatInitialMs_ = 0;
    uint16_t  keyRepeatPeriodMs_  = 0;
    bool      headerDrawnOnce_    = false;
};

} // namespace cdc::ui
