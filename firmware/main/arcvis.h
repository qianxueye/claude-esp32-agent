#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

/* Volume arc visibility: shown while the knob is armed and VOL_ARC_LINGER_MS after it ends; yields what to repaint per frame. */

typedef struct {
    bool armed, linger;
    int val, force; /* force: test-aid level, -1 none */
    uint32_t end_ms;
    bool drawn;
    int drawn_val, drawn_quad;
} arcvis_t;

enum { AV_CLEAR_OLD = 1, AV_DRAW_NEW = 2, AV_DIFF = 4 };

typedef struct {
    int flags;
    int old_quad, quad, val; /* AV_CLEAR_OLD clears the whole arc at old_quad; AV_DRAW_NEW draws it at quad with val */
    int from, to; /* AV_DIFF repaints the change between volumes from and to at quad */
} avact_t;

void av_init(arcvis_t *v);
/* The knob arms: shows the arc at volume val, taking over a lingering arc without a clear. */
void av_arm(arcvis_t *v, int val);
void av_set(arcvis_t *v, int val);
/* The knob ends at volume val (the restored value after a cancel); the arc lingers. */
void av_end(arcvis_t *v, int val, uint32_t now);
void av_force(arcvis_t *v, int val);
bool av_visible(const arcvis_t *v, uint32_t now);
/* Compares the wanted arc with the drawn one at this frame and records the result as drawn. */
avact_t av_frame(arcvis_t *v, uint32_t now, int quad);
