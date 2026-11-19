#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

/* Rim arc geometry in panel pixels: a pixel table sorted by step, and the invalidation areas of a change. Model in docs/mascot.md. */

#define VOLARC_CAP (((VOL_ARC_R_OUT_PX * VOL_ARC_R_OUT_PX - VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX) * 26) / 10 + 128)
#define VOLARC_MAX_AREAS 10 /* one area per VOL_ARC_CHUNK_THR percent of the whole arc */
#define VOLARC_RING_AREAS 21 /* mathematical model: the areas invalidate_ring emits for the ring band at the configured diameter and band height */
#define VOLARC_RING_MAX_AREAS 1 /* mathematical model: the ring redraws only when the arc change has at most this many areas */
#define VOLARC_RING_MAX_BANDS (32 - VOLARC_RING_AREAS - VOLARC_RING_MAX_AREAS) /* mathematical model: LVGL's 32 areas less the ring and one arc area; the ring also needs a tick of at most this many scene bands */

/* Whether the ring may redraw in a tick whose arc change has arc_areas areas and whose scene has bands bands. */
static inline bool volarc_ring_ok(int arc_areas, int bands)
{
    return arc_areas <= VOLARC_RING_MAX_AREAS && bands <= VOLARC_RING_MAX_BANDS;
}

/* Mathematical model: the two stretch runs take 2 * ARC_OVER_CELLS percent of the sweep, either arc, plus a half-disc (pi/8 of the band width squared, bounded by 4/10, and one band width of edge pixels) at each tip. */
#define VOLARC_OVER_CAP (VOLARC_CAP * 2 * ARC_OVER_CELLS / 100 + 2 * (((VOL_ARC_R_OUT_PX - VOL_ARC_R_IN_PX) * (VOL_ARC_R_OUT_PX - VOL_ARC_R_IN_PX) * 4) / 10 + (VOL_ARC_R_OUT_PX - VOL_ARC_R_IN_PX)))

#ifdef VOLARC_COUNT_TRIG
/* Host test build only: trigonometric calls made by the table builders. */
typedef struct {
    long atan2_calls, cossin_calls;
} volarc_trig_t;
extern volarc_trig_t volarc_trig;
#endif

typedef struct {
    uint32_t *e; /* caller storage, VOLARC_CAP entries */
    int n, steps;
    int cnt[101]; /* entries with threshold <= index */
} volarc_t;

typedef struct {
    int x1, y1, x2, y2; /* inclusive, as lv_area_t */
} volarc_rect_t;

enum { VOLARC_TRACK, VOLARC_FILL, VOLARC_MUTE };

/* Builds the volume table for quadrant 0; returns the entry count, or -1 when the storage is too small. */
int volarc_build(volarc_t *a, uint32_t *storage, int cap);
/* As volarc_build for steps equal cells from UI angle start_deg over sweep_deg clockwise; steps is at most 100. */
int volarc_build_seg(volarc_t *a, uint32_t *storage, int cap, double start_deg, double sweep_deg, int steps);
/* Panel position of entry e with the arc turned by quad clockwise quarter turns. */
void volarc_xy(uint32_t e, int quad, int *x, int *y);
int volarc_thr(uint32_t e);
/* Colour class of a pixel with threshold thr at volume v. */
int volarc_class(int thr, int v);
/* Thresholds (lo, hi] whose colour differs between volumes a and b. */
void volarc_change_range(int a, int b, int *lo, int *hi);
/* Colour class of brightness cell thr (1-based) at ladder index bi: the lowest cell is lit even at the minimum. */
int volarc_seg_class(int thr, int bi);
/* Cells (lo, hi] whose colour differs between ladder indices a and b. */
void volarc_seg_change_range(int a, int b, int *lo, int *hi);
/* Fills out with the bounding boxes of the pixels with threshold in (lo, hi], one per chunk; returns the count. */
int volarc_areas(const volarc_t *a, int quad, int lo, int hi, volarc_rect_t *out, int max);
/* As volarc_areas with chunk thresholds per area. */
int volarc_areas_chunk(const volarc_t *a, int quad, int lo, int hi, int chunk, volarc_rect_t *out, int max);
/* Builds the end-cue table of the meter built with the same arguments: thresholds 1..N are the low-end cells (1 touches the end), N+1..2N the high-end cells (N+1 touches it). */
int volarc_build_over(volarc_t *a, uint32_t *storage, int cap, double start_deg, double sweep_deg, int steps);
/* Whether end-cue cell thr is lit at stretch over (negative at the low end, positive at the high end). */
int volarc_over_lit(int thr, int over);
/* Threshold ranges (lo, hi] of the end-cue cells whose lit state differs between stretches from and to; returns the count, at most 2. */
int volarc_over_ranges(int from, int to, int lo[2], int hi[2]);
/* Colour class of a lit end-cue cell: the class of the meter's end cell at that end. */
int volarc_over_class(int seg, int over);
/* Unions extra into the area of rects[0] (end below 0) or rects[n - 1]; returns n. */
int volarc_fold_rect(volarc_rect_t *rects, int n, int end, volarc_rect_t extra);
/* The areas of a whole meter (one per chunk) with its lit stretch folded into the chunk holding the end it touches; the count equals the chunk count. */
int volarc_whole_areas(const volarc_t *m, const volarc_t *o, int quad, int chunk, int stretch, volarc_rect_t *out, int max);
/* The areas of the end-cue cells whose lit state differs between stretches from and to: one per changed end. */
int volarc_over_areas(const volarc_t *o, int quad, int from, int to, volarc_rect_t out[2]);
