#include "rotblit.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MASK565 0x07E0F81Fu

static void sincos_tenths(int tenths, float *s, float *c)
{
    int t = ((tenths % 3600) + 3600) % 3600;
    if (t % 900 == 0) {
        static const float C[4] = {1, 0, -1, 0}, S[4] = {0, 1, 0, -1};
        *c = C[t / 900];
        *s = S[t / 900];
        return;
    }
    float r = (float)t * 0.1f * (float)M_PI / 180.0f;
    *s = sinf(r);
    *c = cosf(r);
}

static bool is_exact(int tenths)
{
    return (((tenths % 3600) + 3600) % 3600) % 900 == 0;
}

/* Narrows [d0, d1] to the dx where lo <= base + a * dx <= hi; returns false when empty. */
static bool span(float base, float a, float lo, float hi, float *d0, float *d1)
{
    if (fabsf(a) < 1e-6f) {
        return base >= lo && base <= hi;
    }
    float t0 = (lo - base) / a, t1 = (hi - base) / a;
    if (t0 > t1) {
        float t = t0;
        t0 = t1;
        t1 = t;
    }
    *d0 = fmaxf(*d0, t0);
    *d1 = fminf(*d1, t1);
    return *d0 <= *d1;
}

/* Source-space limits keeping every sampled texel (and its right/lower neighbour when blending) inside the source. */
static void limits(const irect_t *r, int S, bool exact, float *ulo, float *uhi, float *vlo, float *vhi)
{
    const float cap = exact ? (float)S - 0.001f : (float)S - 1.001f;
    *ulo = r->x0 < 0 ? 0.0f : (float)r->x0;
    *vlo = r->y0 < 0 ? 0.0f : (float)r->y0;
    *uhi = fminf((float)r->x1 - 0.001f, cap);
    *vhi = fminf((float)r->y1 - 0.001f, cap);
}

void rotblit_spans(const irect_t *rect, int S, int OUT, int tenths, xspan_t *spans)
{
    float s, c;
    sincos_tenths(tenths, &s, &c);
    const bool exact = is_exact(tenths);
    const float hs = S / 2.0f, hd = OUT / 2.0f, off = exact ? 0.0f : 0.5f;
    float ulo, uhi, vlo, vhi;
    limits(rect, S, exact, &ulo, &uhi, &vlo, &vhi);
    for (int y = 0; y < OUT; y++) {
        const float dy = y + 0.5f - hd;
        float d0 = -1e9f, d1 = 1e9f;
        bool ok = span(dy * s + hs - off, c, ulo, uhi, &d0, &d1) && span(dy * c + hs - off, -s, vlo, vhi, &d0, &d1);
        int xa = (int)ceilf(d0 + hd - 0.5f), xb = (int)floorf(d1 + hd - 0.5f) + 1;
        xa = xa < 0 ? 0 : xa;
        xb = xb > OUT ? OUT : xb;
        spans[y].x0 = ok && xa < xb ? (int16_t)xa : 0;
        spans[y].x1 = ok && xa < xb ? (int16_t)xb : 0;
    }
}

static inline uint32_t spread(uint16_t p)
{
    return ((uint32_t)p | ((uint32_t)p << 16)) & MASK565;
}

static inline uint32_t mix(uint32_t a, uint32_t b, uint32_t a5)
{
    return (((b - a) * a5 >> 5) + a) & MASK565;
}

void rotblit(const uint8_t *src, int S, const uint16_t *pal, uint16_t *dst, int dstride, int OUT, int tenths,
             const xspan_t *draw, const xspan_t *clear, int y0, int y1)
{
    float s, c;
    sincos_tenths(tenths, &s, &c);
    const bool exact = is_exact(tenths);
    const float hs = S / 2.0f, hd = OUT / 2.0f, off = exact ? 0.0f : 0.5f;
    const int32_t dudx = (int32_t)lroundf(c * 65536.0f), dvdx = (int32_t)lroundf(-s * 65536.0f);
    const uint16_t black = pal[0];
    for (int y = y0; y < y1; y++) {
        uint16_t *row = dst + (size_t)y * dstride;
        int xa = draw[y].x0, xb = draw[y].x1;
        int c0 = clear[y].x0, c1 = clear[y].x1;
        if (xa >= xb) {
            xa = xb = c0;
        }
        for (int x = c0; x < xa; x++) {
            row[x] = black;
        }
        for (int x = xb; x < c1; x++) {
            row[x] = black;
        }
        if (xa >= xb) {
            continue;
        }
        const float dy = y + 0.5f - hd, dx0 = xa + 0.5f - hd;
        int32_t U = (int32_t)lroundf((dx0 * c + dy * s + hs - off) * 65536.0f);
        int32_t V = (int32_t)lroundf((-dx0 * s + dy * c + hs - off) * 65536.0f);
        uint16_t *out = row + xa;
        if (exact) {
            for (int x = xa; x < xb; x++, U += dudx, V += dvdx) {
                *out++ = pal[src[(size_t)(V >> 16) * S + (U >> 16)]];
            }
            continue;
        }
        for (int x = xa; x < xb; x++, U += dudx, V += dvdx) {
            const uint8_t *p = src + (size_t)(V >> 16) * S + (U >> 16);
            uint8_t ia = p[0], ib = p[1], ic = p[S], id = p[S + 1];
            if (ia == ib && ia == ic && ia == id) {
                *out++ = pal[ia];
                continue;
            }
            uint32_t fx = (U >> 11) & 31, fy = (V >> 11) & 31;
            uint32_t top = mix(spread(pal[ia]), spread(pal[ib]), fx), bot = mix(spread(pal[ic]), spread(pal[id]), fx);
            uint32_t m = mix(top, bot, fy);
            *out++ = (uint16_t)(m | (m >> 16));
        }
    }
}

void rotblit_cos_sin(int tenths, float *c, float *s)
{
    sincos_tenths(tenths, s, c);
}

void rotblit_spans_nearest(const irect_t *rect, int S, int OUT, int tenths, xspan_t *spans)
{
    float s, c;
    sincos_tenths(tenths, &s, &c);
    const float hs = S / 2.0f, hd = OUT / 2.0f;
    const float ulo = (float)rect->x0 + ROTBLIT_NEAREST_INSET, uhi = (float)rect->x1 - ROTBLIT_NEAREST_INSET;
    const float vlo = (float)rect->y0 + ROTBLIT_NEAREST_INSET, vhi = (float)rect->y1 - ROTBLIT_NEAREST_INSET;
    for (int y = 0; y < OUT; y++) {
        const float dy = y + 0.5f - hd;
        float d0 = -1e9f, d1 = 1e9f;
        bool ok = span(dy * s + hs, c, ulo, uhi, &d0, &d1) && span(dy * c + hs, -s, vlo, vhi, &d0, &d1);
        int xa = ok ? (int)ceilf(d0 + hd - 0.5f) : 0, xb = ok ? (int)floorf(d1 + hd - 0.5f) + 1 : 0;
        xa = xa < 0 ? 0 : xa;
        xb = xb > OUT ? OUT : xb;
        spans[y].x0 = xa < xb ? (int16_t)xa : 0;
        spans[y].x1 = xa < xb ? (int16_t)xb : 0;
    }
}
