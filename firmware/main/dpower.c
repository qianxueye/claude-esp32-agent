#include "dpower.h"
#include <math.h>
#include "app_config.h"

/* Timestamps older than this are held at it, so a 32-bit clock wrap cannot make them look recent. */
#define STALE_AGE 0x80000000u

static bool is_attention(int state)
{
    return state == DP_STATE_LISTENING || state == DP_STATE_THINKING || state == DP_STATE_SPEAKING || state == DP_STATE_ERROR;
}

/* Elapsed milliseconds, correct across the 32-bit clock wrap. */
static uint32_t since(uint32_t now, uint32_t then)
{
    return now - then;
}

static dp_stage_t stage_after(uint32_t elapsed)
{
    if (elapsed < DISP_HOLD_MS) {
        return DP_ACTIVE;
    }
    return elapsed < (uint32_t)DISP_HOLD_MS + DISP_DIM_TO_OFF_MS ? DP_DIM : DP_OFF;
}

static int ramp_level(int n)
{
    if (n >= DISP_RAMP_STEPS) {
        return DISP_DIM_PERCENT;
    }
    float ratio = (float)DISP_DIM_PERCENT / DISP_ACTIVE_PERCENT;
    return (int)lroundf(DISP_ACTIVE_PERCENT * powf(ratio, (float)n / DISP_RAMP_STEPS));
}

void dp_init(dpower_t *d, uint32_t now)
{
    *d = (dpower_t){0};
    d->last_update = now;
    d->bin_start = now;
    d->last_act = now;
    d->state_since = now;
    d->ramp_t0 = now;
    d->state = DP_STATE_IDLE;
    d->level = DISP_ACTIVE_PERCENT;
    d->force = -1;
    d->ramping = false;
    d->hold = false;
    d->cause = DP_CAUSE_BOOT;
}

void dp_activity(dpower_t *d, uint32_t now, dp_cause_t cause)
{
    if (cause == DP_CAUSE_SMALL) {
        d->last_imu = now;
        d->imu_seen = true;
    } else if (cause == DP_CAUSE_MOTION) {
        d->last_lift = now;
        d->lift_seen = true;
    } else {
        d->last_act = now;
    }
    d->cause = cause;
}

void dp_setdown(dpower_t *d, uint32_t now)
{
    const uint32_t shrink = (uint32_t)DISP_HOLD_MS - DISP_SETDOWN_HOLD_MS;
    if (d->state == DP_STATE_IDLE && d->lift_seen && since(now, d->last_lift) < shrink) {
        d->last_lift = now - shrink;
    }
}

void dp_state(dpower_t *d, uint32_t now, int state)
{
    if (state == d->state) {
        return;
    }
    bool act = d->state != DP_STATE_OFFLINE && state != DP_STATE_OFFLINE && (is_attention(d->state) || is_attention(state));
    d->state = state;
    d->state_since = now;
    d->hold = act && is_attention(state);
    if (act) {
        dp_activity(d, now, DP_CAUSE_STATE);
    }
}

void dp_force(dpower_t *d, int stage)
{
    d->force = stage > DP_OFF ? DP_OFF : (stage < 0 ? -1 : stage);
}

static dp_stage_t pick_stage(const dpower_t *d, uint32_t now, bool *imu_only, bool *clamped)
{
    dp_stage_t st = stage_after(since(now, d->last_act));
    if (is_attention(d->state)) {
        if (d->hold && since(now, d->state_since) < DISP_ATTN_CAP_MS) {
            st = DP_ACTIVE;
        } else if (st == DP_OFF) {
            st = DP_DIM;
        }
    }
    if (d->lift_seen) {
        dp_stage_t m = stage_after(since(now, d->last_lift));
        st = m < st ? m : st;
    }
    dp_stage_t full = st;
    *clamped = false;
    if (d->imu_seen) {
        dp_stage_t m = stage_after(since(now, d->last_imu));
        if (d->exhausted && m == DP_ACTIVE) {
            m = DP_DIM;
            *clamped = m < st;
        }
        if (m < st) {
            st = m;
        }
    }
    *imu_only = st == DP_ACTIVE && full != DP_ACTIVE;
    return st;
}

static void advance_bins(dpower_t *d, uint32_t now)
{
    const uint32_t width = DISP_STORM_PERIOD_MS / DISP_STORM_BINS;
    uint32_t el = since(now, d->bin_start);
    if (el < width) {
        return;
    }
    uint32_t n = el / width;
    if (n >= DISP_STORM_BINS) {
        for (int i = 0; i < DISP_STORM_BINS; i++) {
            d->bins[i] = 0;
        }
        d->bin_start = now;
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        d->bin_idx = (d->bin_idx + 1) % DISP_STORM_BINS;
        d->bins[d->bin_idx] = 0;
    }
    d->bin_start += n * width;
}

static uint32_t storm_used(const dpower_t *d)
{
    uint32_t sum = 0;
    for (int i = 0; i < DISP_STORM_BINS; i++) {
        sum += d->bins[i];
    }
    return sum;
}

static void saturate_age(uint32_t *stamp, uint32_t now)
{
    if (since(now, *stamp) > STALE_AGE) {
        *stamp = now - STALE_AGE;
    }
}

dp_out_t dp_update(dpower_t *d, uint32_t now)
{
    saturate_age(&d->last_act, now);
    saturate_age(&d->last_imu, now);
    saturate_age(&d->last_lift, now);
    saturate_age(&d->state_since, now);
    uint32_t dt = since(now, d->last_update);
    d->last_update = now;
    if (dt > DISP_STORM_PERIOD_MS / DISP_STORM_BINS) {
        dt = DISP_STORM_PERIOD_MS / DISP_STORM_BINS;
    }
    advance_bins(d, now);
    bool imu_only, clamped;
    dp_stage_t st = pick_stage(d, now, &imu_only, &clamped);
    if (imu_only) {
        d->bins[d->bin_idx] += dt;
    }
    bool exhausted = storm_used(d) >= DISP_STORM_BUDGET_MS;
    if (exhausted != d->exhausted) {
        d->exhausted = exhausted;
        st = pick_stage(d, now, &imu_only, &clamped);
    }
    if (d->force >= 0) {
        st = (dp_stage_t)d->force;
    }
    int want = st == DP_ACTIVE ? DISP_ACTIVE_PERCENT : DISP_DIM_PERCENT;
    if (want >= d->level) {
        d->level = want;
        d->ramping = false;
    } else {
        if (!d->ramping) {
            d->ramping = true;
            d->ramp_t0 = now;
        }
        int n = (int)(since(now, d->ramp_t0) / DISP_RAMP_STEP_MS) + 1;
        int lv = ramp_level(n);
        d->level = lv < want ? want : lv;
        if (d->level == want) {
            d->ramping = false;
        }
    }
    dp_cause_t cause = d->force >= 0 ? DP_CAUSE_FORCE : (st == DP_ACTIVE ? d->cause : (clamped ? DP_CAUSE_BUDGET : DP_CAUSE_TIMER));
    return (dp_out_t){.stage = st, .level_pct = d->level, .panel_on = st != DP_OFF, .cause = cause};
}

void dp_orbit(int index, int *x_px, int *y_px)
{
    const int cycle = ORBIT_COLS * ORBIT_ROWS, body = (ORBIT_COLS - 1) * ORBIT_ROWS;
    int i = ((index % cycle) + cycle) % cycle, col, row;
    if (i < body) {
        row = i / (ORBIT_COLS - 1);
        int k = i % (ORBIT_COLS - 1);
        col = (row & 1) ? ORBIT_COLS - 1 - k : 1 + k;
    } else {
        col = 0;
        row = ORBIT_ROWS - 1 - (i - body);
    }
    *x_px = ORBIT_X0_PX + col * ORBIT_PITCH_PX;
    *y_px = ORBIT_Y0_PX + row * ORBIT_PITCH_PX;
}
