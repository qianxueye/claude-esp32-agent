#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

/* Display power timeline: activity events and a millisecond clock in, stage and panel level out. Model in docs/display-power.md. */

typedef enum { DP_ACTIVE, DP_DIM, DP_OFF } dp_stage_t;
typedef enum { DP_CAUSE_NONE, DP_CAUSE_BOOT, DP_CAUSE_TOUCH, DP_CAUSE_BUTTON, DP_CAUSE_STATE, DP_CAUSE_MOTION, DP_CAUSE_TIMER, DP_CAUSE_FORCE, DP_CAUSE_SMALL, DP_CAUSE_SETDOWN, DP_CAUSE_BUDGET, DP_CAUSE_N } dp_cause_t;

/* Same numbering as the device states in docs/protocol.md. */
enum { DP_STATE_IDLE, DP_STATE_LISTENING, DP_STATE_THINKING, DP_STATE_SPEAKING, DP_STATE_ERROR, DP_STATE_OFFLINE };

typedef struct {
    dp_stage_t stage;
    int level_pct;
    bool panel_on;
    dp_cause_t cause;
} dp_out_t;

typedef struct {
    uint32_t last_act, last_lift, last_imu, state_since, ramp_t0, last_update, bin_start;
    uint32_t bins[DISP_STORM_BINS];
    int state, level, force, bin_idx;
    bool ramping, hold, lift_seen, imu_seen, exhausted;
    dp_cause_t cause;
} dpower_t;

void dp_init(dpower_t *d, uint32_t now);
/* Touch, button, pickup (DP_CAUSE_MOTION) or small-motion (DP_CAUSE_SMALL) activity; only small motion is limited to DISP_STORM_BUDGET_MS per DISP_STORM_PERIOD_MS. */
void dp_activity(dpower_t *d, uint32_t now, dp_cause_t cause);
/* A desk rest ended a pickup: in IDLE the pickup hold shrinks to DISP_SETDOWN_HOLD_MS. */
void dp_setdown(dpower_t *d, uint32_t now);
/* Reports the device state; a change into or out of LISTENING, THINKING, SPEAKING or ERROR counts as activity, unless OFFLINE is one end. */
void dp_state(dpower_t *d, uint32_t now, int state);
/* Pins the stage to DP_ACTIVE, DP_DIM or DP_OFF; a negative value releases it. */
void dp_force(dpower_t *d, int stage);
dp_out_t dp_update(dpower_t *d, uint32_t now);
/* Position of lattice step index (modulo the cycle length) in pixels. */
void dp_orbit(int index, int *x_px, int *y_px);
