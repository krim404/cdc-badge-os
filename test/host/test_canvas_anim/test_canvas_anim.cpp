/**
 * \file
 * \brief Host unit tests for the canvas animation engine (easing, tweens,
 *        blink, sprite store). Includes the real implementation, which is
 *        dependency-free by design.
 */

#include "../../../components/cdc_views/src/CanvasAnim.cpp"

#include <cstring>
#include <unity.h>

using namespace cdc::ui::anim;

namespace {

/// Records the last applied offsets/visibility per element id (tiny fake).
struct FakeTarget : AnimTarget {
    static constexpr int MAX = 8;
    uint32_t ids[MAX]     = {0};
    int16_t  x[MAX]       = {0};
    int16_t  y[MAX]       = {0};
    bool     visible[MAX] = {true, true, true, true, true, true, true, true};

    int slot(uint32_t id) {
        for (int i = 0; i < MAX; ++i) {
            if (ids[i] == id) return i;
        }
        for (int i = 0; i < MAX; ++i) {
            if (ids[i] == 0) {
                ids[i] = id;
                return i;
            }
        }
        return 0;
    }
    bool getElemOffset(uint32_t id, int16_t* ox, int16_t* oy) override {
        int i = slot(id);
        *ox = x[i];
        *oy = y[i];
        return true;
    }
    bool setElemOffset(uint32_t id, int16_t ox, int16_t oy) override {
        int i = slot(id);
        x[i] = ox;
        y[i] = oy;
        return true;
    }
    bool setElemVisible(uint32_t id, bool v) override {
        visible[slot(id)] = v;
        return true;
    }
};

Completion done[8];
uint8_t doneCount = 0;

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// --- Easing -------------------------------------------------------------

static void test_easing_endpoints(void) {
    for (uint8_t c = 0; c < EASE_COUNT; ++c) {
        TEST_ASSERT_EQUAL_INT32(0, ease(c, 0));
        TEST_ASSERT_EQUAL_INT32(256, ease(c, 256));
    }
}

static void test_easing_midpoints(void) {
    TEST_ASSERT_EQUAL_INT32(128, ease(EASE_LINEAR, 128));
    TEST_ASSERT_EQUAL_INT32(64, ease(EASE_QUAD_IN, 128));    // 0.5^2 = 0.25
    TEST_ASSERT_EQUAL_INT32(192, ease(EASE_QUAD_OUT, 128));  // 1-0.25 = 0.75
    TEST_ASSERT_EQUAL_INT32(128, ease(EASE_QUAD_IN_OUT, 128));
    TEST_ASSERT_EQUAL_INT32(32, ease(EASE_CUBIC_IN, 128));   // 0.5^3 = 0.125
    TEST_ASSERT_EQUAL_INT32(0, ease(EASE_STEP, 255));
    // Monotone-ish sanity: quad-in stays below linear before the end.
    TEST_ASSERT_LESS_THAN_INT32(200, ease(EASE_QUAD_IN, 200));
}

static void test_easing_overshoot_exceeds_one(void) {
    bool exceeded = false;
    for (int32_t t = 0; t <= 256; ++t) {
        if (ease(EASE_OVERSHOOT, t) > 256) exceeded = true;
    }
    TEST_ASSERT_TRUE(exceeded);
}

static void test_easing_bounce_in_range(void) {
    for (int32_t t = 0; t <= 256; ++t) {
        int32_t e = ease(EASE_BOUNCE, t);
        TEST_ASSERT_GREATER_OR_EQUAL_INT32(0, e);
        TEST_ASSERT_LESS_OR_EQUAL_INT32(256, e);
    }
}

// --- Tweens ---------------------------------------------------------------

static void test_tween_linear_progress_and_completion(void) {
    CanvasAnimator a;
    FakeTarget t;
    TweenConfig cfg;
    cfg.elemId       = 7;
    cfg.toX          = 100;
    cfg.toY          = 50;
    cfg.durationMs   = 1000;
    cfg.doneActionId = 42;
    uint32_t h = a.start(cfg, 0);
    TEST_ASSERT_NOT_EQUAL(0, h);
    TEST_ASSERT_EQUAL_INT8(STATE_DELAYED, a.state(h));

    a.advance(500, t, done, 8, &doneCount);
    TEST_ASSERT_EQUAL_INT16(50, t.x[t.slot(7)]);
    TEST_ASSERT_EQUAL_INT16(25, t.y[t.slot(7)]);
    TEST_ASSERT_EQUAL_INT8(STATE_RUNNING, a.state(h));
    TEST_ASSERT_EQUAL_UINT8(0, doneCount);

    a.advance(1000, t, done, 8, &doneCount);
    TEST_ASSERT_EQUAL_INT16(100, t.x[t.slot(7)]);
    TEST_ASSERT_EQUAL_INT16(50, t.y[t.slot(7)]);
    TEST_ASSERT_EQUAL_UINT8(1, doneCount);
    TEST_ASSERT_EQUAL_UINT32(42, done[0].doneActionId);
    TEST_ASSERT_EQUAL_UINT32(h, done[0].handle);
    TEST_ASSERT_EQUAL_UINT32(7, done[0].refId);
    TEST_ASSERT_EQUAL_INT8(-1, a.state(h));
    TEST_ASSERT_EQUAL_UINT8(0, a.activeCount());
}

static void test_tween_delay_and_from_current(void) {
    CanvasAnimator a;
    FakeTarget t;
    t.setElemOffset(3, 20, 20);
    TweenConfig cfg;
    cfg.elemId     = 3;
    cfg.toX        = 40;
    cfg.toY        = 20;
    cfg.durationMs = 100;
    cfg.delayMs    = 200;
    cfg.flags      = FLAG_FROM_CURRENT;
    uint32_t h = a.start(cfg, 0);
    TEST_ASSERT_NOT_EQUAL(0, h);

    a.advance(100, t, done, 8, &doneCount);          // still delayed
    TEST_ASSERT_EQUAL_INT16(20, t.x[t.slot(3)]);
    a.advance(250, t, done, 8, &doneCount);          // halfway: 20 -> 40
    TEST_ASSERT_EQUAL_INT16(30, t.x[t.slot(3)]);
    a.advance(300, t, done, 8, &doneCount);
    TEST_ASSERT_EQUAL_INT16(40, t.x[t.slot(3)]);
}

static void test_tween_yoyo_repeat(void) {
    CanvasAnimator a;
    FakeTarget t;
    TweenConfig cfg;
    cfg.elemId     = 1;
    cfg.toX        = 100;
    cfg.durationMs = 100;
    cfg.repeat     = 1;   // one extra run
    cfg.flags      = FLAG_YOYO;
    uint32_t h = a.start(cfg, 0);
    TEST_ASSERT_NOT_EQUAL(0, h);

    a.advance(50, t, done, 8, &doneCount);   // forward halfway
    TEST_ASSERT_EQUAL_INT16(50, t.x[t.slot(1)]);
    a.advance(150, t, done, 8, &doneCount);  // second run, reversed, halfway
    TEST_ASSERT_EQUAL_INT16(50, t.x[t.slot(1)]);
    TEST_ASSERT_EQUAL_UINT8(0, doneCount);
    a.advance(200, t, done, 8, &doneCount);  // done, back at start
    TEST_ASSERT_EQUAL_INT16(0, t.x[t.slot(1)]);
    TEST_ASSERT_EQUAL_UINT8(0, a.activeCount());
}

static void test_tween_chain_and_cancel_propagation(void) {
    CanvasAnimator a;
    FakeTarget t;
    TweenConfig first;
    first.elemId     = 1;
    first.toX        = 10;
    first.durationMs = 100;
    uint32_t h1 = a.start(first, 0);

    TweenConfig second;
    second.elemId     = 2;
    second.toX        = 20;
    second.durationMs = 100;
    second.startAfter = h1;
    uint32_t h2 = a.start(second, 0);
    TEST_ASSERT_NOT_EQUAL(0, h2);

    a.advance(50, t, done, 8, &doneCount);
    TEST_ASSERT_EQUAL_INT16(0, t.x[t.slot(2)]);   // chained, not started
    a.advance(100, t, done, 8, &doneCount);       // first finishes
    a.advance(150, t, done, 8, &doneCount);       // second halfway
    TEST_ASSERT_EQUAL_INT16(10, t.x[t.slot(2)]);
    a.advance(200, t, done, 8, &doneCount);
    TEST_ASSERT_EQUAL_INT16(20, t.x[t.slot(2)]);

    // Cancelling a predecessor kills the whole chain.
    uint32_t h3 = a.start(first, 1000);
    second.startAfter = h3;
    uint32_t h4 = a.start(second, 1000);
    TEST_ASSERT_NOT_EQUAL(0, h4);
    TEST_ASSERT_TRUE(a.cancel(h3));
    TEST_ASSERT_EQUAL_INT8(-1, a.state(h4));
    TEST_ASSERT_EQUAL_UINT8(0, a.activeCount());
}

static void test_tween_pause_resume(void) {
    CanvasAnimator a;
    FakeTarget t;
    TweenConfig cfg;
    cfg.elemId     = 5;
    cfg.toX        = 100;
    cfg.durationMs = 100;
    uint32_t h = a.start(cfg, 0);
    a.advance(50, t, done, 8, &doneCount);
    TEST_ASSERT_EQUAL_INT16(50, t.x[t.slot(5)]);
    a.pause(h, true, 50);
    a.advance(500, t, done, 8, &doneCount);   // frozen
    TEST_ASSERT_EQUAL_INT16(50, t.x[t.slot(5)]);
    a.pause(h, false, 500);
    a.advance(525, t, done, 8, &doneCount);   // resumes where it left off
    TEST_ASSERT_EQUAL_INT16(75, t.x[t.slot(5)]);
}

static void test_blink_toggles_and_ends_visible(void) {
    CanvasAnimator a;
    FakeTarget t;
    uint32_t h = a.blink(4, 100, 2, 9, 0);  // two blinks = four phases
    TEST_ASSERT_NOT_EQUAL(0, h);
    a.advance(100, t, done, 8, &doneCount);
    TEST_ASSERT_FALSE(t.visible[t.slot(4)]);
    a.advance(200, t, done, 8, &doneCount);
    TEST_ASSERT_TRUE(t.visible[t.slot(4)]);
    a.advance(400, t, done, 8, &doneCount);  // catches up two phases, finishes
    TEST_ASSERT_TRUE(t.visible[t.slot(4)]);
    TEST_ASSERT_EQUAL_UINT8(1, doneCount);
    TEST_ASSERT_EQUAL_UINT32(9, done[0].doneActionId);
}

static void test_cancel_for_elem_silent(void) {
    CanvasAnimator a;
    FakeTarget t;
    TweenConfig cfg;
    cfg.elemId       = 6;
    cfg.toX          = 10;
    cfg.durationMs   = 100;
    cfg.doneActionId = 77;
    a.start(cfg, 0);
    a.cancelForElem(6);
    a.advance(1000, t, done, 8, &doneCount);
    TEST_ASSERT_EQUAL_UINT8(0, doneCount);   // no completion after cancel
    TEST_ASSERT_EQUAL_UINT8(0, a.activeCount());
}

// --- Sprite store -----------------------------------------------------------

static uint8_t arena[4096];

static void test_sprite_create_and_frames(void) {
    SpriteStore st;
    st.setArena(arena, sizeof(arena));
    // 8x2, 3 frames: stride 1, 2 bytes per frame.
    uint8_t sheet[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    int32_t h = st.create(8, 2, 3, sheet, sizeof(sheet));
    TEST_ASSERT_GREATER_THAN_INT32(0, h);

    SpriteStore::FrameView fv;
    TEST_ASSERT_TRUE(st.frameView(static_cast<uint32_t>(h), &fv));
    TEST_ASSERT_EQUAL_UINT8(0x11, fv.data[0]);
    TEST_ASSERT_NULL(fv.mask);
    TEST_ASSERT_TRUE(st.setFrame(static_cast<uint32_t>(h), 2));
    st.frameView(static_cast<uint32_t>(h), &fv);
    TEST_ASSERT_EQUAL_UINT8(0x55, fv.data[0]);
    TEST_ASSERT_FALSE(st.setFrame(static_cast<uint32_t>(h), 3));
}

static void test_sprite_play_loop_and_once(void) {
    SpriteStore st;
    st.setArena(arena, sizeof(arena));
    uint8_t sheet[6] = {0};
    uint32_t h = static_cast<uint32_t>(st.create(8, 2, 3, sheet, sizeof(sheet)));

    // ONCE: 0 -> 1 -> 2, then stops holding frame 2.
    TEST_ASSERT_TRUE(st.play(h, SPRITE_ONCE, 100, 0, 11, 0));
    st.advance(100, done, 8, &doneCount);
    TEST_ASSERT_EQUAL_INT32(1, st.frame(h));
    doneCount = 0;
    st.advance(300, done, 8, &doneCount);
    TEST_ASSERT_EQUAL_INT32(2, st.frame(h));
    TEST_ASSERT_EQUAL_UINT8(1, doneCount);
    TEST_ASSERT_EQUAL_UINT32(11, done[0].doneActionId);
    TEST_ASSERT_FALSE(st.anyPlaying());

    // LOOP with one extra cycle: wraps once, second wrap holds last frame.
    TEST_ASSERT_TRUE(st.play(h, SPRITE_LOOP, 100, 1, 0, 1000));
    doneCount = 0;
    st.advance(1300, done, 8, &doneCount);   // f2 reached, cycle done, wraps
    TEST_ASSERT_EQUAL_INT32(0, st.frame(h));
    st.advance(1600, done, 8, &doneCount);   // second cycle ends, holds f2
    TEST_ASSERT_EQUAL_INT32(2, st.frame(h));
    TEST_ASSERT_FALSE(st.anyPlaying());
}

static void test_sprite_ping_pong_sequence(void) {
    SpriteStore st;
    st.setArena(arena, sizeof(arena));
    uint8_t sheet[6] = {0};
    uint32_t h = static_cast<uint32_t>(st.create(8, 2, 3, sheet, sizeof(sheet)));
    TEST_ASSERT_TRUE(st.play(h, SPRITE_PING_PONG, 100, REPEAT_FOREVER, 0, 0));
    // Expected: 0 1 2 1 0 1 2 ...
    const uint16_t expect[] = {1, 2, 1, 0, 1, 2, 1};
    for (int i = 0; i < 7; ++i) {
        st.advance(100 * (i + 1), done, 8, &doneCount);
        TEST_ASSERT_EQUAL_INT32(expect[i], st.frame(h));
    }
    TEST_ASSERT_TRUE(st.anyPlaying());
}

static void test_sprite_mask_and_destroy_compaction(void) {
    SpriteStore st;
    st.setArena(arena, sizeof(arena));
    uint8_t sheetA[4] = {0xAA, 0xAA, 0xAA, 0xAA};   // 8x2, 2 frames
    uint8_t sheetB[2] = {0xBB, 0xBB};               // 8x2, 1 frame
    uint32_t a = static_cast<uint32_t>(st.create(8, 2, 2, sheetA, sizeof(sheetA)));
    uint32_t b = static_cast<uint32_t>(st.create(8, 2, 1, sheetB, sizeof(sheetB)));
    uint8_t maskB[2] = {0x0F, 0x0F};
    TEST_ASSERT_TRUE(st.setMask(b, maskB, sizeof(maskB)));

    // Destroying A compacts the arena; B's data and mask must survive.
    TEST_ASSERT_TRUE(st.destroy(a));
    SpriteStore::FrameView fv;
    TEST_ASSERT_TRUE(st.frameView(b, &fv));
    TEST_ASSERT_EQUAL_UINT8(0xBB, fv.data[0]);
    TEST_ASSERT_NOT_NULL(fv.mask);
    TEST_ASSERT_EQUAL_UINT8(0x0F, fv.mask[0]);
}

static void test_sprite_per_frame_durations(void) {
    SpriteStore st;
    st.setArena(arena, sizeof(arena));
    uint8_t sheet[4] = {0};
    uint32_t h = static_cast<uint32_t>(st.create(8, 2, 2, sheet, sizeof(sheet)));
    uint16_t durs[2] = {100, 300};
    TEST_ASSERT_TRUE(st.setFrameDurations(h, durs, 2));
    TEST_ASSERT_TRUE(st.play(h, SPRITE_LOOP, 50, REPEAT_FOREVER, 0, 0));
    st.advance(100, done, 8, &doneCount);
    TEST_ASSERT_EQUAL_INT32(1, st.frame(h));   // frame 0 lasted 100 ms
    st.advance(350, done, 8, &doneCount);
    TEST_ASSERT_EQUAL_INT32(1, st.frame(h));   // frame 1 lasts 300 ms
    st.advance(400, done, 8, &doneCount);
    TEST_ASSERT_EQUAL_INT32(0, st.frame(h));
}

static void test_easing_elastic_shape(void) {
    // Endpoints exact, overshoots past ONE early, settles near ONE late.
    TEST_ASSERT_EQUAL_INT32(0, ease(EASE_ELASTIC, 0));
    TEST_ASSERT_EQUAL_INT32(256, ease(EASE_ELASTIC, 256));
    bool exceeded = false;
    for (int32_t t = 1; t < 128; ++t) {
        if (ease(EASE_ELASTIC, t) > 256) exceeded = true;
    }
    TEST_ASSERT_TRUE(exceeded);
    for (int32_t t = 200; t < 256; ++t) {
        int32_t e = ease(EASE_ELASTIC, t);
        TEST_ASSERT_INT32_WITHIN(8, 256, e);
    }
}

static void test_sprite_scroll_wraps(void) {
    SpriteStore st;
    st.setArena(arena, sizeof(arena));
    // 32x2 single-frame strip, window 8, step 4, gap 8 -> span 40.
    uint8_t strip[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t h = static_cast<uint32_t>(st.create(32, 2, 1, strip, sizeof(strip)));
    TEST_ASSERT_TRUE(st.scroll(h, 8, 4, 8, 100, 0));
    TEST_ASSERT_TRUE(st.anyPlaying());

    SpriteStore::FrameView fv;
    st.advance(100, done, 8, &doneCount);
    st.frameView(h, &fv);
    TEST_ASSERT_EQUAL_UINT16(4, fv.scrollX);
    TEST_ASSERT_EQUAL_UINT16(8, fv.scrollWindow);
    st.advance(1000, done, 8, &doneCount);   // 10 beats total -> 40 % 40 = 0
    st.frameView(h, &fv);
    TEST_ASSERT_EQUAL_UINT16(0, fv.scrollX);
    TEST_ASSERT_TRUE(st.anyPlaying());       // marquees never finish
}

static void test_sprite_scale_setter(void) {
    SpriteStore st;
    st.setArena(arena, sizeof(arena));
    uint8_t sheet[4] = {0};
    uint32_t h = static_cast<uint32_t>(st.create(8, 2, 2, sheet, sizeof(sheet)));
    TEST_ASSERT_TRUE(st.setScale(h, 3));
    SpriteStore::FrameView fv;
    st.frameView(h, &fv);
    TEST_ASSERT_EQUAL_UINT8(3, fv.scale);
    TEST_ASSERT_FALSE(st.setScale(h, 0));
    TEST_ASSERT_FALSE(st.setScale(h, 5));
}

static void test_sprite_arena_exhaustion(void) {
    SpriteStore st;
    static uint8_t tiny[8];
    st.setArena(tiny, sizeof(tiny));
    uint8_t sheet[16] = {0};
    // 8x2 x4 frames = 8 bytes: fits the 8-byte arena exactly.
    TEST_ASSERT_GREATER_THAN_INT32(0, st.create(8, 2, 4, sheet, sizeof(sheet)));
    // Arena is now full; the next sprite must be rejected.
    TEST_ASSERT_EQUAL_INT32(-1, st.create(8, 2, 1, sheet, sizeof(sheet)));
    // Bad geometry / short buffer are parameter errors, not memory errors.
    TEST_ASSERT_EQUAL_INT32(0, st.create(0, 2, 1, sheet, sizeof(sheet)));
    TEST_ASSERT_EQUAL_INT32(0, st.create(8, 2, 4, sheet, 4));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_easing_endpoints);
    RUN_TEST(test_easing_midpoints);
    RUN_TEST(test_easing_overshoot_exceeds_one);
    RUN_TEST(test_easing_bounce_in_range);
    RUN_TEST(test_tween_linear_progress_and_completion);
    RUN_TEST(test_tween_delay_and_from_current);
    RUN_TEST(test_tween_yoyo_repeat);
    RUN_TEST(test_tween_chain_and_cancel_propagation);
    RUN_TEST(test_tween_pause_resume);
    RUN_TEST(test_blink_toggles_and_ends_visible);
    RUN_TEST(test_cancel_for_elem_silent);
    RUN_TEST(test_sprite_create_and_frames);
    RUN_TEST(test_sprite_play_loop_and_once);
    RUN_TEST(test_sprite_ping_pong_sequence);
    RUN_TEST(test_sprite_mask_and_destroy_compaction);
    RUN_TEST(test_sprite_per_frame_durations);
    RUN_TEST(test_easing_elastic_shape);
    RUN_TEST(test_sprite_scroll_wraps);
    RUN_TEST(test_sprite_scale_setter);
    RUN_TEST(test_sprite_arena_exhaustion);
    return UNITY_END();
}
