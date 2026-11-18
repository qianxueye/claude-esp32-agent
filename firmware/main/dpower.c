#include "dpower.h"
#include <math.h>
#include "app_config.h"

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
    d->last_act = now;
    d->cause = cause;
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
    d->force = stage;
}

static dp_stage_t pick_stage(const dpower_t *d, uint32_t now)
{
    dp_stage_t st = stage_after(since(now, d->last_act));
    if (is_attention(d->state)) {
        if (d->hold && since(now, d->state_since) < DISP_ATTN_CAP_MS) {
            return DP_ACTIVE;
        }
        st = st == DP_OFF ? DP_DIM : st;
    }
    return st;
}

dp_out_t dp_update(dpower_t *d, uint32_t now)
{
    dp_stage_t st = d->force >= 0 ? (dp_stage_t)d->force : pick_stage(d, now);
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
    dp_cause_t cause = d->force >= 0 ? DP_CAUSE_FORCE : (st == DP_ACTIVE ? d->cause : DP_CAUSE_TIMER);
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
