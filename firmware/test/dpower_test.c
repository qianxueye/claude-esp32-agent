#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "app_config.h"
#include "dpower.h"

#define TICK 16u

/* Advances from *t to t_end in UI ticks and returns the last output. */
static dp_out_t run_to(dpower_t *d, uint32_t *t, uint32_t t_end)
{
    dp_out_t o = dp_update(d, *t);
    while ((int32_t)(t_end - *t) > 0) {
        *t += (int32_t)(t_end - *t) < (int32_t)TICK ? (t_end - *t) : TICK;
        o = dp_update(d, *t);
    }
    return o;
}

static void test_timeline_from(uint32_t t0)
{
    dpower_t d;
    dp_init(&d, t0);
    uint32_t t = t0;
    dp_out_t o = run_to(&d, &t, t0 + DISP_HOLD_MS - 1);
    assert(o.stage == DP_ACTIVE && o.level_pct == DISP_ACTIVE_PERCENT && o.panel_on);
    o = run_to(&d, &t, t0 + DISP_HOLD_MS);
    assert(o.stage == DP_DIM && o.level_pct == DISP_ACTIVE_PERCENT);
    int prev = o.level_pct;
    while (t < t0 + DISP_HOLD_MS + DISP_RAMP_MS) {
        o = run_to(&d, &t, t + 7);
        assert(o.level_pct <= prev && o.level_pct >= DISP_DIM_PERCENT);
        prev = o.level_pct;
    }
    o = run_to(&d, &t, t0 + DISP_HOLD_MS + DISP_RAMP_MS);
    assert(o.level_pct == DISP_DIM_PERCENT && o.stage == DP_DIM && o.panel_on);
    o = run_to(&d, &t, t0 + DISP_HOLD_MS + DISP_DIM_TO_OFF_MS - 1);
    assert(o.stage == DP_DIM && o.level_pct == DISP_DIM_PERCENT);
    o = run_to(&d, &t, t0 + DISP_HOLD_MS + DISP_DIM_TO_OFF_MS);
    assert(o.stage == DP_OFF && o.panel_on && o.level_pct == DISP_DIM_PERCENT);
    o = run_to(&d, &t, t0 + DISP_HOLD_MS + DISP_DIM_TO_OFF_MS + DISP_RAMP_MS);
    assert(o.stage == DP_OFF && !o.panel_on && o.level_pct == DISP_FLOOR_PERCENT);
    o = run_to(&d, &t, t0 + 10u * 3600u * 1000u);
    assert(o.stage == DP_OFF && !o.panel_on);
}

static void test_activity_returns_active_from_every_point(void)
{
    const uint32_t span = DISP_HOLD_MS + DISP_DIM_TO_OFF_MS + 60000u;
    for (uint32_t at = 0; at < span; at += 6997) {
        for (int cause = DP_CAUSE_TOUCH; cause <= DP_CAUSE_MOTION; cause += 1) {
            if (cause == DP_CAUSE_STATE) {
                continue;
            }
            dpower_t d;
            dp_init(&d, 0);
            uint32_t t = 0;
            run_to(&d, &t, at);
            dp_activity(&d, t, (dp_cause_t)cause);
            const uint32_t act = t;
            dp_out_t o = dp_update(&d, t);
            assert(o.stage == DP_ACTIVE && o.panel_on && (int)o.cause == cause);
            o = run_to(&d, &t, t + DISP_RAMP_MS);
            assert(o.level_pct == DISP_ACTIVE_PERCENT);
            o = run_to(&d, &t, act + DISP_HOLD_MS - 1);
            assert(o.stage == DP_ACTIVE);
            o = run_to(&d, &t, act + DISP_HOLD_MS);
            assert(o.stage == DP_DIM);
        }
    }
}

/* PING and other host frames have no call into dpower; the state calls below cover the rest of the non-activity list. */
static void test_non_activity_state_changes(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    run_to(&d, &t, 15000);
    for (int i = 0; i < 5; i++) {
        dp_state(&d, t, DP_STATE_IDLE);
    }
    dp_state(&d, t, DP_STATE_OFFLINE);
    dp_state(&d, t + 100, DP_STATE_IDLE);
    dp_out_t o = run_to(&d, &t, DISP_HOLD_MS - 1);
    assert(o.stage == DP_ACTIVE);
    o = run_to(&d, &t, DISP_HOLD_MS);
    assert(o.stage == DP_DIM);
    dp_state(&d, t, DP_STATE_OFFLINE);
    o = run_to(&d, &t, DISP_HOLD_MS + DISP_DIM_TO_OFF_MS);
    assert(o.stage == DP_OFF);
    dp_state(&d, t, DP_STATE_IDLE);
    o = dp_update(&d, t);
    assert(o.stage == DP_OFF);
}

static void test_attention_states(void)
{
    for (int st = DP_STATE_LISTENING; st <= DP_STATE_ERROR; st++) {
        dpower_t d;
        dp_init(&d, 0);
        uint32_t t = 0;
        run_to(&d, &t, 50000);
        dp_state(&d, t, st);
        uint32_t t1 = t;
        dp_out_t o = dp_update(&d, t);
        assert(o.stage == DP_ACTIVE && o.cause == DP_CAUSE_STATE);
        o = run_to(&d, &t, t1 + DISP_ATTN_CAP_MS - 1);
        assert(o.stage == DP_ACTIVE && o.level_pct == DISP_ACTIVE_PERCENT);
        o = run_to(&d, &t, t1 + DISP_ATTN_CAP_MS);
        assert(o.stage == DP_DIM);
        o = run_to(&d, &t, t1 + 6u * 3600u * 1000u);
        assert(o.stage == DP_DIM && o.level_pct == DISP_DIM_PERCENT);
        dp_activity(&d, t, DP_CAUSE_TOUCH);
        o = run_to(&d, &t, t + DISP_HOLD_MS - 1);
        assert(o.stage == DP_ACTIVE);
        o = run_to(&d, &t, t + 1);
        assert(o.stage == DP_DIM);
        dp_state(&d, t, DP_STATE_IDLE);
        o = dp_update(&d, t);
        assert(o.stage == DP_ACTIVE && o.cause == DP_CAUSE_STATE);
    }
}

static void test_offline_never_brightens(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    dp_state(&d, t, DP_STATE_THINKING);
    run_to(&d, &t, DISP_ATTN_CAP_MS + 1000);
    dp_out_t o = dp_update(&d, t);
    assert(o.stage == DP_DIM);
    dp_state(&d, t, DP_STATE_OFFLINE);
    o = dp_update(&d, t);
    assert(o.stage != DP_ACTIVE);
    dp_state(&d, t, DP_STATE_SPEAKING);
    o = dp_update(&d, t);
    assert(o.stage != DP_ACTIVE);
}

static void test_force(void)
{
    dpower_t d;
    dp_init(&d, 0);
    dp_force(&d, DP_OFF);
    dp_out_t o = dp_update(&d, 16);
    assert(o.stage == DP_OFF && o.panel_on && o.cause == DP_CAUSE_FORCE);
    o = dp_update(&d, 16 + DISP_RAMP_MS);
    assert(o.stage == DP_OFF && !o.panel_on && o.cause == DP_CAUSE_FORCE);
    dp_force(&d, DP_DIM);
    o = dp_update(&d, 32 + DISP_RAMP_MS);
    assert(o.stage == DP_DIM && o.panel_on);
    dp_force(&d, -1);
    o = dp_update(&d, 48 + DISP_RAMP_MS);
    assert(o.stage == DP_ACTIVE);
}

static void test_orbit(void)
{
    const int cycle = ORBIT_COLS * ORBIT_ROWS;
    int seen[ORBIT_COLS][ORBIT_ROWS] = {{0}};
    for (int i = 0; i < cycle; i++) {
        int x, y, nx, ny;
        dp_orbit(i, &x, &y);
        dp_orbit(i + 1, &nx, &ny);
        assert(x >= ORBIT_X0_PX && x <= ORBIT_X0_PX + (ORBIT_COLS - 1) * ORBIT_PITCH_PX);
        assert(y >= ORBIT_Y0_PX && y <= ORBIT_Y0_PX + (ORBIT_ROWS - 1) * ORBIT_PITCH_PX);
        assert(x % BUBBLE_UNIT_PX == 0 && y % BUBBLE_UNIT_PX == 0);
        assert(abs(nx - x) + abs(ny - y) == ORBIT_PITCH_PX);
        seen[(x - ORBIT_X0_PX) / ORBIT_PITCH_PX][(y - ORBIT_Y0_PX) / ORBIT_PITCH_PX]++;
    }
    for (int c = 0; c < ORBIT_COLS; c++) {
        for (int r = 0; r < ORBIT_ROWS; r++) {
            assert(seen[c][r] == 1);
        }
    }
    int x0, y0, x1, y1;
    dp_orbit(0, &x0, &y0);
    dp_orbit(cycle, &x1, &y1);
    assert(x0 == x1 && y0 == y1);
    dp_orbit(-1, &x1, &y1);
    dp_orbit(cycle - 1, &x0, &y0);
    assert(x0 == x1 && y0 == y1);
}

/* Mirrors the extents in ui.c draw_mascot and draw_bubble: nothing may leave the scene at any lattice point. */
static void test_orbit_keeps_drawing_in_scene(void)
{
    const int u = BUBBLE_UNIT_PX, c = MASCOT_CELL_PX, scene = SCENE_SIZE_PX, shift = BUBBLE_SHIFT_UNITS * u;
    for (int i = 0; i < ORBIT_COLS * ORBIT_ROWS; i++) {
        int gx, gy;
        dp_orbit(i, &gx, &gy);
        int anchor_top = scene / 2 - 5 * c + shift + gy;
        int thought_top_u = (anchor_top - BUBBLE_GAP_UNITS * u) / u - 8 - 18;
        assert(thought_top_u >= 0);
        int body_top = scene / 2 - 5 * c + gy - 2 * u;
        int legs_bottom = scene / 2 - 5 * c + gy + shift + u + 10 * c;
        assert(body_top >= 0 && legs_bottom <= scene);
        int reach = SCUTTLE_DISTANCE_UNITS * u;
        assert(scene / 2 - 8 * c + gx - reach >= 0);
        assert(scene / 2 + 8 * c + gx + reach <= scene);
    }
}

static void test_stale_timestamps_stay_off(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    run_to(&d, &t, DISP_HOLD_MS + DISP_DIM_TO_OFF_MS + 1000);
    assert(dp_update(&d, t).stage == DP_OFF);
    for (int hours = 0; hours < 50 * 24; hours++) {
        t += 3600u * 1000u;
        assert(dp_update(&d, t).stage == DP_OFF);
    }
    dp_activity(&d, t, DP_CAUSE_TOUCH);
    assert(dp_update(&d, t).stage == DP_ACTIVE);
}

/* Lifted at t, a rest at t + 1 s: the stage dims DISP_SETDOWN_HOLD_MS later, or DISP_HOLD_MS later without the set-down call. */
static dp_stage_t after_setdown(bool setdown, int state, bool touch, uint32_t probe)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    run_to(&d, &t, 30000);
    dp_state(&d, t, state);
    for (uint32_t i = 0; i < 1000 / TICK; i++) {
        t += TICK;
        dp_activity(&d, t, DP_CAUSE_MOTION);
        dp_update(&d, t);
    }
    uint32_t rest = t;
    if (setdown) {
        dp_setdown(&d, t);
    }
    if (touch) {
        run_to(&d, &t, rest + 1000);
        dp_activity(&d, t, DP_CAUSE_TOUCH);
        rest = t;
    }
    return run_to(&d, &t, rest + probe).stage;
}

static void test_setdown(void)
{
    const uint32_t hold = DISP_SETDOWN_HOLD_MS;
    assert(after_setdown(true, DP_STATE_IDLE, false, hold - 20) == DP_ACTIVE);
    assert(after_setdown(true, DP_STATE_IDLE, false, hold) == DP_DIM);
    assert(after_setdown(false, DP_STATE_IDLE, false, hold + 1000) == DP_ACTIVE);
    assert(after_setdown(false, DP_STATE_IDLE, false, DISP_HOLD_MS - 20) == DP_ACTIVE);
    assert(after_setdown(false, DP_STATE_IDLE, false, DISP_HOLD_MS) == DP_DIM);
    assert(after_setdown(true, DP_STATE_IDLE, true, DISP_HOLD_MS - 20) == DP_ACTIVE);
    assert(after_setdown(true, DP_STATE_IDLE, true, DISP_HOLD_MS) == DP_DIM);
    assert(after_setdown(true, DP_STATE_THINKING, false, hold + 1000) == DP_ACTIVE);
}

/* The applied hold is reported: the short one after a set-down, the normal one once activity restores it. */
static void test_setdown_hold_reported(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    run_to(&d, &t, 30000);
    assert(dp_update(&d, t).hold_ms == DISP_HOLD_MS);
    dp_activity(&d, t, DP_CAUSE_MOTION);
    dp_setdown(&d, t);
    assert(dp_update(&d, t).hold_ms == DISP_SETDOWN_HOLD_MS);
    run_to(&d, &t, t + DISP_SETDOWN_HOLD_MS);
    assert(dp_update(&d, t).stage == DP_DIM && dp_update(&d, t).hold_ms == DISP_SETDOWN_HOLD_MS);
    dp_activity(&d, t, DP_CAUSE_TOUCH);
    assert(dp_update(&d, t).hold_ms == DISP_HOLD_MS);
}

static void test_setdown_then_new_pickup(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    run_to(&d, &t, 30000);
    dp_activity(&d, t, DP_CAUSE_MOTION);
    dp_setdown(&d, t);
    run_to(&d, &t, t + 2000);
    dp_activity(&d, t, DP_CAUSE_MOTION);
    assert(run_to(&d, &t, t + DISP_HOLD_MS - 20).stage == DP_ACTIVE);
}

/* An attention state lifts OFF to DIM without activity; that brightening is attributed to the state, not the timer. */
static void test_attention_brightening_cause(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    run_to(&d, &t, DISP_HOLD_MS + DISP_DIM_TO_OFF_MS + 1000);
    assert(dp_update(&d, t).stage == DP_OFF);
    dp_state(&d, t, DP_STATE_OFFLINE);
    dp_state(&d, t, DP_STATE_SPEAKING);
    dp_out_t o = dp_update(&d, t);
    assert(o.stage == DP_DIM && o.cause == DP_CAUSE_STATE);
}

/* Ramp: equal-ratio, 0.3 s, both directions; a reversal continues from the current level; waking from OFF starts at the panel minimum. */
static void test_ramp(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    run_to(&d, &t, DISP_HOLD_MS);
    dp_out_t o = run_to(&d, &t, DISP_HOLD_MS + DISP_RAMP_MS / 2);
    assert(o.level_pct >= 31 && o.level_pct <= 33);
    int mid = o.level_pct;
    dp_activity(&d, t, DP_CAUSE_TOUCH);
    o = dp_update(&d, t);
    assert(o.level_pct == mid);
    uint32_t reversed = t;
    int prev = mid;
    while (t < reversed + DISP_RAMP_MS) {
        o = run_to(&d, &t, t + 5);
        assert(o.level_pct >= prev);
        prev = o.level_pct;
    }
    assert(o.level_pct == DISP_ACTIVE_PERCENT);

    run_to(&d, &t, t + DISP_HOLD_MS + DISP_DIM_TO_OFF_MS + 5000);
    assert(dp_update(&d, t).stage == DP_OFF);
    dp_activity(&d, t, DP_CAUSE_BUTTON);
    o = dp_update(&d, t);
    assert(o.stage == DP_ACTIVE && o.panel_on && o.level_pct == DISP_FLOOR_PERCENT);
    o = run_to(&d, &t, t + DISP_RAMP_MS);
    assert(o.level_pct == DISP_ACTIVE_PERCENT);
}

/* DIM to OFF fades to the panel minimum first and switches the panel off only when the fade has ended. */
static void test_fade_out_then_off(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    const uint32_t off_at = DISP_HOLD_MS + DISP_DIM_TO_OFF_MS;
    run_to(&d, &t, off_at - 1);
    dp_out_t o = run_to(&d, &t, off_at);
    assert(o.stage == DP_OFF && o.panel_on && o.level_pct == DISP_DIM_PERCENT);
    int prev = o.level_pct;
    while (t < off_at + DISP_RAMP_MS - 1) {
        o = run_to(&d, &t, t + 7 < off_at + DISP_RAMP_MS - 1 ? t + 7 : off_at + DISP_RAMP_MS - 1);
        assert(o.panel_on && o.level_pct <= prev && o.level_pct >= DISP_FLOOR_PERCENT);
        prev = o.level_pct;
    }
    o = run_to(&d, &t, off_at + DISP_RAMP_MS);
    assert(!o.panel_on && o.level_pct == DISP_FLOOR_PERCENT && o.stage == DP_OFF);
}

/* A wake during the fade-out reverses from the current level and never switches the panel off. */
static void test_wake_during_fade_out(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    const uint32_t off_at = DISP_HOLD_MS + DISP_DIM_TO_OFF_MS;
    run_to(&d, &t, off_at + DISP_RAMP_MS / 2);
    dp_out_t o = dp_update(&d, t);
    int mid = o.level_pct;
    assert(o.panel_on && mid > DISP_FLOOR_PERCENT && mid < DISP_DIM_PERCENT);
    dp_activity(&d, t, DP_CAUSE_TOUCH);
    o = dp_update(&d, t);
    assert(o.stage == DP_ACTIVE && o.panel_on && o.level_pct == mid);
    uint32_t woke = t;
    int prev = mid;
    while (t < woke + DISP_RAMP_MS + 100) {
        o = run_to(&d, &t, t + 5);
        assert(o.panel_on && o.level_pct >= prev);
        prev = o.level_pct;
    }
    assert(o.level_pct == DISP_ACTIVE_PERCENT);
}

/* A wake from OFF switches the panel on at the minimum and ramps up from there, to the dim level in an attention state. */
static void test_wake_from_off_starts_at_floor(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    run_to(&d, &t, DISP_HOLD_MS + DISP_DIM_TO_OFF_MS + DISP_RAMP_MS + 5000);
    assert(!dp_update(&d, t).panel_on);
    dp_state(&d, t, DP_STATE_OFFLINE);
    dp_state(&d, t, DP_STATE_IDLE);
    dp_force(&d, -1);
    dp_activity(&d, t, DP_CAUSE_MOTION);
    dp_out_t o = dp_update(&d, t);
    assert(o.panel_on && o.level_pct == DISP_FLOOR_PERCENT && o.stage == DP_ACTIVE);
    int prev = o.level_pct;
    while (t < DISP_HOLD_MS + DISP_DIM_TO_OFF_MS + DISP_RAMP_MS + 5000 + DISP_RAMP_MS) {
        o = run_to(&d, &t, t + 5);
        assert(o.level_pct >= prev);
        prev = o.level_pct;
    }
    assert(o.level_pct == DISP_ACTIVE_PERCENT);
}


/* DIM lasts 30 minutes: DIM at hold + 1799 s, OFF at hold + 1800 s, also after the quick set-down. */
static void test_dim_to_off(void)
{
    assert(DISP_DIM_TO_OFF_MS == 1800000u);
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    assert(run_to(&d, &t, DISP_HOLD_MS + 1799000u).stage == DP_DIM);
    assert(run_to(&d, &t, DISP_HOLD_MS + 1800000u).stage == DP_OFF);

    dp_init(&d, 0);
    t = 0;
    run_to(&d, &t, 30000);
    for (uint32_t i = 0; i < 1000 / TICK; i++) {
        t += TICK;
        dp_activity(&d, t, DP_CAUSE_MOTION);
        dp_update(&d, t);
    }
    uint32_t rest = t;
    dp_setdown(&d, rest);
    assert(run_to(&d, &t, rest + DISP_SETDOWN_HOLD_MS).stage == DP_DIM);
    assert(run_to(&d, &t, rest + DISP_SETDOWN_HOLD_MS + 1799000u).stage == DP_DIM);
    assert(run_to(&d, &t, rest + DISP_SETDOWN_HOLD_MS + 1800000u).stage == DP_OFF);
}

/* Reference gate that looks at the stage on release; it is wrong because the wake has already made the screen ACTIVE by then. */
static bool wrong_gate_at_release(dp_stage_t stage_at_release)
{
    return stage_at_release == DP_ACTIVE;
}

static bool touch_sends(dp_stage_t at_press)
{
    dp_touch_t t;
    dp_touch_press(&t, at_press);
    return dp_touch_sends(&t);
}

static void test_touch_gate(void)
{
    assert(!touch_sends(DP_DIM));
    assert(!touch_sends(DP_OFF));
    assert(touch_sends(DP_ACTIVE));
    /* A second tap after the wake begins in ACTIVE. */
    assert(touch_sends(DP_ACTIVE));
    /* A hold begun dark is consumed whole, also at 1500 ms when the screen is ACTIVE. */
    dp_touch_t hold;
    dp_touch_press(&hold, DP_OFF);
    assert(!dp_touch_sends(&hold) && !dp_touch_sends(&hold));
    /* The release-time gate would send the tap of a touch begun in DIM: the red proof for the cases above. */
    assert(wrong_gate_at_release(DP_ACTIVE) && !touch_sends(DP_DIM));
}

/* Red proof for D2 and D3: a setter that changes only the target, so dp_update restarts the 300 ms ramp. */
static void wrong_set_active_by_clock(dpower_t *d, int pct)
{
    d->active = pct;
}

/* Red proof for D4: a setter that restarts the ramp from the current level. */
static void wrong_set_active_restart(dpower_t *d, uint32_t now, int pct)
{
    d->ramp_from = d->level;
    d->ramp_to = pct;
    d->ramp_t0 = now;
    d->active = pct;
}

/* Red proof for D6: a setter that always jumps the panel to the new level. */
static void wrong_set_active_jump(dpower_t *d, int pct)
{
    d->active = d->level = d->ramp_from = d->ramp_to = pct;
}

static void test_d2_boot_level(void)
{
    dpower_t d;
    dp_init(&d, 0);
    dp_set_active(&d, 20);
    assert(dp_update(&d, 0).level_pct == 20);
    dp_init(&d, 0);
    wrong_set_active_by_clock(&d, 20);
    assert(dp_update(&d, 0).level_pct == 100 && dp_update(&d, 16).level_pct == 92);
}

static void test_d3_follows_at_once(void)
{
    dpower_t d, w;
    dp_init(&d, 0);
    uint32_t t = 0;
    run_to(&d, &t, 1000);
    w = d;
    dp_set_active(&d, 64);
    assert(dp_update(&d, t).level_pct == 64);
    wrong_set_active_by_clock(&w, 64);
    assert(dp_update(&w, t).level_pct == 100);
}

/* Changes the ACTIVE level from 100 to 95 at elapsed e of a ramp toward ACTIVE that ends at t_end. */
static void retarget_mid_ramp(const dpower_t *start, uint32_t t, uint32_t t_end, bool restart)
{
    dpower_t d = *start;
    int before = dp_update(&d, t).level_pct;
    if (restart) {
        wrong_set_active_restart(&d, t, 95);
    } else {
        dp_set_active(&d, 95);
    }
    int after = dp_update(&d, t).level_pct;
    uint32_t u = t;
    dp_out_t o = run_to(&d, &u, t_end);
    if (restart) {
        assert(before >= 90 || t == t_end - DISP_RAMP_MS || o.level_pct != 95);
        return;
    }
    assert(after <= before && after * 100 >= before * 95 - 100);
    assert(o.level_pct == 95);
}

static void test_d4_no_jump_mid_ramp(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    run_to(&d, &t, DISP_HOLD_MS + DISP_RAMP_MS);
    dp_activity(&d, t, DP_CAUSE_TOUCH);
    dp_update(&d, t);
    const uint32_t wake = t;
    for (uint32_t e = 0; e <= DISP_RAMP_MS; e++) {
        dpower_t at = d;
        uint32_t u = wake;
        if (e > 0) {
            run_to(&at, &u, wake + e);
        }
        retarget_mid_ramp(&at, wake + e, wake + DISP_RAMP_MS, false);
        retarget_mid_ramp(&at, wake + e, wake + DISP_RAMP_MS, true);
    }
    dp_init(&d, 0);
    t = 1000;
    dp_update(&d, t);
    dp_bump(&d, t, 90);
    assert(dp_update(&d, t).level_pct == 90);
    for (uint32_t e = 0; e <= DISP_RAMP_MS; e++) {
        dpower_t at = d;
        uint32_t u = t;
        if (e > 0) {
            run_to(&at, &u, t + e);
        }
        retarget_mid_ramp(&at, t + e, t + DISP_RAMP_MS, false);
    }
}

static void test_d5_clamp_and_dim_below_active(void)
{
    dpower_t d;
    dp_init(&d, 0);
    dp_set_active(&d, 5);
    assert(d.active == BRT_MIN_PERCENT && dp_update(&d, 0).level_pct == BRT_MIN_PERCENT);
    dp_set_active(&d, 200);
    assert(d.active == 100 && dp_update(&d, 0).level_pct == 100);
    const int ladder[] = {20, 21, 22, 23, 24, 26, 27, 28, 30, 31, 33, 35, 37, 38, 40, 43, 45,
                          47, 49, 52, 55, 58, 60, 64, 67, 70, 74, 78, 82, 86, 90, 95, 100};
    for (int i = 0; i < (int)(sizeof(ladder) / sizeof(ladder[0])); i++) {
        dp_init(&d, 0);
        dp_set_active(&d, ladder[i]);
        uint32_t t = 0;
        dp_out_t act = run_to(&d, &t, DISP_HOLD_MS - 1);
        int prev = act.level_pct;
        while (t < DISP_HOLD_MS + DISP_RAMP_MS) {
            dp_out_t o = run_to(&d, &t, t + 7);
            assert(o.level_pct <= prev);
            prev = o.level_pct;
        }
        assert(prev == DISP_DIM_PERCENT && DISP_DIM_PERCENT < act.level_pct && act.level_pct == ladder[i]);
        dp_activity(&d, t, DP_CAUSE_TOUCH);
        const uint32_t wake = t;
        while (t < wake + DISP_RAMP_MS) {
            dp_out_t o = run_to(&d, &t, t + 7);
            assert(o.level_pct >= prev);
            prev = o.level_pct;
        }
        assert(prev == ladder[i]);
    }
    dp_init(&d, 0);
    d.active = d.ramp_to = d.ramp_from = d.level = 5;
    uint32_t t = 0;
    int active_level = run_to(&d, &t, 1000).level_pct, dim_level = run_to(&d, &t, DISP_HOLD_MS + DISP_RAMP_MS).level_pct;
    /* Without the clamp, 5 puts DIM above ACTIVE: the red proof. */
    assert(dim_level > active_level);
}

static void test_d6_set_while_dark(void)
{
    for (int dark = 0; dark < 2; dark++) {
        dpower_t d, w;
        dp_init(&d, 0);
        uint32_t t = 0;
        uint32_t settle = dark ? DISP_HOLD_MS + DISP_DIM_TO_OFF_MS + DISP_RAMP_MS : DISP_HOLD_MS + DISP_RAMP_MS;
        dp_out_t o = run_to(&d, &t, settle);
        int dark_level = dark ? DISP_FLOOR_PERCENT : DISP_DIM_PERCENT;
        assert(o.level_pct == dark_level && o.stage == (dark ? DP_OFF : DP_DIM));
        w = d;
        dp_set_active(&d, 64);
        o = run_to(&d, &t, t + 1000);
        assert(o.level_pct == dark_level);
        dp_activity(&d, t, DP_CAUSE_TOUCH);
        const uint32_t wake = t;
        o = dp_update(&d, t);
        assert(o.level_pct == dark_level);
        o = run_to(&d, &t, wake + DISP_RAMP_MS / 2);
        assert(o.level_pct > dark_level && o.level_pct < 64);
        o = run_to(&d, &t, wake + DISP_RAMP_MS);
        assert(o.level_pct == 64);
        wrong_set_active_jump(&w, 64);
        assert(dp_update(&w, settle).level_pct == 64);
    }
}

static void test_d7_bump(void)
{
    dpower_t d, twin;
    dp_init(&d, 0);
    dp_set_active(&d, 64);
    uint32_t t = 1000;
    dp_update(&d, t);
    dp_bump(&d, t, 55);
    assert(dp_update(&d, t).level_pct == 55);
    const uint32_t bumped = t;
    int prev = 55;
    while (t < bumped + DISP_RAMP_MS) {
        dp_out_t o = run_to(&d, &t, t + 5);
        assert(o.level_pct >= prev && o.level_pct <= 64);
        prev = o.level_pct;
    }
    assert(prev == 64);

    dp_init(&d, 0);
    t = 0;
    run_to(&d, &t, DISP_HOLD_MS + DISP_RAMP_MS);
    dp_activity(&d, t, DP_CAUSE_TOUCH);
    dp_update(&d, t);
    run_to(&d, &t, t + 100);
    twin = d;
    dp_bump(&d, t, 55);
    for (uint32_t end = t + DISP_RAMP_MS; t < end;) {
        uint32_t u = t;
        assert(run_to(&d, &t, t + 16).level_pct == run_to(&twin, &u, u + 16).level_pct);
    }

    dp_init(&d, 0);
    t = 0;
    run_to(&d, &t, DISP_HOLD_MS + DISP_RAMP_MS);
    dp_bump(&d, t, 55);
    assert(dp_update(&d, t).level_pct == DISP_DIM_PERCENT);
}

int main(void)
{
    test_timeline_from(0);
    test_timeline_from(12345);
    test_timeline_from(0xFFFFFFFFu - 100000u);
    test_timeline_from(0xFFFFFFFFu - 1);
    test_activity_returns_active_from_every_point();
    test_non_activity_state_changes();
    test_attention_states();
    test_offline_never_brightens();
    test_force();
    test_orbit();
    test_ramp();
    test_fade_out_then_off();
    test_wake_during_fade_out();
    test_wake_from_off_starts_at_floor();
    test_dim_to_off();
    test_touch_gate();
    test_setdown();
    test_attention_brightening_cause();
    test_setdown_hold_reported();
    test_setdown_then_new_pickup();
    test_orbit_keeps_drawing_in_scene();
    test_stale_timestamps_stay_off();
    test_d2_boot_level();
    test_d3_follows_at_once();
    test_d4_no_jump_mid_ramp();
    test_d5_clamp_and_dim_below_active();
    test_d6_set_while_dark();
    test_d7_bump();
    puts("ok");
    return 0;
}
