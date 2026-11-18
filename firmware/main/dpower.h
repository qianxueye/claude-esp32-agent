#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

/* Display power timeline: activity events and a millisecond clock in, stage and panel level out. Model in docs/display-power.md. */

typedef enum { DP_ACTIVE, DP_DIM, DP_OFF } dp_stage_t;
typedef enum { DP_CAUSE_NONE, DP_CAUSE_BOOT, DP_CAUSE_TOUCH, DP_CAUSE_BUTTON, DP_CAUSE_STATE, DP_CAUSE_MOTION, DP_CAUSE_TIMER, DP_CAUSE_FORCE, DP_CAUSE_SETDOWN, DP_CAUSE_N } dp_cause_t;

/* Same numbering as the device states in docs/protocol.md. */
enum { DP_STATE_IDLE, DP_STATE_LISTENING, DP_STATE_THINKING, DP_STATE_SPEAKING, DP_STATE_ERROR, DP_STATE_OFFLINE };

typedef struct {
    dp_stage_t stage;
    int level_pct;
    bool panel_on;
    dp_cause_t cause;
    uint32_t hold_ms;
} dp_out_t;

typedef struct {
    uint32_t last_act, last_lift, state_since, ramp_t0;
    int state, level, force, ramp_from, ramp_to;
    bool hold, lift_seen, quick;
    dp_cause_t cause;
} dpower_t;

void dp_init(dpower_t *d, uint32_t now);
/* Touch, button or pickup (DP_CAUSE_MOTION) activity. */
void dp_activity(dpower_t *d, uint32_t now, dp_cause_t cause);
/* A rest ended a pickup: in IDLE the pickup hold shrinks to DISP_SETDOWN_HOLD_MS. */
void dp_setdown(dpower_t *d, uint32_t now);
/* Reports the device state; a change into or out of LISTENING, THINKING, SPEAKING or ERROR counts as activity, unless OFFLINE is one end. */
void dp_state(dpower_t *d, uint32_t now, int state);
/* Pins the stage to DP_ACTIVE, DP_DIM or DP_OFF; a negative value releases it. */
void dp_force(dpower_t *d, int stage);
dp_out_t dp_update(dpower_t *d, uint32_t now);
/* Position of lattice step index (modulo the cycle length) in pixels. */
void dp_orbit(int index, int *x_px, int *y_px);

/* A touch that begins while the screen is DIM or OFF only wakes it. */
typedef struct {
    bool consumed;
} dp_touch_t;

/* Call when the press starts, with the stage shown before the wake is posted. */
void dp_touch_press(dp_touch_t *t, dp_stage_t stage_at_press);
/* True when the release (TAP) or the long hold of this touch may be sent. */
bool dp_touch_sends(const dp_touch_t *t);
