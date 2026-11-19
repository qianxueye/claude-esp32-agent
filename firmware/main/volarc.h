#pragma once
#include <stdint.h>
#include "app_config.h"

/* Rim arc geometry in panel pixels: a pixel table sorted by step, and the invalidation areas of a change. Model in docs/mascot.md. */

#define VOLARC_CAP (((VOL_ARC_R_OUT_PX * VOL_ARC_R_OUT_PX - VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX) * 26) / 10 + 128)
#define VOLARC_MAX_AREAS 10 /* one area per VOL_ARC_CHUNK_THR percent of the whole arc */
#define VOLARC_RING_MAX_AREAS 1 /* mathematical model: LVGL's 32 areas less the scene's 10 and the ring's 21; the ring redraws only when the arc change has at most this many */

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
