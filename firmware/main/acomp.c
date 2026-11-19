#include "acomp.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static bool art_ok(const acomp_art_t *a)
{
    return a->buf && a->w > 0 && a->h > 0 && a->stride >= a->w && (a->scale == 1 || a->scale == 2);
}

static bool empty(xspan_t s)
{
    return s.x0 >= s.x1;
}

void acomp_spans(const acomp_art_t *a, xspan_t *spans)
{
    if (!art_ok(a)) {
        for (int y = 0; y < a->out; y++) {
            spans[y] = (xspan_t){0, 0};
        }
        return;
    }
    const irect_t r = {a->ox, a->oy, a->ox + a->w * a->scale, a->oy + a->h * a->scale};
    rotblit_spans_nearest(&r, a->scene, a->out, a->tenths, spans);
}

void acomp_rows(const acomp_art_t *a, const xspan_t *spans, const uint16_t *pal, uint16_t *dst, int stride_px, int x0, int y0,
                int x1, int y1)
{
    if (!art_ok(a)) {
        return;
    }
    float c, s;
    rotblit_cos_sin(a->tenths, &c, &s);
    const int32_t dudx = (int32_t)lroundf(c * 65536.0f), dvdx = (int32_t)lroundf(-s * 65536.0f);
    const int sh = a->scale == 2 ? 17 : 16;
    const float hs = a->scene / 2.0f, hd = a->out / 2.0f;
    for (int y = y0; y < y1; y++) {
        const xspan_t sp = spans[y];
        const int xa = sp.x0 > x0 ? sp.x0 : x0, xb = sp.x1 < x1 ? sp.x1 : x1;
        if (empty(sp) || xa >= xb) {
            continue;
        }
        /* Start from the span's own first pixel so every chunk steps the same fixed-point sequence. */
        const float dy = y + 0.5f - hd, dx0 = sp.x0 + 0.5f - hd;
        int32_t U = (int32_t)lroundf((dx0 * c + dy * s + hs - (float)a->ox) * 65536.0f) + (xa - sp.x0) * dudx;
        int32_t V = (int32_t)lroundf((-dx0 * s + dy * c + hs - (float)a->oy) * 65536.0f) + (xa - sp.x0) * dvdx;
        uint16_t *d = dst + (size_t)(y - y0) * stride_px + (xa - x0);
        for (int x = xa; x < xb; x++, d++, U += dudx, V += dvdx) {
            if (*d) {
                continue;
            }
            const uint8_t idx = a->buf[(size_t)(V >> sh) * a->stride + (size_t)(U >> sh)];
            if (idx) {
                *d = pal[idx];
            }
        }
    }
}

void acomp_dirty(const xspan_t *prev, const xspan_t *next, xspan_t *out, int n)
{
    for (int y = 0; y < n; y++) {
        const xspan_t p = prev[y], q = next[y];
        out[y] = empty(q) ? p : empty(p) ? q : (xspan_t){q.x0 < p.x0 ? q.x0 : p.x0, q.x1 > p.x1 ? q.x1 : p.x1};
    }
}

int acomp_bands(const xspan_t *spans, int n, int band_rows, irect_t *out_rects, int max)
{
    int ymin = n, ymax = 0, count = 0;
    for (int y = 0; y < n; y++) {
        if (!empty(spans[y])) {
            ymin = y < ymin ? y : ymin;
            ymax = y + 1;
        }
    }
    for (int y0 = ymin; band_rows > 0 && y0 < ymax; y0 += band_rows) {
        const int y1 = y0 + band_rows < ymax ? y0 + band_rows : ymax;
        int xa = 0x7FFF, xb = 0;
        for (int y = y0; y < y1; y++) {
            if (!empty(spans[y])) {
                xa = spans[y].x0 < xa ? spans[y].x0 : xa;
                xb = spans[y].x1 > xb ? spans[y].x1 : xb;
            }
        }
        if (xa < xb) {
            if (count < max) {
                out_rects[count] = (irect_t){xa, y0, xb, y1};
            }
            count++;
        }
    }
    return count;
}

int acomp_step(acomp_state_t *st, bool active, const acomp_art_t *art, const xspan_t *scene)
{
    /* st->all holds the new spans until they replace st->art. */
    if (active && art->out == ACOMP_ROWS) {
        acomp_spans(art, st->all);
    } else {
        memset(st->all, 0, sizeof st->all);
    }
    acomp_dirty(st->art, st->all, st->dirty, ACOMP_ROWS);
    memcpy(st->art, st->all, sizeof st->art);
    if (scene) {
        acomp_dirty(st->dirty, scene, st->all, ACOMP_ROWS);
    } else {
        memcpy(st->all, st->dirty, sizeof st->all);
    }
    st->n_bands = acomp_bands(st->all, ACOMP_ROWS, DIRTY_BAND_ROWS, st->bands, ACOMP_MAX_BANDS);
    return st->n_bands;
}

bool acomp_split(const xspan_t *spans, int y0, int y1, int *ya, int *mid, int *yb)
{
    int a = y1, b = y0;
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < ACOMP_ROWS; y++) {
        if (!empty(spans[y])) {
            a = y < a ? y : a;
            b = y + 1;
        }
    }
    *ya = a;
    *yb = b;
    *mid = (a + b) / 2;
    return a < b;
}
