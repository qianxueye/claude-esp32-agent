#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "rotblit.h"

#define S 480
#define OUT 466

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

static uint8_t g_src[S * S];
static uint16_t g_pal[256];
static uint16_t g_dst[OUT * OUT];
static xspan_t g_draw[OUT], g_clear[OUT];

static void fixture(void)
{
    uint32_t r = 12345u;
    for (int i = 0; i < S * S; i++) {
        r = r * 1664525u + 1013904223u;
        g_src[i] = (uint8_t)(1 + ((r >> 24) % 12));
    }
    for (int i = 0; i < 256; i++) {
        r = r * 1664525u + 1013904223u;
        g_pal[i] = (uint16_t)(r >> 16);
    }
    g_pal[0] = 0x1111;
}

typedef struct {
    int tenths;
    uint32_t spans, pixels;
} pin_t;

/* Right angles only: that path uses no trig and integer steps, so the pins hold on any compiler and libm. */
static const pin_t PINS[] = {
    {0, 0x4553142Du, 0xBFA2AAEFu},
    {900, 0x609FD81Du, 0xF93B25F7u},
    {1800, 0x0D952DEDu, 0xBE8A9C67u},
    {2700, 0xEA03DF9Du, 0xB6663387u},
};

static void r1(void)
{
    fixture();
    const irect_t inner = {40, 60, 440, 420}, outer = {20, 30, 470, 450};
    const int n = (int)(sizeof PINS / sizeof PINS[0]);
    for (int i = 0; i < n; i++) {
        const int t = PINS[i].tenths;
        rotblit_spans(&inner, S, OUT, t, g_draw);
        rotblit_spans(&outer, S, OUT, t, g_clear);
        uint32_t hs = fnv(2166136261u, g_draw, sizeof g_draw);
        hs = fnv(hs, g_clear, sizeof g_clear);
        memset(g_dst, 0, sizeof g_dst);
        rotblit(g_src, S, g_pal, g_dst, OUT, OUT, t, g_draw, g_clear, 0, OUT);
        const uint32_t hp = fnv(2166136261u, g_dst, sizeof g_dst);
        assert(hs == PINS[i].spans && hp == PINS[i].pixels);
    }
    printf("R1: %d right angles pinned\n", n);
}

#define TOL 1e-3 /* test tolerance: above the double-versus-float span error, far below the inset */

static void exact_cs(int tenths, double *s, double *c)
{
    static const double CO[4] = {1, 0, -1, 0}, SI[4] = {0, 1, 0, -1};
    if (tenths % 900 == 0) {
        *c = CO[tenths / 900];
        *s = SI[tenths / 900];
        return;
    }
    const double r = tenths * 0.1 * 3.14159265358979323846 / 180.0;
    *s = sin(r);
    *c = cos(r);
}

static void exact_uv(int tenths, int x, int y, double *u, double *v)
{
    double s, c;
    exact_cs(tenths, &s, &c);
    const double dx = x + 0.5 - OUT / 2.0, dy = y + 0.5 - OUT / 2.0;
    *u = dx * c + dy * s + S / 2.0;
    *v = -dx * s + dy * c + S / 2.0;
}

/* One row end read through the same 16.16 stepping the composer uses; true when it stays inside the rectangle. */
static bool stepped_inside(const irect_t *r, int tenths, int y, int xa, int n, double *worst)
{
    float c, s;
    rotblit_cos_sin(tenths, &c, &s);
    const int32_t dudx = (int32_t)lroundf(c * 65536.0f), dvdx = (int32_t)lroundf(-s * 65536.0f);
    const float hs = S / 2.0f, hd = OUT / 2.0f, dy = y + 0.5f - hd, dx0 = xa + 0.5f - hd;
    const int64_t U = (int32_t)lroundf((dx0 * c + dy * s + hs) * 65536.0f) + (int64_t)(n - 1) * dudx;
    const int64_t V = (int32_t)lroundf((-dx0 * s + dy * c + hs) * 65536.0f) + (int64_t)(n - 1) * dvdx;
    double u, v;
    exact_uv(tenths, xa + n - 1, y, &u, &v);
    const double e = fmax(fabs((double)U / 65536.0 - u), fabs((double)V / 65536.0 - v));
    *worst = fmax(*worst, e);
    return (U >> 16) >= r->x0 && (U >> 16) < r->x1 && (V >> 16) >= r->y0 && (V >> 16) < r->y1;
}

static void r2(void)
{
    /* Per step: the rounded 16.16 increment and one float ulp of the trig value; at the start: four float operations on
       values below 2048, whose half ulp is 2^-13 / 2, and the trig error over half the output width. */
    const double step_err = OUT * (0.5 / 65536.0 + 1.0 / 8388608.0);
    const double start_err = 0.5 / 65536.0 + 4.0 * 0.5 / 8192.0 + (OUT / 2.0) * 2.0 / 8388608.0;
    const double bound = step_err + start_err;
    assert((double)ROTBLIT_NEAREST_INSET > bound);
    const irect_t rects[] = {{40, 60, 440, 420}, {-30, -10, 150, 200}, {235, 235, 251, 247}, {300, 100, 700, 600}};
    static xspan_t sp[OUT];
    double worst = 0;
    long rows = 0, pix_in = 0;
    for (size_t k = 0; k < sizeof rects / sizeof rects[0]; k++) {
        const irect_t *r = &rects[k];
        for (int t = 0; t < 3600; t++) {
            rotblit_spans_nearest(r, S, OUT, t, sp);
            for (int y = 0; y < OUT; y++) {
                const int n = sp[y].x1 - sp[y].x0;
                if (n > 0) {
                    assert(stepped_inside(r, t, y, sp[y].x0, 1, &worst) && stepped_inside(r, t, y, sp[y].x0, n, &worst));
                    rows++;
                    pix_in += n;
                }
                const bool full = t % 50 == 0;
                for (int x = 0; x < OUT; x++) {
                    if (!full && (x < sp[y].x0 - 1 || x > sp[y].x1)) {
                        continue;
                    }
                    double u, v;
                    exact_uv(t, x, y, &u, &v);
                    const double in = ROTBLIT_NEAREST_INSET;
                    const bool inside = u >= r->x0 + in - TOL && u <= r->x1 - in + TOL && v >= r->y0 + in - TOL && v <= r->y1 - in + TOL;
                    const bool deep = u > r->x0 + in + TOL && u < r->x1 - in - TOL && v > r->y0 + in + TOL && v < r->y1 - in - TOL;
                    const bool listed = x >= sp[y].x0 && x < sp[y].x1;
                    assert(!listed || inside);
                    assert(listed || !deep);
                }
            }
        }
    }
    printf("R2: inset %.5f px > bound %.5f px (steps %.5f, start %.5f); worst stepped deviation %.5f px; %ld rows, %ld pixels\n",
           (double)ROTBLIT_NEAREST_INSET, bound, step_err, start_err, worst, rows, pix_in);
}

int main(void)
{
    r1();
    r2();
    return 0;
}
