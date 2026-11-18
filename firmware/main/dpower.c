#include "dpower.h"
#include <math.h>
#include "app_config.h"

_Static_assert(BRT_MIN_PERCENT >= 2 * DISP_DIM_PERCENT, "BRT_MIN_PERCENT must be at least twice DISP_DIM_PERCENT");

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

/* Equal-ratio interpolation in integer percent. */
static int ramp_level(int from, int to, uint32_t elapsed)
{
    if (elapsed >= DISP_RAMP_MS || from == to) {
        return to;
    }
    return (int)lroundf((float)from * powf((float)to / (float)from, (float)elapsed / DISP_RAMP_MS));
}

void dp_init(dpower_t *d, uint32_t now)
{
    *d = (dpower_t){0};
    d->last_act = now;
    d->state_since = now;
    d->ramp_t0 = now;
    d->state = DP_STATE_IDLE;
    d->level = DISP_ACTIVE_PERCENT;
    d->force = -1;
    d->ramp_from = d->ramp_to = DISP_ACTIVE_PERCENT;
    d->active = DISP_ACTIVE_PERCENT;
    d->hold = false;
    d->cause = DP_CAUSE_BOOT;
}

void dp_set_active(dpower_t *d, int pct)
{
    pct = pct < BRT_MIN_PERCENT ? BRT_MIN_PERCENT : pct > 100 ? 100 : pct;
    if (d->ramp_to == d->active) {
        if (d->level == d->active) {
            d->ramp_from = d->level = pct;
        }
        d->ramp_to = pct;
    }
    d->active = pct;
}

void dp_bump(dpower_t *d, uint32_t now, int pct)
{
    if (d->ramp_to != d->active || d->level != d->active) {
        return;
    }
    d->ramp_from = d->level = pct;
    d->ramp_t0 = now;
}

void dp_activity(dpower_t *d, uint32_t now, dp_cause_t cause)
{
    d->quick = false;
    if (cause == DP_CAUSE_MOTION) {
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
        d->quick = true;
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

static dp_stage_t pick_stage(const dpower_t *d, uint32_t now)
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
    return st;
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
    saturate_age(&d->last_lift, now);
    saturate_age(&d->state_since, now);
    saturate_age(&d->ramp_t0, now);
    dp_stage_t st = d->force >= 0 ? (dp_stage_t)d->force : pick_stage(d, now);
    int want = st == DP_ACTIVE ? d->active : (st == DP_DIM ? DISP_DIM_PERCENT : DISP_FLOOR_PERCENT);
    if (want != d->ramp_to) {
        d->ramp_from = d->level;
        d->ramp_to = want;
        d->ramp_t0 = now;
    }
    d->level = ramp_level(d->ramp_from, d->ramp_to, since(now, d->ramp_t0));
    bool faded = st == DP_OFF && since(now, d->ramp_t0) >= DISP_RAMP_MS;
    dp_cause_t cause = d->force >= 0 ? DP_CAUSE_FORCE : (st == DP_ACTIVE ? d->cause : (is_attention(d->state) ? DP_CAUSE_STATE : DP_CAUSE_TIMER));
    return (dp_out_t){.stage = st, .level_pct = d->level, .panel_on = !faded, .cause = cause, .hold_ms = d->quick ? DISP_SETDOWN_HOLD_MS : DISP_HOLD_MS};
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

void dp_touch_press(dp_touch_t *t, dp_stage_t stage_at_press)
{
    t->consumed = stage_at_press != DP_ACTIVE;
}

bool dp_touch_sends(const dp_touch_t *t)
{
    return !t->consumed;
}
