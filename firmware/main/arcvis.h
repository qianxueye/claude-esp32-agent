#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

/* Rim meter visibility, one meter at a time: shown while a gesture is armed and VOL_ARC_LINGER_MS after it ends; yields what to repaint per frame. */

enum { AV_VOL, AV_BRT };

typedef struct {
    bool armed, linger, blink;
    int kind, val, quad_latch; /* quad_latch: the quadrant the meter stays at, -1 to follow the target */
    int force, force_kind; /* force: test-aid level, -1 none */
    uint32_t end_ms, blink_ms;
    bool drawn;
    int drawn_val, drawn_quad, drawn_kind;
} arcvis_t;

enum { AV_CLEAR_OLD = 1, AV_DRAW_NEW = 2, AV_DIFF = 4 };

typedef struct {
    int flags;
    int old_quad, old_kind; /* AV_CLEAR_OLD clears the whole meter of old_kind at old_quad */
    int quad, kind, val; /* AV_DRAW_NEW draws the meter of kind at quad with val */
    int from, to; /* AV_DIFF repaints the change between values from and to at quad */
} avact_t;

void av_init(arcvis_t *v);
/* The knob arms: shows the volume arc at volume val, taking over a lingering meter of the same kind without a clear. */
void av_arm(arcvis_t *v, int val);
/* A gesture of kind arms at val; a meter of the other kind is cleared in the same frame. */
void av_arm_kind(arcvis_t *v, int kind, int val, int quad_latch);
void av_set(arcvis_t *v, int val);
/* The gesture ends at val (the restored value after a cancel); the meter lingers. */
void av_end(arcvis_t *v, int val, uint32_t now);
/* Blanks the meter for BRT_END_BLINK_MS from now, then draws it again. */
void av_blink(arcvis_t *v, uint32_t now);
void av_force(arcvis_t *v, int val);
void av_force_kind(arcvis_t *v, int kind, int val);
bool av_visible(const arcvis_t *v, uint32_t now);
/* Compares the wanted meter with the drawn one at this frame and records the result as drawn. */
avact_t av_frame(arcvis_t *v, uint32_t now, int quad);
