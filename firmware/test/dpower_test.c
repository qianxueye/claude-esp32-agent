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
    assert(o.stage == DP_DIM && o.level_pct == 56);
    o = run_to(&d, &t, t0 + DISP_HOLD_MS + 80);
    assert(o.level_pct == 32);
    o = run_to(&d, &t, t0 + DISP_HOLD_MS + 160);
    assert(o.level_pct == 18);
    o = run_to(&d, &t, t0 + DISP_HOLD_MS + 240);
    assert(o.level_pct == DISP_DIM_PERCENT && o.stage == DP_DIM && o.panel_on);
    o = run_to(&d, &t, t0 + DISP_HOLD_MS + DISP_DIM_TO_OFF_MS - 1);
    assert(o.stage == DP_DIM && o.level_pct == DISP_DIM_PERCENT);
    o = run_to(&d, &t, t0 + DISP_HOLD_MS + DISP_DIM_TO_OFF_MS);
    assert(o.stage == DP_OFF && !o.panel_on);
    o = run_to(&d, &t, t0 + 10u * 3600u * 1000u);
    assert(o.stage == DP_OFF);
}

static void test_activity_returns_active_from_every_point(void)
{
    const uint32_t span = DISP_HOLD_MS + DISP_DIM_TO_OFF_MS + 60000u;
    for (uint32_t at = 0; at < span; at += 997) {
        for (int cause = DP_CAUSE_TOUCH; cause <= DP_CAUSE_MOTION; cause += 1) {
            if (cause == DP_CAUSE_STATE) {
                continue;
            }
            dpower_t d;
            dp_init(&d, 0);
            uint32_t t = 0;
            run_to(&d, &t, at);
            dp_activity(&d, t, (dp_cause_t)cause);
            dp_out_t o = dp_update(&d, t);
            assert(o.stage == DP_ACTIVE && o.level_pct == DISP_ACTIVE_PERCENT && o.panel_on && (int)o.cause == cause);
            o = run_to(&d, &t, t + DISP_HOLD_MS - 1);
            assert(o.stage == DP_ACTIVE);
            o = run_to(&d, &t, t + 1);
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
    assert(o.stage == DP_OFF && !o.panel_on && o.cause == DP_CAUSE_FORCE);
    dp_force(&d, DP_DIM);
    o = dp_update(&d, 32);
    assert(o.stage == DP_DIM && o.panel_on);
    dp_force(&d, -1);
    o = dp_update(&d, 48);
    assert(o.stage == DP_ACTIVE && o.level_pct == DISP_ACTIVE_PERCENT);
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

/* Motion activity every tick for three hours, from a clock start that may sit on the wrap. */
static void test_storm_budget_from(uint32_t t0)
{
    const uint32_t hour = DISP_STORM_PERIOD_MS, total = 3u * hour;
    const uint32_t n = total / TICK;
    uint8_t *active = calloc(n + 1, 1);
    assert(active);
    dpower_t d;
    dp_init(&d, t0);
    uint32_t t = t0;
    dp_out_t o;
    uint32_t first_dim = 0;
    for (uint32_t i = 0; i < n; i++) {
        t += TICK;
        dp_activity(&d, t, DP_CAUSE_MOTION);
        o = dp_update(&d, t);
        active[i] = o.stage == DP_ACTIVE;
        assert(o.stage != DP_OFF);
        if (!first_dim && o.stage == DP_DIM) {
            first_dim = i * TICK;
        }
    }
    /* Boot activity holds ACTIVE for DISP_HOLD_MS before the budget starts to count. */
    assert(first_dim >= DISP_HOLD_MS + DISP_STORM_BUDGET_MS - 1000 && first_dim <= DISP_HOLD_MS + DISP_STORM_BUDGET_MS + 1000);
    uint32_t win = hour / TICK, sum = 0, worst = 0;
    for (uint32_t i = 0; i < n; i++) {
        sum += active[i];
        if (i >= win) {
            sum -= active[i - win];
        }
        worst = sum > worst ? sum : worst;
    }
    assert((uint64_t)worst * TICK <= (uint64_t)DISP_STORM_BUDGET_MS + hour / DP_STORM_BINS);
    uint32_t all = 0;
    for (uint32_t i = 0; i < n; i++) {
        all += active[i];
    }
    assert((uint64_t)all * TICK >= 2u * DISP_STORM_BUDGET_MS);
    free(active);

    t += 1000;
    dp_activity(&d, t, DP_CAUSE_TOUCH);
    o = dp_update(&d, t);
    assert(o.stage == DP_ACTIVE);
}

static void test_storm_dim_from_off(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    run_to(&d, &t, DISP_HOLD_MS + DISP_DIM_TO_OFF_MS + 1000);
    assert(dp_update(&d, t).stage == DP_OFF);
    dp_activity(&d, t, DP_CAUSE_MOTION);
    dp_out_t o = dp_update(&d, t);
    assert(o.stage == DP_ACTIVE && o.panel_on && o.cause == DP_CAUSE_MOTION);
    for (uint32_t i = 0; i < DISP_STORM_BUDGET_MS / TICK + 100; i++) {
        t += TICK;
        dp_activity(&d, t, DP_CAUSE_MOTION);
        dp_update(&d, t);
    }
    run_to(&d, &t, t + DISP_HOLD_MS + DISP_DIM_TO_OFF_MS + 5000);
    assert(dp_update(&d, t).stage == DP_OFF);
    dp_activity(&d, t, DP_CAUSE_MOTION);
    o = dp_update(&d, t);
    assert(o.stage == DP_DIM && o.panel_on && o.level_pct == DISP_DIM_PERCENT);
}

static void test_touch_does_not_spend_budget(void)
{
    dpower_t d;
    dp_init(&d, 0);
    uint32_t t = 0;
    for (uint32_t i = 0; i < 2u * DISP_STORM_BUDGET_MS / TICK; i++) {
        t += TICK;
        dp_activity(&d, t, DP_CAUSE_TOUCH);
        assert(dp_update(&d, t).stage == DP_ACTIVE);
    }
    dp_activity(&d, t, DP_CAUSE_MOTION);
    run_to(&d, &t, t + DISP_HOLD_MS - 1);
    assert(dp_update(&d, t).stage == DP_ACTIVE);
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
    test_storm_budget_from(0);
    test_storm_budget_from(0xFFFFFFFFu - 1000000u);
    test_storm_dim_from_off();
    test_touch_does_not_spend_budget();
    test_orbit();
    puts("ok");
    return 0;
}
