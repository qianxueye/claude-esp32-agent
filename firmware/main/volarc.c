#include "volarc.h"
#include <math.h>

#define PI_D 3.14159265358979323846
#define C2 (LCD_SIZE - 1) /* twice the centre coordinate of the pixel grid */

_Static_assert(VOL_ARC_R_IN_PX > LISTEN_RING_DIAMETER / 2 + 1, "arc must lie outside the listening ring");
_Static_assert(VOL_ARC_R_OUT_PX < LCD_SIZE / 2, "arc must lie inside the glass");
_Static_assert(VOL_ARC_R_IN_PX < VOL_ARC_R_OUT_PX, "arc band must not be empty");
_Static_assert(2 * ARC_OVER_CELLS * VOL_SWEEP_DEG <= (360 - VOL_SWEEP_DEG) * 100, "both volume end-cue runs must fit the gap of the volume arc");
_Static_assert(2 * ARC_OVER_CELLS * BRT_ARC_SWEEP_DEG <= (360 - BRT_ARC_SWEEP_DEG) * (BRT_STEPS + 1), "both brightness end-cue runs must fit outside its arc");
_Static_assert(2 * ARC_OVER_CELLS * VOL_SWEEP_DEG * VOL_ARC_R_IN_PX + 90 * (VOL_ARC_R_OUT_PX - VOL_ARC_R_IN_PX) * 100 <= (360 - VOL_SWEEP_DEG) * 100 * VOL_ARC_R_IN_PX,
               "the end-cue runs and their round caps must fit the gap of the volume arc");
_Static_assert(2 * ARC_OVER_CELLS * BRT_ARC_SWEEP_DEG * VOL_ARC_R_IN_PX + 90 * (VOL_ARC_R_OUT_PX - VOL_ARC_R_IN_PX) * (BRT_STEPS + 1) <= (360 - BRT_ARC_SWEEP_DEG) * (BRT_STEPS + 1) * VOL_ARC_R_IN_PX,
               "the end-cue runs and their round caps must fit outside the brightness arc");
_Static_assert(BRT_ARC_SWEEP_DEG * 100 * 10 <= (BRT_STEPS + 1) * VOL_SWEEP_DEG * 11, "VOLARC_OVER_CAP must hold the brightness cell pitch");

static int thr_at(int x, int y, double start_deg, double sweep_deg, int steps)
{
    double a = atan2((double)(2 * y - C2), (double)(2 * x - C2)) * 180.0 / PI_D;
    double t = a - start_deg;
    t -= 360.0 * floor(t / 360.0);
    if (t >= sweep_deg) {
        return 0;
    }
    return (int)floor(t * steps / sweep_deg) + 1;
}

static int in_band(int x, int y)
{
    int dx = 2 * x - C2, dy = 2 * y - C2, d2 = dx * dx + dy * dy;
    return d2 >= 4 * VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX && d2 <= 4 * VOL_ARC_R_OUT_PX * VOL_ARC_R_OUT_PX;
}

int volarc_build(volarc_t *a, uint32_t *storage, int cap)
{
    return volarc_build_seg(a, storage, cap, VOL_ARC_START_DEG, VOL_SWEEP_DEG, 100);
}

typedef struct {
    double start, sweep;
    int steps;
} geom_t;

static double rel_t(int x, int y, double start)
{
    double t = atan2((double)(2 * y - C2), (double)(2 * x - C2)) * 180.0 / PI_D - start;
    return t - 360.0 * floor(t / 360.0);
}

/* Round cap, mathematical model: half the band width closes the band with a half-disc of that radius on the arc's middle radius. */
static int in_cap(int x, int y, const geom_t *g, int high, int k)
{
    double pitch = g->sweep / g->steps, t = rel_t(x, y, g->start);
    double th = (g->start + (high ? g->sweep + k * pitch : -k * pitch)) * PI_D / 180.0;
    double mid = VOL_ARC_R_IN_PX + VOL_ARC_R_OUT_PX, w = VOL_ARC_R_OUT_PX - VOL_ARC_R_IN_PX;
    double ex = 2 * x - C2 - mid * cos(th), ey = 2 * y - C2 - mid * sin(th);
    int outward = high ? t >= g->sweep + k * pitch : (t > 180.0 ? t - 360.0 : t) < -k * pitch;
    return outward && ex * ex + ey * ey <= w * w;
}

/* The end cap k cells past the meter end that holds the pixel, as 1 + k at the low end and 1 + ARC_OVER_CELLS + k at the high end; 0 for none. */
static int cap_of(int x, int y, const geom_t *g)
{
    for (int k = 0; k <= ARC_OVER_CELLS; k++) {
        for (int high = 0; high < 2; high++) {
            if (in_cap(x, y, g, high, k)) {
                return 1 + high * (1 + ARC_OVER_CELLS) + k;
            }
        }
    }
    return 0;
}

static int thr_over_at(int x, int y, const geom_t *g)
{
    int cap = cap_of(x, y, g);
    if (cap) {
        int high = cap > 1 + ARC_OVER_CELLS, k = cap - 1 - high * (1 + ARC_OVER_CELLS);
        return k == 0 ? 0 : (high ? ARC_OVER_CELLS : 0) + k;
    }
    double a = atan2((double)(2 * y - C2), (double)(2 * x - C2)) * 180.0 / PI_D, t = a - g->start;
    t -= 360.0 * floor(t / 360.0);
    double pitch = g->sweep / g->steps, run = ARC_OVER_CELLS * pitch;
    if (t >= g->sweep && t - g->sweep < run) {
        int j = (int)floor((t - g->sweep) / pitch) + 1;
        return ARC_OVER_CELLS + (j > ARC_OVER_CELLS ? ARC_OVER_CELLS : j);
    }
    if (t > 0 && 360.0 - t < run) {
        int k = (int)floor((360.0 - t) / pitch) + 1;
        return k > ARC_OVER_CELLS ? ARC_OVER_CELLS : k;
    }
    return 0;
}

static int thr_main_at(int x, int y, const geom_t *g)
{
    int t = thr_at(x, y, g->start, g->sweep, g->steps), cap = t ? 0 : cap_of(x, y, g);
    if (cap == 1) {
        return 1;
    }
    return cap == 2 + ARC_OVER_CELLS ? g->steps : t;
}

static int build_table(volarc_t *a, uint32_t *storage, int cap, int nthr, const geom_t *g, int (*thr_fn)(int, int, const geom_t *))
{
    int count[101] = {0};
    a->e = storage;
    a->n = 0;
    a->steps = nthr;
    for (int y = 0; y < LCD_SIZE; y++) {
        for (int x = 0; x < LCD_SIZE; x++) {
            if (in_band(x, y)) {
                int t = thr_fn(x, y, g);
                if (t > 0) {
                    count[t]++;
                }
            }
        }
    }
    a->cnt[0] = 0;
    int total = 0, pos[101];
    for (int t = nthr + 1; t <= 100; t++) {
        a->cnt[t] = 0;
    }
    for (int t = 1; t <= nthr; t++) {
        pos[t] = total;
        total += count[t];
        a->cnt[t] = total;
    }
    if (total > cap) {
        return -1;
    }
    for (int y = 0; y < LCD_SIZE; y++) {
        for (int x = 0; x < LCD_SIZE; x++) {
            if (in_band(x, y)) {
                int t = thr_fn(x, y, g);
                if (t > 0) {
                    a->e[pos[t]++] = ((uint32_t)t << 20) | ((uint32_t)y << 10) | (uint32_t)x;
                }
            }
        }
    }
    a->n = total;
    return total;
}

int volarc_build_seg(volarc_t *a, uint32_t *storage, int cap, double start_deg, double sweep_deg, int steps)
{
    geom_t g = {start_deg, sweep_deg, steps};
    return build_table(a, storage, cap, steps, &g, thr_main_at);
}

int volarc_build_over(volarc_t *a, uint32_t *storage, int cap, double start_deg, double sweep_deg, int steps)
{
    geom_t g = {start_deg, sweep_deg, steps};
    return build_table(a, storage, cap, 2 * ARC_OVER_CELLS, &g, thr_over_at);
}

void volarc_xy(uint32_t e, int quad, int *x, int *y)
{
    int px = (int)(e & 0x3FF), py = (int)((e >> 10) & 0x3FF);
    for (int i = 0; i < (quad & 3); i++) {
        int nx = (LCD_SIZE - 1) - py;
        py = px;
        px = nx;
    }
    *x = px;
    *y = py;
}

int volarc_thr(uint32_t e)
{
    return (int)(e >> 20);
}

int volarc_class(int thr, int v)
{
    if (v > 0) {
        return thr <= v ? VOLARC_FILL : VOLARC_TRACK;
    }
    return thr <= VOL_ARC_MUTE_THR ? VOLARC_MUTE : VOLARC_TRACK;
}

void volarc_change_range(int a, int b, int *lo, int *hi)
{
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
    if (*lo == 0 && *hi < VOL_ARC_MUTE_THR) {
        *hi = VOL_ARC_MUTE_THR;
    }
}

int volarc_seg_class(int thr, int bi)
{
    return volarc_class(thr, bi + 1);
}

void volarc_seg_change_range(int a, int b, int *lo, int *hi)
{
    *lo = (a < b ? a : b) + 1;
    *hi = (a < b ? b : a) + 1;
}

int volarc_areas(const volarc_t *a, int quad, int lo, int hi, volarc_rect_t *out, int max)
{
    return volarc_areas_chunk(a, quad, lo, hi, VOL_ARC_CHUNK_THR, out, max);
}

int volarc_areas_chunk(const volarc_t *a, int quad, int lo, int hi, int chunk, volarc_rect_t *out, int max)
{
    int n = 0;
    for (int from = lo; from < hi && n < max; from += chunk) {
        int to = from + chunk < hi ? from + chunk : hi;
        volarc_rect_t r = {LCD_SIZE, LCD_SIZE, -1, -1};
        for (int i = a->cnt[from]; i < a->cnt[to]; i++) {
            int x, y;
            volarc_xy(a->e[i], quad, &x, &y);
            r.x1 = x < r.x1 ? x : r.x1;
            r.y1 = y < r.y1 ? y : r.y1;
            r.x2 = x > r.x2 ? x : r.x2;
            r.y2 = y > r.y2 ? y : r.y2;
        }
        if (r.x2 >= r.x1) {
            out[n++] = r;
        }
    }
    return n;
}

int volarc_over_lit(int thr, int over)
{
    return over < 0 ? thr <= -over : over > 0 && thr > ARC_OVER_CELLS && thr - ARC_OVER_CELLS <= over;
}

int volarc_over_ranges(int from, int to, int lo[2], int hi[2])
{
    int n = 0, fl = from < 0 ? -from : 0, tl = to < 0 ? -to : 0, fh = from > 0 ? from : 0, th = to > 0 ? to : 0;
    if (fl != tl) {
        lo[n] = fl < tl ? fl : tl;
        hi[n++] = fl < tl ? tl : fl;
    }
    if (fh != th) {
        lo[n] = ARC_OVER_CELLS + (fh < th ? fh : th);
        hi[n++] = ARC_OVER_CELLS + (fh < th ? th : fh);
    }
    return n;
}

int volarc_over_class(int seg, int over)
{
    return !seg && over < 0 ? VOLARC_MUTE : VOLARC_FILL;
}

int volarc_fold_rect(volarc_rect_t *rects, int n, int end, volarc_rect_t extra)
{
    if (n <= 0) {
        return 0;
    }
    volarc_rect_t *r = &rects[end < 0 ? 0 : n - 1];
    r->x1 = extra.x1 < r->x1 ? extra.x1 : r->x1;
    r->y1 = extra.y1 < r->y1 ? extra.y1 : r->y1;
    r->x2 = extra.x2 > r->x2 ? extra.x2 : r->x2;
    r->y2 = extra.y2 > r->y2 ? extra.y2 : r->y2;
    return n;
}

int volarc_whole_areas(const volarc_t *m, const volarc_t *o, int quad, int chunk, int stretch, volarc_rect_t *out, int max)
{
    int n = volarc_areas_chunk(m, quad, 0, m->steps, chunk, out, max);
    int lo[2], hi[2], k = volarc_over_ranges(0, stretch, lo, hi);
    for (int i = 0; i < k; i++) {
        volarc_rect_t x;
        if (volarc_areas_chunk(o, quad, lo[i], hi[i], ARC_OVER_CELLS, &x, 1) == 1) {
            volarc_fold_rect(out, n, stretch, x);
        }
    }
    return n;
}

int volarc_over_areas(const volarc_t *o, int quad, int from, int to, volarc_rect_t out[2])
{
    int lo[2], hi[2], n = 0, k = volarc_over_ranges(from, to, lo, hi);
    for (int i = 0; i < k; i++) {
        n += volarc_areas_chunk(o, quad, lo[i], hi[i], ARC_OVER_CELLS, out + n, 1);
    }
    return n;
}
