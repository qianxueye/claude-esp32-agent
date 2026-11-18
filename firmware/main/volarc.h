#pragma once
#include <stdint.h>
#include "app_config.h"

/* Volume arc geometry in panel pixels: a pixel table sorted by percent step, and the invalidation areas of a change. Model in docs/mascot.md. */

#define VOLARC_CAP (((VOL_ARC_R_OUT_PX * VOL_ARC_R_OUT_PX - VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX) * 26) / 10 + 128)
#define VOLARC_MAX_AREAS 10 /* one area per VOL_ARC_CHUNK_THR percent of the whole arc */

typedef struct {
    uint32_t *e; /* caller storage, VOLARC_CAP entries */
    int n;
    int cnt[101]; /* entries with threshold <= index */
} volarc_t;

typedef struct {
    int x1, y1, x2, y2; /* inclusive, as lv_area_t */
} volarc_rect_t;

enum { VOLARC_TRACK, VOLARC_FILL, VOLARC_MUTE };

/* Builds the table for quadrant 0; returns the entry count, or -1 when the storage is too small. */
int volarc_build(volarc_t *a, uint32_t *storage, int cap);
/* Panel position of entry e with the arc turned by quad clockwise quarter turns. */
void volarc_xy(uint32_t e, int quad, int *x, int *y);
int volarc_thr(uint32_t e);
/* Colour class of a pixel with threshold thr at volume v. */
int volarc_class(int thr, int v);
/* Thresholds (lo, hi] whose colour differs between volumes a and b. */
void volarc_change_range(int a, int b, int *lo, int *hi);
/* Fills out with the bounding boxes of the pixels with threshold in (lo, hi], one per chunk; returns the count. */
int volarc_areas(const volarc_t *a, int quad, int lo, int hi, volarc_rect_t *out, int max);
