#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aplay.h"

#define TICK 16u
#define MAXF 64

enum { ST_IDLE, ST_LISTENING, ST_THINKING, ST_SPEAKING, ST_ERROR, ST_OFFLINE };
enum { C_IDLE, C_TALK, C_ONCE, C_STILL, C_END, C_COUNT };

/* Synthetic timing: a repeating 50/40/30 ms pattern over 30 frames is a 1200 ms loop, 75 ticks of 16 ms. */
static const uint16_t PATTERN[3] = {50, 40, 30};
#define IDLE_FRAMES 30
#define IDLE_LOOP_MS 1200u
#define ONCE_FRAMES 4
#define ONCE_MS 40u

static uint8_t g_tbl[C_COUNT][MAXF * AP_FRAME_ENTRY_BYTES];
static const uint8_t g_payload[1];
static ap_pack_t g_pack;

static void set_clip(int k, int frames, const uint16_t *ms, int n_ms, bool loop)
{
    ap_clip_t *c = &g_pack.clip[k];
    c->w = c->h = 1;
    c->frames = (uint16_t)frames;
    c->colors = 1;
    c->loop = loop;
    c->frame_tbl = g_tbl[k];
    for (int i = 0; i < frames; i++) {
        uint8_t *e = g_tbl[k] + i * AP_FRAME_ENTRY_BYTES;
        uint16_t meta = (uint16_t)(ms[i % n_ms] | (i == 0 ? AP_FRAME_KEY : 0));
        memset(e, 0, AP_FRAME_ENTRY_BYTES);
        e[12] = (uint8_t)meta;
        e[13] = (uint8_t)(meta >> 8);
    }
}

/* IDLE plays C_IDLE; LISTENING and THINKING share C_TALK; SPEAKING has a one-frame clip; ERROR a clip without loop. */
static const ap_pack_t *pack(void)
{
    static const uint16_t TALK[1] = {50}, ONCE[1] = {ONCE_MS}, STILL[1] = {100}, END[1] = {60};
    memset(&g_pack, 0, sizeof g_pack);
    g_pack.base = g_payload;
    g_pack.clip_count = C_COUNT;
    set_clip(C_IDLE, IDLE_FRAMES, PATTERN, 3, true);
    set_clip(C_TALK, 6, TALK, 1, true);
    set_clip(C_ONCE, ONCE_FRAMES, ONCE, 1, false);
    set_clip(C_STILL, 1, STILL, 1, true);
    set_clip(C_END, 3, END, 1, false);
    memset(g_pack.state_clip, AP_NO_CLIP, sizeof g_pack.state_clip);
    g_pack.state_clip[ST_IDLE] = C_IDLE;
    g_pack.state_clip[ST_LISTENING] = C_TALK;
    g_pack.state_clip[ST_THINKING] = C_TALK;
    g_pack.state_clip[ST_SPEAKING] = C_STILL;
    g_pack.state_clip[ST_ERROR] = C_END;
    g_pack.pickup_clip = C_ONCE;
    return &g_pack;
}

static bool is(aplay_out_t o, int clip, int frame)
{
    return o.clip == clip && o.frame == frame;
}

/* Ticks every TICK ms from *t, the last tick landing on t_end; returns the last output. */
static aplay_out_t run(aplay_t *a, uint32_t *t, uint32_t t_end, int state, aplay_stage_t stage)
{
    aplay_out_t o = aplay_tick(a, *t, state, stage);
    while ((int32_t)(t_end - *t) > 0) {
        *t += (int32_t)(t_end - *t) < (int32_t)TICK ? (t_end - *t) : TICK;
        o = aplay_tick(a, *t, state, stage);
    }
    return o;
}

static void test_s1_state_to_clip(void)
{
    aplay_t a;
    aplay_init(&a, pack());
    static const int WANT[] = {C_IDLE, C_TALK, C_TALK, C_STILL, C_END, APLAY_CLAWD};
    for (int s = ST_IDLE; s <= ST_OFFLINE; s++) {
        assert(aplay_tick(&a, 1000u * (uint32_t)s, s, APS_ACTIVE).clip == WANT[s]);
    }
    assert(aplay_tick(&a, 9000, 9, APS_ACTIVE).clip == APLAY_CLAWD);
    assert(aplay_tick(&a, 9016, -1, APS_ACTIVE).clip == APLAY_CLAWD);
    assert(is(aplay_tick(&a, 9032, ST_IDLE, APS_ACTIVE), C_IDLE, 0));
}

static void test_s2_default_on_and_fallbacks(void)
{
    aplay_t a;
    aplay_init(&a, pack());
    assert(a.enabled && is(aplay_tick(&a, 0, ST_IDLE, APS_ACTIVE), C_IDLE, 0));
    /* Red proof: a player that starts off until told otherwise shows Clawd with a valid pack and no stored setting. */
    aplay_t off;
    aplay_init(&off, pack());
    off.enabled = false;
    assert(aplay_tick(&off, 0, ST_IDLE, APS_ACTIVE).clip == APLAY_CLAWD);
    aplay_enable(&a, false);
    assert(aplay_tick(&a, 16, ST_IDLE, APS_ACTIVE).clip == APLAY_CLAWD);
    assert(aplay_tick(&a, 32, ST_IDLE, APS_OFF).clip == APLAY_CLAWD);
    aplay_enable(&a, true);
    assert(is(aplay_tick(&a, 48, ST_IDLE, APS_ACTIVE), C_IDLE, 0));
    aplay_t none;
    aplay_init(&none, NULL);
    aplay_enable(&none, true);
    aplay_pin(&none, 0);
    aplay_pickup(&none);
    assert(!none.enabled);
    for (int s = ST_IDLE; s <= ST_OFFLINE; s++) {
        assert(aplay_tick(&none, (uint32_t)s * TICK, s, APS_ACTIVE).clip == APLAY_CLAWD);
    }
}

/* Wrong rule: rebase each frame on the tick that showed it, so every frame lasts a whole number of ticks. */
static uint32_t loop_ms_rebasing(void)
{
    uint32_t t = 0, due = PATTERN[0];
    int f = 0;
    for (;;) {
        t += TICK;
        if ((int32_t)(t - due) >= 0) {
            f = (f + 1) % IDLE_FRAMES;
            due = t + PATTERN[f % 3];
            if (f == 0) {
                return t;
            }
        }
    }
}

/* Ticks the player in IDLE from t0 and returns the elapsed time of each of the first n loop wraps. */
static void loop_wraps(uint32_t t0, uint32_t *wraps, int n)
{
    aplay_t a;
    aplay_init(&a, pack());
    uint32_t t = t0;
    aplay_out_t prev = aplay_tick(&a, t, ST_IDLE, APS_ACTIVE);
    int got = 0;
    while (got < n && t - t0 < 2u * (uint32_t)n * IDLE_LOOP_MS) {
        t += TICK;
        aplay_out_t o = aplay_tick(&a, t, ST_IDLE, APS_ACTIVE);
        assert(o.clip == C_IDLE && (o.frame == prev.frame || o.frame == (prev.frame + 1) % IDLE_FRAMES));
        if (o.frame == 0 && prev.frame != 0) {
            wraps[got++] = t - t0;
        }
        prev = o;
    }
    assert(got == n && a.late == 0 && a.steps == (uint32_t)n * IDLE_FRAMES);
}

static void test_s3_exact_rate(void)
{
    uint32_t w[10];
    loop_wraps(0, w, 10);
    for (int k = 0; k < 10; k++) {
        assert(w[k] == (uint32_t)(k + 1) * IDLE_LOOP_MS);
    }
    /* Red proof: rebasing on the showing tick stretches the loop from 1200 to 1440 ms. */
    assert(loop_ms_rebasing() == 1440u);
}

/* Counts jumps to frame 0 from inside the loop over one second of THINKING ticks. */
static int restarts(aplay_t *a, uint32_t *t, bool restart_on_set)
{
    int n = 0;
    aplay_out_t prev = aplay_tick(a, *t, ST_THINKING, APS_ACTIVE);
    for (int i = 1; i <= 62; i++) {
        *t += TICK;
        if (restart_on_set && i % 6 == 0) {
            a->running = false;
        }
        aplay_out_t o = aplay_tick(a, *t, ST_THINKING, APS_ACTIVE);
        n += o.frame == 0 && prev.frame != 0 && prev.frame != g_pack.clip[C_TALK].frames - 1;
        prev = o;
    }
    return n;
}

static void test_s4_cut_rules(void)
{
    aplay_t a;
    aplay_init(&a, pack());
    uint32_t t = 0;
    aplay_out_t o = run(&a, &t, 400, ST_IDLE, APS_ACTIVE);
    assert(o.clip == C_IDLE && o.frame > 3);
    o = aplay_tick(&a, t + 5, ST_LISTENING, APS_ACTIVE);
    assert(is(o, C_TALK, 0) && a.cuts == 1);
    t += 5;
    o = run(&a, &t, t + 120, ST_LISTENING, APS_ACTIVE);
    assert(is(o, C_TALK, 2));
    /* THINKING shares the clip but is a new state: cut to frame 0. */
    assert(is(aplay_tick(&a, t + TICK, ST_THINKING, APS_ACTIVE), C_TALK, 0) && a.cuts == 2);
    t += TICK;
    /* The same state on every tick, as after a re-sent SET_STATE, never restarts the clip mid-loop. */
    assert(restarts(&a, &t, false) == 0 && a.cuts == 2);
    /* Red proof: restarting on every SET_STATE (here every sixth tick) jumps back to frame 0 mid-loop. */
    assert(restarts(&a, &t, true) > 5);
}

/* Counts once-clip starts over 3 s in IDLE: one pickup edge at 112 ms, or the lifted level on every tick while held. */
static int once_starts(bool level, uint32_t lifted_ms)
{
    aplay_t a;
    aplay_init(&a, pack());
    uint32_t t = 0;
    int starts = 0;
    aplay_out_t prev = aplay_tick(&a, t, ST_IDLE, APS_ACTIVE);
    for (; t < 3000; t += TICK) {
        bool lifted = t >= 100 && t < 100 + lifted_ms;
        if (level ? lifted : t == 112) {
            aplay_pickup(&a);
        }
        aplay_out_t o = aplay_tick(&a, t + TICK, ST_IDLE, APS_ACTIVE);
        starts += is(o, C_ONCE, 0) && !is(prev, C_ONCE, 0);
        prev = o;
    }
    return starts;
}

static void test_s5_pickup_once(void)
{
    aplay_t a;
    aplay_init(&a, pack());
    uint32_t t = 0;
    run(&a, &t, 200, ST_IDLE, APS_ACTIVE);
    aplay_pickup(&a);
    t += TICK;
    assert(is(aplay_tick(&a, t, ST_IDLE, APS_ACTIVE), C_ONCE, 0));
    for (int f = 1; f < ONCE_FRAMES; f++) {
        if (f == 2) {
            aplay_pickup(&a);
        }
        aplay_out_t o = run(&a, &t, t + ONCE_MS, ST_IDLE, APS_ACTIVE);
        assert(is(o, C_ONCE, f));
    }
    assert(is(run(&a, &t, t + ONCE_MS, ST_IDLE, APS_ACTIVE), C_IDLE, 0));
    assert(is(run(&a, &t, t + PATTERN[0], ST_IDLE, APS_ACTIVE), C_IDLE, 1));
    /* Pickup in LISTENING is dropped, not kept for the next IDLE. */
    aplay_init(&a, pack());
    t = 0;
    run(&a, &t, 100, ST_LISTENING, APS_ACTIVE);
    aplay_pickup(&a);
    assert(aplay_tick(&a, t += TICK, ST_LISTENING, APS_ACTIVE).clip == C_TALK);
    assert(is(aplay_tick(&a, t += TICK, ST_IDLE, APS_ACTIVE), C_IDLE, 0));
    /* A state change cuts a running once-clip. */
    aplay_pickup(&a);
    assert(is(aplay_tick(&a, t += TICK, ST_IDLE, APS_ACTIVE), C_ONCE, 0));
    assert(is(aplay_tick(&a, t += TICK, ST_SPEAKING, APS_ACTIVE), C_STILL, 0));
    /* The tick that sees the pickup in DIM drops it. */
    aplay_init(&a, pack());
    t = 0;
    aplay_tick(&a, t, ST_IDLE, APS_DIM);
    aplay_pickup(&a);
    assert(aplay_tick(&a, t += TICK, ST_IDLE, APS_DIM).clip == C_IDLE);
    assert(aplay_tick(&a, t += TICK, ST_IDLE, APS_ACTIVE).clip == C_IDLE);
    /* One pickup edge plays once, however long the board stays lifted. */
    assert(once_starts(false, 2000) == 1);
    /* Red proof: feeding the lifted level replays the once-clip for as long as the board is held. */
    assert(once_starts(true, 2000) > 5);
}

static void test_s6_dim_holds(void)
{
    aplay_t a;
    aplay_init(&a, pack());
    uint32_t t = 0;
    aplay_out_t held = run(&a, &t, 333, ST_IDLE, APS_ACTIVE);
    const uint32_t steps = a.steps;
    for (uint32_t end = t + 30u * 60u * 1000u; t < end;) {
        t += TICK;
        assert(is(aplay_tick(&a, t, ST_IDLE, APS_DIM), held.clip, held.frame));
    }
    assert(a.steps == steps);
    /* Back to ACTIVE: the held frame shows for its full duration, without a burst or a late count. */
    assert(is(aplay_tick(&a, t += TICK, ST_IDLE, APS_ACTIVE), held.clip, held.frame));
    ap_frame_t f;
    assert(ap_frame(&g_pack, held.clip, held.frame, &f));
    const uint32_t back = t;
    while (t + TICK < back + f.ms) {
        t += TICK;
        assert(is(aplay_tick(&a, t, ST_IDLE, APS_ACTIVE), held.clip, held.frame));
    }
    assert(is(run(&a, &t, back + f.ms + TICK, ST_IDLE, APS_ACTIVE), held.clip, held.frame + 1));
    assert(a.late == 0 && a.steps == steps + 1);
    /* Red proof: resuming without rebasing the schedule steps on the first ACTIVE tick and counts it late. */
    aplay_t r;
    aplay_init(&r, pack());
    uint32_t tr = 0;
    run(&r, &tr, 333, ST_IDLE, APS_ACTIVE);
    run(&r, &tr, tr + 60000, ST_IDLE, APS_DIM);
    r.dim = false;
    const uint32_t r_steps = r.steps;
    aplay_tick(&r, tr += TICK, ST_IDLE, APS_ACTIVE);
    assert(r.steps == r_steps + 1 && r.late == 1);
    /* A once-clip ends at DIM entry: frame 0 of the state's clip, held. */
    aplay_pickup(&a);
    assert(is(aplay_tick(&a, t += TICK, ST_IDLE, APS_ACTIVE), C_ONCE, 0));
    assert(is(aplay_tick(&a, t += TICK, ST_IDLE, APS_DIM), C_IDLE, 0));
    assert(is(run(&a, &t, t + 2000, ST_IDLE, APS_DIM), C_IDLE, 0));
}

static void test_s7_off(void)
{
    aplay_t a;
    aplay_init(&a, pack());
    uint32_t t = 0;
    aplay_out_t before = run(&a, &t, 333, ST_IDLE, APS_ACTIVE);
    assert(before.frame > 0);
    const uint32_t steps = a.steps;
    assert(run(&a, &t, t + 60000, ST_IDLE, APS_OFF).clip == APLAY_NOTHING && a.steps == steps);
    assert(is(aplay_tick(&a, t += TICK, ST_IDLE, APS_ACTIVE), C_IDLE, 0));
    run(&a, &t, t + 333, ST_IDLE, APS_ACTIVE);
    aplay_tick(&a, t += TICK, ST_IDLE, APS_OFF);
    aplay_pickup(&a);
    assert(is(aplay_tick(&a, t += TICK, ST_IDLE, APS_ACTIVE), C_ONCE, 0));
    /* Red proof: resuming the held frame after OFF would show a delta frame without its key. */
    aplay_init(&a, pack());
    t = 0;
    before = run(&a, &t, 333, ST_IDLE, APS_ACTIVE);
    aplay_tick(&a, t += TICK, ST_IDLE, APS_OFF);
    a.running = true;
    aplay_out_t resumed = aplay_tick(&a, t += TICK, ST_IDLE, APS_ACTIVE);
    ap_frame_t f;
    assert(resumed.frame >= before.frame && ap_frame(&g_pack, resumed.clip, resumed.frame, &f) && !f.key);
}

/* Wrong rule: catch up every missed frame in one tick. */
static int catch_up_steps(uint32_t stall_ms)
{
    uint32_t due = PATTERN[0], now = stall_ms;
    int f = 0, steps = 0;
    while ((int32_t)(now - due) >= 0) {
        f = (f + 1) % IDLE_FRAMES;
        due += PATTERN[f % 3];
        steps++;
    }
    return steps;
}

static void test_s8_late(void)
{
    aplay_t a;
    aplay_init(&a, pack());
    assert(is(aplay_tick(&a, 0, ST_IDLE, APS_ACTIVE), C_IDLE, 0));
    aplay_out_t o = aplay_tick(&a, 500, ST_IDLE, APS_ACTIVE);
    assert(is(o, C_IDLE, 1) && a.steps == 1 && a.late == 1);
    uint32_t t = 500;
    o = run(&a, &t, 500 + PATTERN[1] - 1, ST_IDLE, APS_ACTIVE);
    assert(is(o, C_IDLE, 1) && a.late == 1);
    o = run(&a, &t, 500 + PATTERN[1] + TICK, ST_IDLE, APS_ACTIVE);
    assert(is(o, C_IDLE, 2) && a.late == 1);
    /* Red proof: catching up would step 12 frames in the one tick after the stall. */
    assert(catch_up_steps(500) == 12);
}

/* Wrong rule: unsigned comparison of the clock against the due time; returns the first loop wrap, 0 if none. */
static uint32_t first_wrap_unsigned(uint32_t t0, uint32_t span)
{
    uint32_t t = t0, due = t0 + PATTERN[0];
    int f = 0;
    for (uint32_t k = 0; k < span / TICK; k++) {
        t += TICK;
        if (t >= due) {
            f = (f + 1) % IDLE_FRAMES;
            due += PATTERN[f % 3];
            if (f == 0) {
                return t - t0;
            }
        }
    }
    return 0;
}

static void test_s9_clock_wrap(void)
{
    /* From this start frame 1 falls due 5 ms before the clock wraps and the next tick lands at 1. */
    const uint32_t t0 = 0xFFFFFFFFu - 94u;
    uint32_t w[5];
    loop_wraps(t0, w, 5);
    for (int k = 0; k < 5; k++) {
        assert(w[k] == (uint32_t)(k + 1) * IDLE_LOOP_MS);
    }
    /* Red proof: the unsigned compare stalls on frame 1 once the clock has wrapped. */
    assert(first_wrap_unsigned(t0, 5 * IDLE_LOOP_MS) == 0);
    /* A 500 ms stall across the wrap is one late step, then the normal pace. */
    aplay_t a;
    aplay_init(&a, pack());
    uint32_t t = 0xFFFFFFFFu - 200u;
    aplay_tick(&a, t, ST_IDLE, APS_ACTIVE);
    t += 500;
    assert(is(aplay_tick(&a, t, ST_IDLE, APS_ACTIVE), C_IDLE, 1) && a.late == 1);
    assert(is(aplay_tick(&a, t + TICK, ST_IDLE, APS_ACTIVE), C_IDLE, 1));
    assert(is(aplay_tick(&a, t + PATTERN[1], ST_IDLE, APS_ACTIVE), C_IDLE, 2) && a.late == 1 && a.steps == 2);
}

static void test_s10_one_frame_clip(void)
{
    aplay_t a;
    aplay_init(&a, pack());
    uint32_t t = 0;
    aplay_tick(&a, t, ST_SPEAKING, APS_ACTIVE);
    for (; t < 10000; t += TICK) {
        assert(is(aplay_tick(&a, t, ST_SPEAKING, APS_ACTIVE), C_STILL, 0));
    }
    assert(a.steps == 0 && a.late == 0);
}

static void test_clip_without_loop_holds_last_frame(void)
{
    aplay_t a;
    aplay_init(&a, pack());
    uint32_t t = 0;
    assert(is(run(&a, &t, 5000, ST_ERROR, APS_ACTIVE), C_END, 2) && a.steps == 2 && a.late == 0);
}

static void test_pin(void)
{
    aplay_t a;
    aplay_init(&a, pack());
    uint32_t t = 0;
    run(&a, &t, 200, ST_IDLE, APS_ACTIVE);
    aplay_pin(&a, C_END);
    assert(is(aplay_tick(&a, t += TICK, ST_IDLE, APS_ACTIVE), C_END, 0));
    assert(aplay_tick(&a, t += TICK, ST_LISTENING, APS_ACTIVE).clip == C_END && a.frame == 0);
    aplay_pickup(&a);
    assert(aplay_tick(&a, t += TICK, ST_IDLE, APS_ACTIVE).clip == C_END);
    aplay_pin(&a, C_COUNT);
    assert(a.pin == -1 && is(aplay_tick(&a, t += TICK, ST_IDLE, APS_ACTIVE), C_IDLE, 0));
}

/* Every shown frame is the previous one, its successor in the same clip, or a key frame 0. */
static void test_random_sequences_never_skip(void)
{
    aplay_t a;
    aplay_init(&a, pack());
    uint32_t seed = 12345, t = 0xFFFF0000u;
    aplay_out_t prev = {APLAY_NOTHING, 0};
    for (int i = 0; i < 200000; i++) {
        seed = seed * 1103515245u + 12345u;
        const uint32_t r = seed >> 8;
        t += (r % 50 == 0) ? (r % 3000) : TICK;
        const int state = (r % 400 == 0) ? (int)(r % 7) : (prev.clip == C_TALK ? ST_LISTENING : ST_IDLE);
        const aplay_stage_t stage = (r % 997 < 3) ? APS_OFF : (r % 991 < 5) ? APS_DIM : APS_ACTIVE;
        if (r % 300 == 0) {
            aplay_pickup(&a);
        }
        if (r % 5000 == 0) {
            aplay_enable(&a, (r & 1) != 0);
        }
        aplay_out_t o = aplay_tick(&a, t, state, stage);
        if (o.clip >= 0) {
            bool same = is(o, prev.clip, prev.frame), next = o.clip == prev.clip && o.frame == prev.frame + 1;
            assert(same || next || o.frame == 0);
            assert(o.clip != C_ONCE || state == ST_IDLE);
        }
        assert(stage != APS_OFF || o.clip < 0);
        prev = o;
    }
}

static int g_ready;

static int ready_cb(void *ctx, int clip)
{
    (void)ctx;
    (void)clip;
    return g_ready;
}

/* S11: the player holds before an unready frame, counts the stall once, and rebases on arrival. */
static void test_s11_ready_gate(void)
{
    aplay_t a;
    aplay_init(&a, pack());
    aplay_set_ready(&a, ready_cb, NULL);
    g_ready = 3;
    uint32_t t = 0;
    aplay_tick(&a, t, ST_LISTENING, APS_ACTIVE);
    assert(is(run(&a, &t, 250, ST_LISTENING, APS_ACTIVE), C_TALK, 2) && a.starve == 1 && a.late == 0);
    assert(is(run(&a, &t, 400, ST_LISTENING, APS_ACTIVE), C_TALK, 2) && a.starve == 1);
    g_ready = 6;
    assert(is(aplay_tick(&a, t += TICK, ST_LISTENING, APS_ACTIVE), C_TALK, 3) && a.late == 1 && a.starve == 1);
}

int main(void)
{
    test_s1_state_to_clip();
    test_s2_default_on_and_fallbacks();
    test_s3_exact_rate();
    test_s4_cut_rules();
    test_s5_pickup_once();
    test_s6_dim_holds();
    test_s7_off();
    test_s8_late();
    test_s9_clock_wrap();
    test_s10_one_frame_clip();
    test_clip_without_loop_holds_last_frame();
    test_pin();
    test_random_sequences_never_skip();
    test_s11_ready_gate();
    puts("aplay_test ok");
    return 0;
}
