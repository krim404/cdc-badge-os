/**
 * \file CanvasAnim.cpp
 * \brief Fixed-point easing, element-offset tween engine and sprite store for
 *        the plugin canvas. Dependency-free (see CanvasAnim.h).
 */

#include "cdc_views/CanvasAnim.h"

#include <cstring>

namespace cdc::ui::anim {

namespace {

/// 8.8 one.
constexpr int32_t ONE = 256;

int32_t quadIn(int32_t t) { return (t * t) >> 8; }
int32_t cubicIn(int32_t t) { return (((t * t) >> 8) * t) >> 8; }

/// Penner back-out with s = 1.70158 (435 in 8.8): 1 + (s+1)u^3 + s*u^2,
/// u = t - 1. Overshoots past ONE mid-curve by design.
int32_t backOut(int32_t t) {
    constexpr int32_t S = 435;
    int32_t u  = t - ONE;                    // -256..0
    int32_t u2 = (u * u) >> 8;
    int32_t u3 = (u2 * u) >> 8;
    return ONE + (((S + ONE) * u3) >> 8) + ((S * u2) >> 8);
}

/// Penner bounce-out, n1 = 7.5625 (1936 in 8.8), d1 = 2.75. The rounded
/// segment offsets can add up to a hair above ONE, hence the final clamp.
int32_t bounceOut(int32_t t) {
    int32_t v;
    if (t < 93) {                            // t < 1/2.75
        v = (1936 * t * t) >> 16;
    } else if (t < 186) {                    // t < 2/2.75
        int32_t u = t - 140;                 // 1.5/2.75
        v = ((1936 * u * u) >> 16) + 192;    // + 0.75
    } else if (t < 233) {                    // t < 2.5/2.75
        int32_t u = t - 209;                 // 2.25/2.75
        v = ((1936 * u * u) >> 16) + 240;    // + 0.9375
    } else {
        int32_t u = t - 244;                 // 2.625/2.75
        v = ((1936 * u * u) >> 16) + 252;    // + 0.984375
    }
    return v > ONE ? ONE : v;
}

/// Elastic-out sampled at 16 intervals (8.8), linearly interpolated. A sine
/// approximation in fixed point buys nothing over this at 5 fps.
const int16_t kElasticOut[17] = {
    0, 213, 349, 305, 233, 228, 256, 268, 260,
    252, 253, 257, 257, 256, 255, 256, 256,
};

int32_t elasticOut(int32_t t) {
    int32_t idx  = t >> 4;          // 0..15
    int32_t frac = t & 15;          // within the segment
    int32_t a    = kElasticOut[idx];
    int32_t b    = kElasticOut[idx + 1];
    return a + ((b - a) * frac) / 16;
}

}  // namespace

int32_t ease(uint8_t curve, int32_t t) {
    if (t <= 0) return 0;
    if (t >= ONE) return ONE;
    switch (curve) {
        case EASE_QUAD_IN:      return quadIn(t);
        case EASE_QUAD_OUT:     return ONE - quadIn(ONE - t);
        case EASE_QUAD_IN_OUT:  return t < ONE / 2 ? (t * t) >> 7
                                                   : ONE - (((ONE - t) * (ONE - t)) >> 7);
        case EASE_CUBIC_IN:     return cubicIn(t);
        case EASE_CUBIC_OUT:    return ONE - cubicIn(ONE - t);
        case EASE_CUBIC_IN_OUT: {
            if (t < ONE / 2) return (4 * t * t * t) >> 16;
            int32_t u = ONE - t;
            return ONE - ((4 * u * u * u) >> 16);
        }
        case EASE_OVERSHOOT:    return backOut(t);
        case EASE_BOUNCE:       return bounceOut(t);
        case EASE_ELASTIC:      return elasticOut(t);
        case EASE_STEP:         return 0;
        case EASE_LINEAR:
        default:                return t;
    }
}

// --- CanvasAnimator ----------------------------------------------------------

CanvasAnimator::Slot* CanvasAnimator::findSlot(uint32_t handle) {
    if (handle == 0) return nullptr;
    for (auto& s : slots_) {
        if (s.phase != Phase::Free && s.handle == handle) return &s;
    }
    return nullptr;
}

const CanvasAnimator::Slot* CanvasAnimator::findSlot(uint32_t handle) const {
    return const_cast<CanvasAnimator*>(this)->findSlot(handle);
}

CanvasAnimator::Slot* CanvasAnimator::allocSlot() {
    for (auto& s : slots_) {
        if (s.phase == Phase::Free) {
            s = Slot{};
            s.handle = nextHandle_++;
            if (nextHandle_ == 0) nextHandle_ = 1;
            return &s;
        }
    }
    return nullptr;
}

uint32_t CanvasAnimator::start(const TweenConfig& cfg, uint32_t nowMs) {
    if (cfg.elemId == 0 || cfg.durationMs == 0 || cfg.durationMs > MAX_DURATION_MS
        || cfg.delayMs > MAX_DURATION_MS || cfg.easing >= EASE_COUNT) {
        return 0;
    }
    if (cfg.startAfter != 0 && !findSlot(cfg.startAfter)) return 0;

    Slot* s = allocSlot();
    if (!s) return 0;
    s->elemId       = cfg.elemId;
    s->doneActionId = cfg.doneActionId;
    s->fromX        = cfg.fromX;
    s->fromY        = cfg.fromY;
    s->toX          = cfg.toX;
    s->toY          = cfg.toY;
    s->durationMs   = cfg.durationMs;
    s->delayMs      = cfg.delayMs;
    s->remaining    = cfg.repeat;
    s->easing       = cfg.easing;
    s->flags        = cfg.flags;
    s->kind         = Kind::Move;
    if (cfg.startAfter != 0) {
        s->startAfter = cfg.startAfter;
        s->phase      = Phase::Chained;
    } else {
        s->startMs = nowMs + cfg.delayMs;
        s->phase   = Phase::Delayed;
    }
    return s->handle;
}

uint32_t CanvasAnimator::blink(uint32_t elemId, uint16_t periodMs, uint16_t count,
                               uint32_t doneActionId, uint32_t nowMs) {
    if (elemId == 0 || periodMs == 0 || count == 0) return 0;
    Slot* s = allocSlot();
    if (!s) return 0;
    s->elemId       = elemId;
    s->doneActionId = doneActionId;
    s->durationMs   = periodMs;
    // Two visibility phases per blink; REPEAT_FOREVER stays forever.
    s->blinkCount   = (count == REPEAT_FOREVER)
                          ? REPEAT_FOREVER
                          : static_cast<uint16_t>(count * 2);
    s->kind         = Kind::Blink;
    s->phase        = Phase::Delayed;
    s->startMs      = nowMs;
    s->reversed     = false;  // false = currently visible
    return s->handle;
}

void CanvasAnimator::releaseChain(uint32_t handle) {
    for (auto& s : slots_) {
        if (s.phase == Phase::Chained && s.startAfter == handle) {
            uint32_t child = s.handle;
            s.phase = Phase::Free;
            releaseChain(child);
        }
    }
}

bool CanvasAnimator::cancel(uint32_t handle) {
    if (handle == 0) {
        bool any = false;
        for (auto& s : slots_) {
            any |= (s.phase != Phase::Free);
            s.phase = Phase::Free;
        }
        return any;
    }
    Slot* s = findSlot(handle);
    if (!s) return false;
    s->phase = Phase::Free;
    releaseChain(handle);
    return true;
}

void CanvasAnimator::cancelForElem(uint32_t elemId) {
    for (auto& s : slots_) {
        if (s.phase != Phase::Free && s.elemId == elemId) {
            uint32_t handle = s.handle;
            s.phase = Phase::Free;
            releaseChain(handle);
        }
    }
}

bool CanvasAnimator::pause(uint32_t handle, bool paused, uint32_t nowMs) {
    Slot* s = findSlot(handle);
    if (!s) return false;
    if (paused == s->paused) return true;
    if (paused) {
        s->pausedAtMs = nowMs;
        s->paused     = true;
    } else {
        s->startMs += nowMs - s->pausedAtMs;
        s->paused = false;
    }
    return true;
}

int8_t CanvasAnimator::state(uint32_t handle) const {
    const Slot* s = findSlot(handle);
    if (!s) return -1;
    if (s->paused) return STATE_PAUSED;
    if (s->phase == Phase::Running) return STATE_RUNNING;
    return STATE_DELAYED;
}

uint8_t CanvasAnimator::activeCount() const {
    uint8_t n = 0;
    for (const auto& s : slots_) {
        if (s.phase != Phase::Free) ++n;
    }
    return n;
}

void CanvasAnimator::shiftTime(uint32_t deltaMs) {
    for (auto& s : slots_) {
        if (s.phase == Phase::Delayed || s.phase == Phase::Running) {
            s.startMs += deltaMs;
        }
    }
}

void CanvasAnimator::reset() {
    for (auto& s : slots_) s.phase = Phase::Free;
}

void CanvasAnimator::finishSlot(Slot& s, AnimTarget& target, uint32_t nowMs,
                                Completion* done, uint8_t doneCap,
                                uint8_t* doneCount) {
    if (s.flags & FLAG_HIDE_DONE) {
        target.setElemVisible(s.elemId, false);
    }
    if (s.doneActionId != 0 && done && *doneCount < doneCap) {
        done[*doneCount].doneActionId = s.doneActionId;
        done[*doneCount].handle       = s.handle;
        done[*doneCount].refId        = s.elemId;
        ++*doneCount;
    }
    uint32_t handle = s.handle;
    s.phase = Phase::Free;
    // Wake chained successors.
    for (auto& c : slots_) {
        if (c.phase == Phase::Chained && c.startAfter == handle) {
            c.phase   = Phase::Delayed;
            c.startMs = nowMs + c.delayMs;
        }
    }
}

bool CanvasAnimator::advanceMove(Slot& s, uint32_t nowMs, AnimTarget& target) {
    if (static_cast<int32_t>(nowMs - s.startMs) < 0) return false;

    if (!s.started) {
        if (s.flags & FLAG_FROM_CURRENT) {
            int16_t ox = 0, oy = 0;
            if (target.getElemOffset(s.elemId, &ox, &oy)) {
                s.fromX = ox;
                s.fromY = oy;
            }
        }
        if (s.flags & FLAG_SHOW_START) {
            target.setElemVisible(s.elemId, true);
        }
        s.started = true;
        s.phase   = Phase::Running;
    }

    // Fold completed runs (repeat/yoyo) keeping the time base steady.
    uint32_t elapsed = nowMs - s.startMs;
    while (elapsed >= s.durationMs) {
        if (s.remaining == 0) {
            int16_t tx = s.reversed ? s.fromX : s.toX;
            int16_t ty = s.reversed ? s.fromY : s.toY;
            writeOffset(s, target, tx, ty);
            return true;  // caller finishes the slot
        }
        if (s.remaining != REPEAT_FOREVER) --s.remaining;
        if (s.flags & FLAG_YOYO) s.reversed = !s.reversed;
        s.startMs += s.durationMs;
        elapsed = nowMs - s.startMs;
    }

    int32_t t = static_cast<int32_t>((elapsed * 256u) / s.durationMs);
    int32_t e = ease(s.easing, t);
    int16_t fx = s.reversed ? s.toX : s.fromX;
    int16_t fy = s.reversed ? s.toY : s.fromY;
    int16_t tx = s.reversed ? s.fromX : s.toX;
    int16_t ty = s.reversed ? s.fromY : s.toY;
    int16_t x = static_cast<int16_t>(fx + ((static_cast<int32_t>(tx - fx) * e) >> 8));
    int16_t y = static_cast<int16_t>(fy + ((static_cast<int32_t>(ty - fy) * e) >> 8));
    writeOffset(s, target, x, y);
    return false;
}

// Write the eased offset, remembering the last value so advance() can report
// whether anything visible actually moved this tick.
bool CanvasAnimator::writeOffset(Slot& s, AnimTarget& target, int16_t x, int16_t y) {
    if (s.hasLast && s.lastX == x && s.lastY == y) return false;
    target.setElemOffset(s.elemId, x, y);
    s.lastX   = x;
    s.lastY   = y;
    s.hasLast = true;
    s.moved   = true;
    return true;
}

bool CanvasAnimator::advanceBlink(Slot& s, uint32_t nowMs, AnimTarget& target) {
    s.phase = Phase::Running;
    bool finished = false;
    while (static_cast<int32_t>(nowMs - (s.startMs + s.durationMs)) >= 0) {
        s.startMs += s.durationMs;
        s.reversed = !s.reversed;  // toggle visibility phase
        target.setElemVisible(s.elemId, !s.reversed);
        s.moved = true;
        if (s.blinkCount != REPEAT_FOREVER && --s.blinkCount == 0) {
            target.setElemVisible(s.elemId, true);
            finished = true;
            break;
        }
    }
    return finished;
}

bool CanvasAnimator::advance(uint32_t nowMs, AnimTarget& target,
                             Completion* done, uint8_t doneCap,
                             uint8_t* doneCount) {
    if (doneCount) *doneCount = 0;
    bool changed = false;
    for (auto& s : slots_) {
        if (s.phase == Phase::Free || s.phase == Phase::Chained || s.paused) {
            continue;
        }
        s.moved = false;
        bool finished = (s.kind == Kind::Move) ? advanceMove(s, nowMs, target)
                                               : advanceBlink(s, nowMs, target);
        changed |= s.moved;
        if (finished) finishSlot(s, target, nowMs, done, doneCap, doneCount);
    }
    return changed;
}

// --- SpriteStore -------------------------------------------------------------

void SpriteStore::setArena(uint8_t* buf, uint32_t cap) {
    arena_     = buf;
    arenaCap_  = buf ? cap : 0;
    arenaUsed_ = 0;
    for (auto& s : slots_) s = Slot{};
}

SpriteStore::Slot* SpriteStore::findSlot(uint32_t handle) {
    if (handle == 0) return nullptr;
    for (auto& s : slots_) {
        if (s.handle == handle) return &s;
    }
    return nullptr;
}

const SpriteStore::Slot* SpriteStore::findSlot(uint32_t handle) const {
    return const_cast<SpriteStore*>(this)->findSlot(handle);
}

uint32_t SpriteStore::frameBytes(const Slot& s) const {
    return static_cast<uint32_t>((s.w + 7) / 8) * s.h;
}

int32_t SpriteStore::arenaAlloc(uint32_t bytes) {
    if (!arena_ || arenaUsed_ + bytes > arenaCap_) return -1;
    uint32_t off = arenaUsed_;
    arenaUsed_ += bytes;
    return static_cast<int32_t>(off);
}

// Close the gap at [off, off+bytes) and re-point every block above it. The
// arena is append-allocated, so a single memmove keeps it compact.
void SpriteStore::arenaFree(uint32_t off, uint32_t bytes) {
    if (bytes == 0) return;
    memmove(arena_ + off, arena_ + off + bytes, arenaUsed_ - off - bytes);
    arenaUsed_ -= bytes;
    for (auto& s : slots_) {
        if (s.handle == 0) continue;
        if (s.sheetOff > off) s.sheetOff -= bytes;
        if (s.hasMask && s.maskOff > off) s.maskOff -= bytes;
        if (s.hasDur && s.durOff > off) s.durOff -= bytes;
    }
}

int32_t SpriteStore::create(uint16_t w, uint16_t h, uint16_t frameCount,
                            const uint8_t* data, uint32_t len) {
    if (w == 0 || h == 0 || frameCount == 0 || frameCount > MAX_FRAMES || !data) {
        return 0;
    }
    Slot* free = nullptr;
    for (auto& s : slots_) {
        if (s.handle == 0) {
            free = &s;
            break;
        }
    }
    if (!free) return -1;

    uint32_t stride = static_cast<uint32_t>((w + 7) / 8);
    uint32_t bytes  = stride * h * frameCount;
    if (bytes == 0 || len < bytes) return 0;
    int32_t off = arenaAlloc(bytes);
    if (off < 0) return -1;

    memcpy(arena_ + off, data, bytes);
    *free = Slot{};
    free->handle     = nextHandle_++;
    // Sprite handles stay 16-bit so display-list commands can reference them
    // in the existing u16 slot.
    if (nextHandle_ > 0xFFFF) nextHandle_ = 1;
    free->sheetOff   = static_cast<uint32_t>(off);
    free->bytes      = bytes;
    free->w          = w;
    free->h          = h;
    free->frameCount = frameCount;
    return static_cast<int32_t>(free->handle);
}

bool SpriteStore::setMask(uint32_t handle, const uint8_t* mask, uint32_t len) {
    Slot* s = findSlot(handle);
    if (!s || !mask || len < s->bytes) return false;
    if (!s->hasMask) {
        int32_t off = arenaAlloc(s->bytes);
        if (off < 0) return false;
        s->maskOff = static_cast<uint32_t>(off);
        s->hasMask = true;
    }
    memcpy(arena_ + s->maskOff, mask, s->bytes);
    return true;
}

bool SpriteStore::setFrameDurations(uint32_t handle, const uint16_t* ms,
                                    uint16_t count) {
    Slot* s = findSlot(handle);
    if (!s) return false;
    if (count == 0) {
        if (s->hasDur) {
            arenaFree(s->durOff, static_cast<uint32_t>(s->frameCount) * 2);
            s->hasDur = false;
        }
        return true;
    }
    if (!ms || count != s->frameCount) return false;
    if (!s->hasDur) {
        int32_t off = arenaAlloc(static_cast<uint32_t>(count) * 2);
        if (off < 0) return false;
        s->durOff = static_cast<uint32_t>(off);
        s->hasDur = true;
    }
    memcpy(arena_ + s->durOff, ms, static_cast<uint32_t>(count) * 2);
    return true;
}

bool SpriteStore::destroy(uint32_t handle) {
    Slot* s = findSlot(handle);
    if (!s) return false;
    // Free the highest block first so lower offsets stay valid in between.
    struct Block { uint32_t off, bytes; };
    Block blocks[3];
    int   n = 0;
    blocks[n++] = {s->sheetOff, s->bytes};
    if (s->hasMask) blocks[n++] = {s->maskOff, s->bytes};
    if (s->hasDur) blocks[n++] = {s->durOff, static_cast<uint32_t>(s->frameCount) * 2};
    *s = Slot{};
    for (int i = 0; i < n; ++i) {
        int hi = 0;
        for (int j = 1; j < n; ++j) {
            if (blocks[j].off > blocks[hi].off) hi = j;
        }
        arenaFree(blocks[hi].off, blocks[hi].bytes);
        blocks[hi].off   = 0;
        blocks[hi].bytes = 0;
    }
    return true;
}

bool SpriteStore::setFlags(uint32_t handle, uint8_t flags) {
    Slot* s = findSlot(handle);
    if (!s) return false;
    s->flags = flags;
    return true;
}

bool SpriteStore::setScale(uint32_t handle, uint8_t scale) {
    Slot* s = findSlot(handle);
    if (!s || scale == 0 || scale > 4) return false;
    s->scale = scale;
    return true;
}

bool SpriteStore::scroll(uint32_t handle, uint16_t windowW, uint16_t stepPx,
                         uint16_t gapPx, uint16_t frameMs, uint32_t nowMs) {
    Slot* s = findSlot(handle);
    if (!s || windowW == 0 || stepPx == 0) return false;
    s->scrollWindow = windowW;
    s->scrollStep   = stepPx;
    s->scrollGap    = gapPx;
    s->scrollX      = 0;
    s->frameMs      = frameMs < 50 ? 50 : frameMs;
    s->mode         = SPRITE_LOOP;
    s->remaining    = REPEAT_FOREVER;
    s->doneActionId = 0;
    s->playing      = true;
    s->nextFrameAt  = nowMs + s->frameMs;
    return true;
}

bool SpriteStore::setFrame(uint32_t handle, uint16_t frame) {
    Slot* s = findSlot(handle);
    if (!s || frame >= s->frameCount) return false;
    s->curFrame = frame;
    return true;
}

int32_t SpriteStore::frame(uint32_t handle) const {
    const Slot* s = findSlot(handle);
    return s ? s->curFrame : -1;
}

uint16_t SpriteStore::frameDuration(const Slot& s) const {
    if (s.hasDur) {
        uint16_t ms;
        memcpy(&ms, arena_ + s.durOff + static_cast<uint32_t>(s.curFrame) * 2,
               sizeof(ms));
        if (ms > 0) return ms;
    }
    return s.frameMs;
}

bool SpriteStore::play(uint32_t handle, uint8_t mode, uint16_t frameMs,
                       uint16_t repeat, uint32_t doneActionId, uint32_t nowMs) {
    Slot* s = findSlot(handle);
    if (!s || mode > SPRITE_PING_PONG) return false;
    // Floor the pace at 50 ms; anything faster is beyond the panel anyway and
    // would busy-spin the catch-up loop.
    s->frameMs      = frameMs < 50 ? 50 : frameMs;
    s->mode         = mode;
    s->remaining    = repeat;
    s->doneActionId = doneActionId;
    s->curFrame     = 0;
    s->dir          = 1;
    s->playing      = true;
    s->nextFrameAt  = nowMs + frameDuration(*s);
    return true;
}

bool SpriteStore::stop(uint32_t handle) {
    Slot* s = findSlot(handle);
    if (!s) return false;
    s->playing = false;
    return true;
}

void SpriteStore::stopAll() {
    for (auto& s : slots_) {
        if (s.handle != 0) s.playing = false;
    }
}

bool SpriteStore::anyPlaying() const {
    for (const auto& s : slots_) {
        if (s.handle != 0 && s.playing) return true;
    }
    return false;
}

void SpriteStore::shiftTime(uint32_t deltaMs) {
    for (auto& s : slots_) {
        if (s.handle != 0 && s.playing) s.nextFrameAt += deltaMs;
    }
}

void SpriteStore::reset() {
    arenaUsed_ = 0;
    for (auto& s : slots_) s = Slot{};
}

bool SpriteStore::advance(uint32_t nowMs, Completion* done, uint8_t doneCap,
                          uint8_t* doneCount) {
    bool changed = false;
    for (auto& s : slots_) {
        if (s.handle == 0 || !s.playing) continue;
        if (s.scrollWindow != 0) {
            // Marquee: slide the window, wrapping over content + gap.
            uint32_t span = static_cast<uint32_t>(s.w) + s.scrollGap;
            while (static_cast<int32_t>(nowMs - s.nextFrameAt) >= 0) {
                s.scrollX = static_cast<uint16_t>((s.scrollX + s.scrollStep) % span);
                s.nextFrameAt += s.frameMs;
                changed = true;
            }
            continue;
        }
        while (s.playing && static_cast<int32_t>(nowMs - s.nextFrameAt) >= 0) {
            bool cycleDone = false;
            if (s.frameCount == 1) {
                cycleDone = true;
            } else if (s.mode == SPRITE_PING_PONG) {
                int32_t next = s.curFrame + s.dir;
                if (next < 0) {
                    // Bounce at the start: one there-and-back completed. The
                    // next cycle's first step happens in the same beat, so a
                    // continuing loop shows ...2 1 0 1 2... without doubling
                    // the end frames.
                    s.dir     = 1;
                    next      = 1;
                    cycleDone = true;
                } else if (next >= s.frameCount) {
                    s.dir = -1;
                    next  = s.frameCount - 2;
                }
                s.curFrame = static_cast<uint16_t>(next);
                changed    = true;
            } else {
                uint16_t next = s.curFrame + 1;
                if (next >= s.frameCount) {
                    cycleDone = true;  // wrap (LOOP) or hold (ONCE) decided below
                } else {
                    s.curFrame = next;
                    changed    = true;
                }
            }

            if (cycleDone) {
                bool moreRuns = (s.remaining == REPEAT_FOREVER) || (s.remaining > 0);
                if (s.mode == SPRITE_ONCE) moreRuns = false;
                if (moreRuns) {
                    if (s.remaining != REPEAT_FOREVER) --s.remaining;
                    if (s.mode == SPRITE_LOOP && s.frameCount > 1) {
                        s.curFrame = 0;
                        changed    = true;
                    }
                } else {
                    // ONCE and exhausted LOOP hold the last frame; a finished
                    // PING_PONG rests where it bounced, at frame 0.
                    if (s.mode == SPRITE_PING_PONG) s.curFrame = 0;
                    s.playing = false;
                    if (s.doneActionId != 0 && done && doneCount
                        && *doneCount < doneCap) {
                        done[*doneCount].doneActionId = s.doneActionId;
                        done[*doneCount].handle       = s.handle;
                        done[*doneCount].refId        = s.curFrame;
                        ++*doneCount;
                    }
                    break;
                }
            }
            s.nextFrameAt += frameDuration(s);
        }
    }
    return changed;
}

bool SpriteStore::frameView(uint32_t handle, FrameView* out) const {
    const Slot* s = findSlot(handle);
    if (!s || !out || !arena_) return false;
    uint32_t fb = frameBytes(*s);
    out->data         = arena_ + s->sheetOff + fb * s->curFrame;
    out->mask         = s->hasMask ? arena_ + s->maskOff + fb * s->curFrame : nullptr;
    out->w            = s->w;
    out->h            = s->h;
    out->flags        = s->flags;
    out->scale        = s->scale;
    out->scrollX      = s->scrollX;
    out->scrollWindow = s->scrollWindow;
    out->scrollGap    = s->scrollGap;
    return true;
}

}  // namespace cdc::ui::anim
