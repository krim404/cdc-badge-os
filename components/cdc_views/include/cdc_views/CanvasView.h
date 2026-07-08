#pragma once

#include "cdc_ui/IView.h"
#include "cdc_core/Raii.h"
#include "cdc_views/CanvasAnim.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
class CanvasView : public ViewBase, private anim::AnimTarget {
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
    static constexpr uint8_t MAX_ELEMS     = 16;
    static constexpr uint16_t MAX_TEXT_LEN  = 256;
    static constexpr uint16_t T9_SETTLE_MS  = 800;

    /// Retained display list: draw primitives are recorded here and replayed in
    /// render() after the framework clears the screen. Arenas are PSRAM-backed.
    static constexpr uint16_t MAX_CMDS   = 128;
    static constexpr uint16_t TEXT_ARENA = 2048;
    /// Pixel-data arena for recorded bitmaps. 8 KB holds one full-body mono bitmap.
    static constexpr uint16_t BLOB_ARENA = 8192;
    /// Pixel arena for sprite frame sheets (lazy PSRAM allocation).
    static constexpr uint32_t SPRITE_ARENA = 65536;

    /// Animation refresh policies (mirror HOST_CANVAS_ANIM_REFRESH_*).
    enum : uint8_t {
        ANIM_REFRESH_AUTO  = 0,  ///< PARTIAL_LIGHT while running + FAST cleanup.
        ANIM_REFRESH_LIGHT = 1,  ///< PARTIAL_LIGHT always, no automatic cleanup.
    };
    /// Animation step-rate cap. The partial waveform (~200-300 ms) bounds the
    /// panel to ~5 Hz; anything above just burns replay CPU.
    static constexpr uint8_t  ANIM_FPS_DEFAULT      = 4;
    static constexpr uint8_t  ANIM_FPS_MAX          = 5;
    /// Idle time after the last animation before the AUTO ghost-cleanup FAST.
    static constexpr uint16_t ANIM_CLEANUP_IDLE_MS  = 1000;
    /// Endless animations get one hygiene FAST every this many anim commits.
    static constexpr uint8_t  ANIM_HYGIENE_COMMITS  = 128;

    using KeyCallback       = void(*)(char key, uint32_t focused_widget);
    using WidgetCallback    = void(*)(uint32_t widget_id, WidgetEvent event);
    using LongPressCallback = void(*)(char key);
    /// Fired outside the edit mutex for every finished tween/sprite playback
    /// that carries a done-action id.
    using AnimCallback      = void(*)(uint32_t done_action_id, uint32_t handle,
                                      uint32_t ref_id);

    void init(const char* title);

    /**
     * Set an optional recursive mutex serialising display-list access.
     *
     * When set (non-null), the draw/element mutators and the render-time
     * replay acquire it. A cross-task writer (plugin host calls on the
     * plg_tick task) mutates the command list and arenas while the UI task
     * replays them, so both sides must hold the same mutex. Null (default)
     * disables locking entirely for single-task use.
     */
    void setEditMutex(SemaphoreHandle_t mutex) { editMutex_ = mutex; }

    void setKeyCallback(KeyCallback cb)        { keyCb_ = cb; }
    void setWidgetCallback(WidgetCallback cb)  { widgetCb_ = cb; }
    /// Register a plugin long-press handler. Setting a non-null callback opts
    /// this canvas into deferred short-press keypad mode while it is active.
    void setLongPressCallback(LongPressCallback cb);
    void setFooter(const char* hint);
    void setKeyRepeat(uint16_t initial_ms, uint16_t repeat_ms);
    void getBodySize(uint16_t* w, uint16_t* h) const;

    /// Drop the display list, elements and their tweens. With `keep_sprites`
    /// the sprite assets (frames, masks, flags, current frame) survive; their
    /// playback is stopped so orphans cannot keep the animation clock busy.
    void clearBody(bool keep_sprites = false);
    void setTextSize(uint8_t size)             { textSize_ = size > 0 ? size : 1; }
    void setTextInverted(bool inverted)        { textInverted_ = inverted; }
    /// Select one of the canonical font ids (see cdc_views/Fonts.h).
    void setFontId(uint8_t font_id)            { fontId_ = font_id; }
    /// Fill ink for subsequent filled shapes: 0=none, 255=solid, between=dither.
    void setShade(uint8_t shade)               { shade_ = shade; }
    /// Draw subsequent shapes in white instead of black (eraser/wipes).
    void setInkWhite(bool white)               { inkWhite_ = white; }
    void drawText(int16_t x, int16_t y, const char* text);
    void drawTextAligned(int16_t x, int16_t y, int16_t w, const char* text, uint8_t align);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, bool filled);
    void drawPixel(int16_t x, int16_t y);
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1);
    void drawCircle(int16_t x, int16_t y, int16_t r, bool filled);
    void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, bool filled);
    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bool filled);
    /// Draw a 1-bpp bitmap (rows byte-padded, MSB first). Set bits draw black,
    /// unset bits are transparent. Pixel data is copied into the canvas arena.
    void drawBitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                    const uint8_t* data, uint32_t len);
    void drawHLine(int16_t x, int16_t y, int16_t w);
    void drawVLine(int16_t x, int16_t y, int16_t h);
    void commit(bool full_refresh);

    /// Start recording subsequent draw calls under element `id` (creates the
    /// element on first use). Elements are named groups of draw commands that
    /// can later be moved, hidden or removed without rebuilding the whole
    /// display list — the building block for animations.
    bool beginElem(uint32_t id);
    /// Stop tagging draw calls with an element id.
    void endElem();
    /// Set the element's replay offset relative to its recorded coordinates.
    bool elemSetOffset(uint32_t id, int16_t ox, int16_t oy);
    /// Shift the element's replay offset by a delta.
    bool elemMove(uint32_t id, int16_t dx, int16_t dy);
    /// Show or hide the element (hidden elements are skipped on replay).
    bool elemShow(uint32_t id, bool visible);
    /// Drop the element and all its draw commands; arena space is reclaimed.
    /// Tweens bound to the element are cancelled silently.
    bool elemRemove(uint32_t id);
    /// Drop only the element's draw commands, keeping the element (offset,
    /// visibility, z, running tweens) for re-recording in place.
    bool elemClear(uint32_t id);
    /// Set the element's replay layer (-128..127, default 0). Lower layers
    /// draw first; untagged commands draw in layer 0. Ties keep recording
    /// order.
    bool elemSetZ(uint32_t id, int8_t z);
    /// Read the element's current replay offset.
    bool elemGetOffset(uint32_t id, int16_t* ox, int16_t* oy) const;
    /// Bounding box of the element's recorded commands with its offset
    /// applied, in body-local pixels. False when unknown or empty.
    bool elemGetBounds(uint32_t id, int16_t* x, int16_t* y,
                       uint16_t* w, uint16_t* h);

    // --- Sprites (multi-frame 1-bpp resources, drawn by reference) ---------

    /// Copy a vertically stacked frame sheet into the sprite arena.
    /// \return Handle >= 1, 0 = invalid arguments, -1 = out of slots/arena.
    int32_t spriteCreate(uint16_t w, uint16_t h, uint16_t frame_count,
                         const uint8_t* data, uint32_t len);
    bool spriteSetMask(uint32_t handle, const uint8_t* mask, uint32_t len);
    bool spriteSetFlags(uint32_t handle, uint8_t flags);
    /// Integer upscale 1..4 applied whenever the sprite is drawn.
    bool spriteSetScale(uint32_t handle, uint8_t scale);
    bool spriteSetFrame(uint32_t handle, uint16_t frame);
    int32_t spriteGetFrame(uint32_t handle) const;
    bool spriteSetFrameDurations(uint32_t handle, const uint16_t* ms,
                                 uint16_t count);
    bool spritePlay(uint32_t handle, uint8_t mode, uint16_t frame_ms,
                    uint16_t repeat, uint32_t done_action_id);
    bool spriteStop(uint32_t handle);
    /// Destroy the sprite; recorded draw-sprite commands referencing it are
    /// skipped on replay from then on.
    bool spriteDestroy(uint32_t handle);
    /// Record a draw-by-reference of the sprite's current frame at (x, y).
    void drawSprite(int16_t x, int16_t y, uint32_t handle);
    /// Snapshot the sprite's current frame pixels (for stamping onto
    /// surfaces). Pointers stay valid until the next sprite mutation.
    bool spriteFrameView(uint32_t handle, anim::SpriteStore::FrameView* out) const;

    /**
     * Record a host-driven text marquee: the text is rendered once into an
     * internal sprite and a `window_w`-wide window scrolls through it
     * seamlessly (content + a window-sized blank gap). Uses the current
     * font/size. Returns the backing sprite handle (stop/destroy like any
     * sprite) or 0/-1 like spriteCreate.
     */
    int32_t marquee(int16_t x, int16_t y, int16_t window_w, const char* text,
                    uint16_t step_px, uint16_t frame_ms);

    // --- Tweens (host-driven element-offset animation) ---------------------

    /// \return Tween handle >= 1, or 0 on invalid config / full slots /
    ///         unknown element / unknown startAfter predecessor.
    uint32_t animStart(const anim::TweenConfig& cfg);
    /// Blink an element: `count` on/off cycles of `period_ms` per phase,
    /// ending visible. Occupies one tween slot.
    uint32_t animBlink(uint32_t elem_id, uint16_t period_ms, uint16_t count,
                       uint32_t done_action_id);
    bool animCancel(uint32_t handle);  ///< Handle 0 cancels all (no events).
    bool animPause(uint32_t handle, bool paused);
    int8_t animState(uint32_t handle) const;  ///< anim::STATE_* or -1.
    /// Live tweens plus playing sprites.
    uint8_t animActiveCount() const;
    void setAnimCallback(AnimCallback cb)      { animCb_ = cb; }
    /// Configure refresh policy and step-rate cap (0 = default 4 fps).
    bool setAnimPolicy(uint8_t policy, uint8_t max_fps);

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
    void onPause() override;
    void onExit() override;
    void onTick(uint32_t nowMs) override;
    const char* getName() const override       { return "CanvasView"; }

    /// Plugin free-draw surfaces accumulate churn; a single-flash FAST refresh
    /// gives them a mostly clean base without the full multi-flash waveform.
    hal::RefreshMode preferredEnterRefresh() const override { return hal::RefreshMode::FAST; }

    /// While animations run, partial refreshes must not escalate to a FAST
    /// mid-motion flash; ghost hygiene is handled by the anim policy instead.
    bool prefersLightRefresh() const override  { return animActive_; }

private:
    enum class CmdType : uint8_t {
        Text, TextAligned, Rect, HLine, VLine,
        Pixel, Line, Circle, Triangle, RoundRect, Bitmap, Sprite
    };

    struct DrawCmd {
        CmdType  type     = CmdType::Text;
        int16_t  x        = 0;
        int16_t  y        = 0;
        int16_t  w        = 0;   // width, or 2nd point x (Line/Triangle), or radius (Circle)
        int16_t  h        = 0;   // height, or 2nd point y (Line/Triangle)
        int16_t  x2       = 0;   // 3rd point x (Triangle), or corner radius (RoundRect)
        int16_t  y2       = 0;   // 3rd point y (Triangle)
        uint16_t strOff   = 0;   // byte offset into textArena_ (or blobArena_ for
                                 // Bitmap), or the sprite handle (Sprite)
        uint16_t strLen   = 0;   // string/blob length (text excludes NUL)
        uint32_t elemId   = 0;   // owning element (0 = untagged)
        uint8_t  align    = 0;
        uint8_t  fontId   = 0;
        uint8_t  textSize = 1;
        uint8_t  shade    = 255; // fill ink: 0=none, 255=solid; between = dither
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

    /// A named group of draw commands with a replay offset, visibility and
    /// z layer.
    struct Elem {
        uint32_t id     = 0;
        int16_t  ox     = 0;
        int16_t  oy     = 0;
        int8_t   z      = 0;
        bool     hidden = false;
    };

    Widget*       findWidget(uint32_t id);
    const Widget* findWidget(uint32_t id) const;
    Elem*         findElem(uint32_t id);
    const Elem*   findElem(uint32_t id) const;
    void          applyElemOffset(DrawCmd& c, int16_t ox, int16_t oy) const;
    void          compactArenas();
    void          pushCmd(DrawCmd& c);
    bool          dropElemCmds(uint32_t id);
    void          replayCmd(const DrawCmd& cmd);
    bool          cmdBounds(const DrawCmd& c, int16_t* x, int16_t* y,
                            uint16_t* w, uint16_t* h);
    bool          ensureSpriteArena();

    // anim::AnimTarget (called under editMutex_ from onTick/animStart paths).
    bool getElemOffset(uint32_t elemId, int16_t* ox, int16_t* oy) override;
    bool setElemOffset(uint32_t elemId, int16_t ox, int16_t oy) override;
    bool setElemVisible(uint32_t elemId, bool visible) override;
    Widget*       focusedWidget();
    InputResult   dispatchKeyToWidget(Widget& w, char key);
    void          t9_commit_pending(Widget& w);
    void          t9_apply_key(Widget& w, char key, uint32_t now);

    Gdey029T94* gfx() const;
    void        applyKeypadConfig();
    uint16_t    internText(const char* text, uint16_t* outLen);
    uint16_t    internBlob(const uint8_t* data, uint16_t len);
    void        allocArenas();
    void        replayDisplayList();
    void        paintText(int16_t x, int16_t y, int16_t w, const char* text,
                          uint8_t align, uint8_t fontId, uint8_t textSize,
                          bool inverted);
    int         bodyTop() const                { return headerHeight_; }
    int         bodyBottom() const;

    cdc::core::PsramUniquePtr<Widget> widgets_;
    uint8_t   widgetCount_ = 0;
    uint32_t  focused_     = 0;

    Elem      elems_[MAX_ELEMS] = {};
    uint8_t   elemCount_ = 0;
    uint32_t  curElem_   = 0;

    const char*       footer_ = nullptr;
    KeyCallback       keyCb_ = nullptr;
    WidgetCallback    widgetCb_ = nullptr;
    LongPressCallback longPressCb_ = nullptr;

    cdc::core::PsramUniquePtr<DrawCmd> cmds_;
    uint16_t  cmdCount_ = 0;
    cdc::core::PsramUniquePtr<char>    textArena_;
    uint16_t  textArenaUsed_ = 0;
    cdc::core::PsramUniquePtr<uint8_t> blobArena_;
    uint16_t  blobUsed_ = 0;
    bool      overflowLogged_ = false;

    uint8_t   textSize_ = 1;
    uint8_t   fontId_ = 0;
    uint8_t   shade_ = 255;
    bool      inkWhite_ = false;
    bool      textInverted_ = false;
    int       headerHeight_ = 18;
    bool      needsFullRefresh_ = true;

    uint16_t  keyRepeatInitialMs_ = 0;
    uint16_t  keyRepeatPeriodMs_  = 0;
    bool      headerDrawnOnce_    = false;

    // Optional, borrowed (not owned): serialises display-list access with a
    // cross-task writer. Null disables locking. See setEditMutex().
    SemaphoreHandle_t editMutex_ = nullptr;

    // --- Animation state ----------------------------------------------------
    anim::CanvasAnimator animator_;
    anim::SpriteStore    sprites_;
    cdc::core::PsramUniquePtr<uint8_t> spriteArena_;
    AnimCallback animCb_          = nullptr;
    uint8_t   animPolicy_         = ANIM_REFRESH_AUTO;
    uint16_t  animFrameIntervalMs_ = 1000 / ANIM_FPS_DEFAULT;
    uint32_t  lastAnimStepMs_     = 0;
    uint32_t  idleCleanupAtMs_    = 0;   // 0 = no cleanup pending
    uint32_t  pausedAtMs_         = 0;
    uint8_t   animCommitCount_    = 0;   // hygiene-FAST counter
    // Written under editMutex_ in onTick, read lock-free by the render task's
    // prefersLightRefresh(); a stale read only delays escalation by one frame.
    volatile bool animActive_     = false;
};

} // namespace cdc::ui
