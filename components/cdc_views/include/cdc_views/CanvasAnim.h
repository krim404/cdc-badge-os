#pragma once

#include <cstddef>
#include <cstdint>

/**
 * \file CanvasAnim.h
 * \brief Host-side animation engine for the plugin canvas: fixed-point easing,
 *        element-offset tweens and multi-frame sprite playback.
 *
 * Pure logic with no ESP-IDF, FreeRTOS or display dependencies: time is passed
 * in as milliseconds, results are applied through the AnimTarget interface and
 * the sprite pixel arena is injected by the owner. This keeps the engine unit
 * testable in the native PlatformIO environment (see test/host).
 */

namespace cdc::ui::anim {

/// Easing curves. Values mirror the HOST_EASE_* ids in host_api.h.
enum : uint8_t {
    EASE_LINEAR       = 0,
    EASE_QUAD_IN      = 1,
    EASE_QUAD_OUT     = 2,
    EASE_QUAD_IN_OUT  = 3,
    EASE_CUBIC_IN     = 4,
    EASE_CUBIC_OUT    = 5,
    EASE_CUBIC_IN_OUT = 6,
    EASE_OVERSHOOT    = 7,
    EASE_BOUNCE       = 8,
    EASE_STEP         = 9,
    EASE_ELASTIC      = 10,
    EASE_COUNT        = 11,
};

/// Tween flags. Values mirror the HOST_ANIM_FLAG_* ids in host_api.h.
enum : uint8_t {
    FLAG_FROM_CURRENT = 0x01,  ///< Ignore from_x/y, start at the live offset.
    FLAG_YOYO         = 0x02,  ///< Each repeat run alternates direction.
    FLAG_HIDE_DONE    = 0x04,  ///< Hide the element on completion.
    FLAG_SHOW_START   = 0x08,  ///< Show the element when the delay elapses.
};

/// Tween/playback states as reported by CanvasAnimator::state().
enum : int8_t {
    STATE_DELAYED = 0,
    STATE_RUNNING = 1,
    STATE_PAUSED  = 2,
};

/// Sprite playback modes. Values mirror the HOST_SPRITE_* ids in host_api.h.
enum : uint8_t {
    SPRITE_ONCE      = 0,  ///< Play to the last frame and hold it.
    SPRITE_LOOP      = 1,
    SPRITE_PING_PONG = 2,
};

/// Sprite flags. Values mirror the HOST_SPRITE_FLAG_* ids in host_api.h.
enum : uint8_t {
    SPRITE_FLAG_OPAQUE = 0x01,  ///< Unset data bits paint white.
    SPRITE_FLAG_FLIP_H = 0x02,
    SPRITE_FLAG_FLIP_V = 0x04,
    SPRITE_FLAG_ROT_90 = 0x08,  ///< Rotate 90 deg clockwise (before flips).
};

/// Repeat-forever sentinel shared by tweens and sprite playback.
constexpr uint16_t REPEAT_FOREVER = 0xFFFF;

/// A finished tween or sprite playback to be reported to the plugin after the
/// canvas lock is released.
struct Completion {
    uint32_t doneActionId = 0;
    uint32_t handle       = 0;  ///< Tween handle or sprite handle.
    uint32_t refId        = 0;  ///< Element id (tween) or final frame (sprite).
};

/// Interface the animator drives; implemented by CanvasView on its elements.
class AnimTarget {
public:
    virtual ~AnimTarget() = default;
    virtual bool getElemOffset(uint32_t elemId, int16_t* ox, int16_t* oy) = 0;
    virtual bool setElemOffset(uint32_t elemId, int16_t ox, int16_t oy)   = 0;
    virtual bool setElemVisible(uint32_t elemId, bool visible)            = 0;
};

/**
 * \brief Apply an easing curve to a normalized 8.8 fixed-point time.
 * \param curve One of the EASE_* ids (out of range falls back to linear).
 * \param t     Normalized time 0..256.
 * \return Eased progress in 8.8 (0..256; OVERSHOOT exceeds 256 mid-curve).
 */
int32_t ease(uint8_t curve, int32_t t);

/// Tween start configuration (mirrors host_anim_t plus the blink shorthand).
struct TweenConfig {
    uint32_t elemId       = 0;
    int16_t  fromX        = 0;
    int16_t  fromY        = 0;
    int16_t  toX          = 0;
    int16_t  toY          = 0;
    uint16_t durationMs   = 0;
    uint16_t delayMs      = 0;
    uint16_t repeat       = 0;  ///< Extra runs; REPEAT_FOREVER = endless.
    uint8_t  easing       = EASE_LINEAR;
    uint8_t  flags        = 0;
    uint32_t doneActionId = 0;
    uint32_t startAfter   = 0;  ///< Predecessor tween handle; 0 = start now.
};

/**
 * \brief Fixed-slot tween engine animating element offsets (plus a blink
 *        convenience that toggles visibility).
 *
 * Handles are host-assigned, non-zero and unique until the slot is recycled
 * after completion/cancel. Chained tweens (startAfter) wait for their
 * predecessor; cancelling a predecessor silently cancels its whole chain.
 */
class CanvasAnimator {
public:
    static constexpr uint8_t  MAX_TWEENS      = 16;
    static constexpr uint16_t MAX_DURATION_MS = 60000;

    /**
     * \brief Start an offset tween. A FROM_CURRENT start captures the live
     *        offset lazily on the first advance() after the delay elapses.
     * \return Handle >= 1, or 0 when the config is invalid / no slot is free /
     *         the startAfter predecessor does not exist.
     */
    uint32_t start(const TweenConfig& cfg, uint32_t nowMs);

    /**
     * \brief Start a visibility blink: `count` on/off cycles of `periodMs` per
     *        phase, ending visible. Occupies one tween slot.
     */
    uint32_t blink(uint32_t elemId, uint16_t periodMs, uint16_t count,
                   uint32_t doneActionId, uint32_t nowMs);

    /// Cancel a tween and its chain (handle 0 = all). No completion fires.
    bool cancel(uint32_t handle);
    /// Silently cancel every tween bound to an element (element was removed).
    void cancelForElem(uint32_t elemId);
    bool pause(uint32_t handle, bool paused, uint32_t nowMs);
    /// \return STATE_* or -1 when the handle is unknown/finished.
    int8_t state(uint32_t handle) const;
    /// Number of live slots (delayed, chained, running or paused).
    uint8_t activeCount() const;
    /// Shift every time base forward (view was paused for deltaMs).
    void shiftTime(uint32_t deltaMs);
    /// Drop all slots without firing completions (teardown).
    void reset();

    /**
     * \brief Advance all tweens to nowMs, applying offsets/visibility.
     * \param done      Out array for finished tweens (completions with a
     *                  doneActionId only).
     * \param doneCap   Capacity of \p done.
     * \param doneCount Out: number of entries written.
     * \return true when any element offset or visibility changed.
     */
    bool advance(uint32_t nowMs, AnimTarget& target,
                 Completion* done, uint8_t doneCap, uint8_t* doneCount);

private:
    enum class Kind : uint8_t { Move, Blink };
    enum class Phase : uint8_t { Free, Chained, Delayed, Running };

    struct Slot {
        uint32_t handle       = 0;
        uint32_t elemId       = 0;
        uint32_t doneActionId = 0;
        uint32_t startAfter   = 0;   // predecessor handle while Chained
        uint32_t startMs      = 0;   // run start (delay already applied)
        int16_t  fromX = 0, fromY = 0, toX = 0, toY = 0;
        uint16_t durationMs   = 0;
        uint16_t delayMs      = 0;
        uint16_t remaining    = 0;   // remaining extra runs
        uint16_t blinkCount   = 0;   // remaining blink phases (Kind::Blink)
        uint32_t pausedAtMs   = 0;
        uint8_t  easing       = EASE_LINEAR;
        uint8_t  flags        = 0;
        int16_t  lastX = 0, lastY = 0;  // last offset written to the target
        Kind     kind         = Kind::Move;
        Phase    phase        = Phase::Free;
        bool     paused       = false;
        bool     reversed     = false;  // current yoyo direction / blink phase
        bool     started      = false;  // FROM_CURRENT/SHOW_START applied
        bool     hasLast      = false;
        bool     moved        = false;  // wrote a visible change this tick
    };

    Slot*       findSlot(uint32_t handle);
    const Slot* findSlot(uint32_t handle) const;
    Slot*       allocSlot();
    void        releaseChain(uint32_t handle);
    void        finishSlot(Slot& s, AnimTarget& target, uint32_t nowMs,
                           Completion* done, uint8_t doneCap, uint8_t* doneCount);
    bool        advanceMove(Slot& s, uint32_t nowMs, AnimTarget& target);
    bool        advanceBlink(Slot& s, uint32_t nowMs, AnimTarget& target);
    bool        writeOffset(Slot& s, AnimTarget& target, int16_t x, int16_t y);

    Slot     slots_[MAX_TWEENS];
    uint32_t nextHandle_ = 1;
};

/**
 * \brief Fixed-slot store for multi-frame 1-bpp sprites with host-driven
 *        playback. Pixel data lives in an injected arena (PSRAM, owner-held).
 *
 * Sheet layout: frames stacked vertically, rows byte-padded, MSB first, set
 * bit = black (the surface/QR/image convention). An optional mask plane uses
 * the same layout; mask bit set = pixel painted.
 */
class SpriteStore {
public:
    static constexpr uint8_t  MAX_SPRITES = 8;
    static constexpr uint16_t MAX_FRAMES  = 32;

    /// Inject the pixel arena. Must happen before create(); resets the store.
    void setArena(uint8_t* buf, uint32_t cap);
    bool hasArena() const { return arena_ != nullptr; }

    /**
     * \brief Copy a frame sheet into the arena and allocate a sprite slot.
     * \return Handle >= 1, 0 = invalid geometry, -1 = out of slots/arena.
     */
    int32_t create(uint16_t w, uint16_t h, uint16_t frameCount,
                   const uint8_t* data, uint32_t len);
    /// Attach a mask plane (same sheet layout/size as the frame data).
    bool setMask(uint32_t handle, const uint8_t* mask, uint32_t len);
    /// Optional per-frame durations; count must equal frameCount (0 reverts
    /// to the global frame duration passed to play()).
    bool setFrameDurations(uint32_t handle, const uint16_t* ms, uint16_t count);
    bool destroy(uint32_t handle);

    bool setFlags(uint32_t handle, uint8_t flags);
    /// Integer upscale factor 1..4 applied when the sprite is drawn.
    bool setScale(uint32_t handle, uint8_t scale);
    bool setFrame(uint32_t handle, uint16_t frame);
    int32_t frame(uint32_t handle) const;  ///< Current frame or -1.

    /**
     * \brief Turn the sprite into a horizontal scroller (marquee backing).
     *
     * The renderer shows a `windowW`-wide window into the (single-frame)
     * content; advance() slides it by `stepPx` per beat, wrapping seamlessly
     * with `gapPx` of blank space between repetitions.
     */
    bool scroll(uint32_t handle, uint16_t windowW, uint16_t stepPx,
                uint16_t gapPx, uint16_t frameMs, uint32_t nowMs);

    bool play(uint32_t handle, uint8_t mode, uint16_t frameMs, uint16_t repeat,
              uint32_t doneActionId, uint32_t nowMs);
    bool stop(uint32_t handle);
    /// Stop playback on every sprite, keeping assets and frame indices
    /// (a keep-sprites canvas clear must not leave orphans driving the
    /// animation clock).
    void stopAll();
    bool anyPlaying() const;
    void shiftTime(uint32_t deltaMs);
    void reset();

    /// Advance playback; fills completions for finished ONCE/finite runs.
    /// \return true when any visible frame index changed.
    bool advance(uint32_t nowMs, Completion* done, uint8_t doneCap,
                 uint8_t* doneCount);

    /// Replay accessors: geometry, flags and pixel pointers of a sprite.
    struct FrameView {
        const uint8_t* data = nullptr;  ///< Current frame pixels.
        const uint8_t* mask = nullptr;  ///< Mask plane or null.
        uint16_t w = 0, h = 0;
        uint8_t  flags = 0;
        uint8_t  scale = 1;
        uint16_t scrollX = 0;   ///< Scroll position (scrollWindow > 0).
        uint16_t scrollWindow = 0;  ///< Window width; 0 = not scrolling.
        uint16_t scrollGap = 0;     ///< Blank gap between wraps.
    };
    bool frameView(uint32_t handle, FrameView* out) const;

private:
    struct Slot {
        uint32_t handle       = 0;   // 0 = free
        uint32_t sheetOff     = 0;
        uint32_t maskOff      = 0;
        uint32_t durOff       = 0;
        uint32_t bytes        = 0;   // arena bytes at sheetOff (sheet only)
        uint32_t doneActionId = 0;
        uint32_t nextFrameAt  = 0;
        uint16_t w = 0, h = 0;
        uint16_t frameCount   = 0;
        uint16_t curFrame     = 0;
        uint16_t frameMs      = 0;
        uint16_t remaining    = 0;   // remaining extra cycles
        uint16_t scrollX      = 0;   // marquee position
        uint16_t scrollWindow = 0;   // marquee window width (0 = frames mode)
        uint16_t scrollStep   = 0;   // px per beat
        uint16_t scrollGap    = 0;   // blank px between wraps
        uint8_t  mode         = SPRITE_ONCE;
        uint8_t  flags        = 0;
        uint8_t  scale        = 1;
        bool     playing      = false;
        bool     hasMask      = false;
        bool     hasDur       = false;
        int8_t   dir          = 1;   // ping-pong direction
    };

    Slot*       findSlot(uint32_t handle);
    const Slot* findSlot(uint32_t handle) const;
    uint32_t    frameBytes(const Slot& s) const;
    uint16_t    frameDuration(const Slot& s) const;
    int32_t     arenaAlloc(uint32_t bytes);
    void        arenaFree(uint32_t off, uint32_t bytes);

    Slot     slots_[MAX_SPRITES];
    uint8_t* arena_      = nullptr;
    uint32_t arenaCap_   = 0;
    uint32_t arenaUsed_  = 0;
    uint32_t nextHandle_ = 1;
};

}  // namespace cdc::ui::anim
