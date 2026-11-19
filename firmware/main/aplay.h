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
    bool enabled, pickup, running, once, dim;
    int pin, state, base, clip, frame;
    uint32_t due, steps, late, cuts;
} aplay_t;

/* A NULL pack always gives Clawd; a valid pack starts enabled. */
void aplay_init(aplay_t *a, const ap_pack_t *pack);
void aplay_enable(aplay_t *a, bool on);
/* Plays one clip in a loop whatever the state; -1 or an unknown clip releases the pin. */
void aplay_pin(aplay_t *a, int clip);
/* Pickup edge; the next tick plays the pickup clip once if it finds IDLE and ACTIVE, else drops it. */
void aplay_pickup(aplay_t *a);
aplay_out_t aplay_tick(aplay_t *a, uint32_t now_ms, int state, aplay_stage_t stage);
