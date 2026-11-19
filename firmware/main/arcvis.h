#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

/* Rim meter visibility, one meter at a time: shown while a gesture is armed and VOL_ARC_LINGER_MS after it ends; yields what to repaint per frame. */

enum { AV_VOL, AV_BRT };

typedef struct {
    bool armed, linger;
    int kind, val, quad_latch; /* quad_latch: the quadrant the meter stays at, -1 to follow the target */
    int force, force_kind, force_over; /* force: test-aid level, -1 none; force_over: test-aid stretch */
    int over; /* requested stretch of the armed meter: cells past the end, negative at the low end */
    uint32_t end_ms;
    bool drawn;
    int drawn_val, drawn_quad, drawn_kind, drawn_over;
} arcvis_t;

enum { AV_CLEAR_OLD = 1, AV_DRAW_NEW = 2, AV_DIFF = 4, AV_OVER = 8 };

typedef struct {
    int flags;
    int old_quad, old_kind; /* AV_CLEAR_OLD clears the whole meter of old_kind at old_quad */
    int quad, kind, val; /* AV_DRAW_NEW draws the meter of kind at quad with val */
    int from, to; /* AV_DIFF repaints the change between values from and to at quad */
    int old_over; /* AV_CLEAR_OLD: the stretch drawn with the old meter */
    int over_from, over_to; /* AV_DRAW_NEW draws the stretch over_to; AV_OVER repaints the change from over_from to over_to */
} avact_t;

void av_init(arcvis_t *v);
/* The knob arms: shows the volume arc at volume val, taking over a lingering meter of the same kind without a clear. */
void av_arm(arcvis_t *v, int val);
/* A gesture of kind arms at val; a meter of the other kind is cleared in the same frame. */
void av_arm_kind(arcvis_t *v, int kind, int val, int quad_latch);
void av_set(arcvis_t *v, int val);
/* The gesture ends at val (the restored value after a cancel); the meter lingers and its drawn stretch springs back. */
void av_end(arcvis_t *v, int val, uint32_t now);
void av_force(arcvis_t *v, int val);
void av_force_kind(arcvis_t *v, int kind, int val);
void av_force_over(arcvis_t *v, int over);
/* Sets the requested stretch of the armed meter; it shows only while the value rests at the end the sign names. */
void av_over(arcvis_t *v, int over);
/* Stretch cells left t ms after the gesture ended with over0 drawn: one fewer at once, then one per ARC_SPRING_STEP_MS. */
int av_spring(int over0, uint32_t elapsed_ms);
bool av_visible(const arcvis_t *v, uint32_t now);
/* Compares the wanted meter with the drawn one at this frame and records the result as drawn. */
avact_t av_frame(arcvis_t *v, uint32_t now, int quad);
