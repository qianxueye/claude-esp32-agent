#include "volarc.h"
#include <math.h>

#define PI_D 3.14159265358979323846
#define C2 (LCD_SIZE - 1) /* twice the centre coordinate of the pixel grid */

_Static_assert(VOL_ARC_R_IN_PX > LISTEN_RING_DIAMETER / 2 + 1, "arc must lie outside the listening ring");
_Static_assert(VOL_ARC_R_OUT_PX < LCD_SIZE / 2, "arc must lie inside the glass");
_Static_assert(VOL_ARC_R_IN_PX < VOL_ARC_R_OUT_PX, "arc band must not be empty");

static int thr_at(int x, int y)
{
    double a = atan2((double)(2 * y - C2), (double)(2 * x - C2)) * 180.0 / PI_D;
    double t = a - VOL_ARC_START_DEG;
    t -= 360.0 * floor(t / 360.0);
    if (t >= VOL_SWEEP_DEG) {
        return 0;
    }
    return (int)floor(t * 100.0 / VOL_SWEEP_DEG) + 1;
}

static int in_band(int x, int y)
{
    int dx = 2 * x - C2, dy = 2 * y - C2, d2 = dx * dx + dy * dy;
    return d2 >= 4 * VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX && d2 <= 4 * VOL_ARC_R_OUT_PX * VOL_ARC_R_OUT_PX;
}

int volarc_build(volarc_t *a, uint32_t *storage, int cap)
{
    int count[101] = {0};
    a->e = storage;
    a->n = 0;
    for (int y = 0; y < LCD_SIZE; y++) {
        for (int x = 0; x < LCD_SIZE; x++) {
            if (in_band(x, y)) {
                int t = thr_at(x, y);
                if (t > 0) {
                    count[t]++;
                }
            }
        }
    }
    a->cnt[0] = 0;
    int total = 0, pos[101];
    for (int t = 1; t <= 100; t++) {
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
                int t = thr_at(x, y);
                if (t > 0) {
                    a->e[pos[t]++] = ((uint32_t)t << 20) | ((uint32_t)y << 10) | (uint32_t)x;
                }
            }
        }
    }
    a->n = total;
    return total;
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

int volarc_areas(const volarc_t *a, int quad, int lo, int hi, volarc_rect_t *out, int max)
{
    int n = 0;
    for (int from = lo; from < hi && n < max; from += VOL_ARC_CHUNK_THR) {
        int to = from + VOL_ARC_CHUNK_THR < hi ? from + VOL_ARC_CHUNK_THR : hi;
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
