#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "apack.h"

/* Which pack clip and frame to show on each UI tick; pure, no ESP-IDF includes. */

#define APLAY_CLAWD (-1)   /* draw the built-in mascot */
#define APLAY_NOTHING (-2) /* panel off: hide the layer, decode nothing */
#define APLAY_STATE_IDLE 0 /* same numbering as the device states in docs/protocol.md */

typedef enum { APS_ACTIVE, APS_DIM, APS_OFF } aplay_stage_t;

typedef struct {
    int clip, frame; /* clip >= 0 is a pack clip, else APLAY_CLAWD or APLAY_NOTHING */
} aplay_out_t;

typedef struct {
    const ap_pack_t *pack;
    int (*ready)(void *ctx, int clip); /* frames of the clip that may be shown; NULL means all */
    void *ready_ctx;
    bool enabled, pickup, running, once, dim, stalled;
    int pin, state, base, clip, frame;
    uint32_t due, steps, late, cuts, starve;
} aplay_t;

/* A NULL pack always gives Clawd; a valid pack starts enabled. */
void aplay_init(aplay_t *a, const ap_pack_t *pack);
/* The player never steps to a frame at or past ready(clip); each stalled frame counts one starve. */
void aplay_set_ready(aplay_t *a, int (*ready)(void *ctx, int clip), void *ctx);
void aplay_enable(aplay_t *a, bool on);
/* Plays one clip whatever the state, as its loop flag says; -1 or an unknown clip releases the pin. */
void aplay_pin(aplay_t *a, int clip);
/* Pickup edge; the next tick plays the pickup clip once if it finds IDLE and ACTIVE, else drops it. */
void aplay_pickup(aplay_t *a);
aplay_out_t aplay_tick(aplay_t *a, uint32_t now_ms, int state, aplay_stage_t stage);
