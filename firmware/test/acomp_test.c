#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "acomp.h"
#include "aglue.h"
#include "apack_golden.h"
#include "app_config.h"
#include "rotblit.h"
#include "scenepal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SCENE SCENE_SIZE_PX
#define OUT SCENE_OUT_PX
#define SENTINEL 255
#define SENTINEL_COLOR 0xABCD
#define TOL_PX (1.0 / 64.0) /* mathematical model: the span inset, used as the boundary allowance */
#define MISS_PX (1.0 / 32.0) /* plain configuration: a pixel this deep inside the crop must be listed */
#define GUARD_PX 4 /* plain configuration: sentinel ring width, wider than the one texel an overrun can reach */

static uint16_t g_pal[256];
static uint16_t g_dst[OUT * OUT], g_ref[OUT * OUT], g_whole[OUT * OUT];
static uint8_t g_model[SCENE * SCENE];
static xspan_t g_sp[OUT], g_clear[OUT];

static uint32_t rnd(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void init_pal(void)
{
    for (int i = 1; i < 255; i++) {
        g_pal[i] = (uint16_t)((i + 1) * 257);
    }
    g_pal[0] = 0;
    g_pal[SENTINEL] = SENTINEL_COLOR;
    for (int i = 0; i < 255; i++) {
        assert(g_pal[i] != SENTINEL_COLOR);
    }
}

static void compose(const acomp_art_t *a, uint16_t *dst)
{
    acomp_spans(a, g_sp);
    acomp_rows(a, g_sp, g_pal, dst, OUT, 0, 0, OUT, OUT);
}

/* X1: the key rule on a small crop at a right angle. */
static void x1(void)
{
    static const uint8_t art[12] = {1, 0, 2, 0, 3, 4, 5, 6, 0, 7, 8, 9};
    const acomp_art_t a = {art, 6, 2, 6, 1, 240, 240, 0, SCENE, OUT};
    g_pal[0] = 0x7777; /* index 0 must never reach the palette */
    enum { CX = 233, CY = 233, SW = 20, X0 = 230, Y0 = 232, H = 4 };
    uint16_t dst[SW * H];
    memset(dst, 0, sizeof dst);
    dst[(CY - Y0) * SW + (CX - X0)] = 0x1234;
    dst[(CY + 1 - Y0) * SW + (CX + 3 - X0)] = 0x1234;
    acomp_spans(&a, g_sp);
    assert(g_sp[CY].x0 == CX && g_sp[CY].x1 == CX + 6 && g_sp[CY + 1].x0 == CX && g_sp[CY - 1].x0 == g_sp[CY - 1].x1);
    acomp_rows(&a, g_sp, g_pal, dst, SW, X0, Y0, X0 + SW, Y0 + H);
    int written = 0, zeros = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < SW; x++) {
            const int tx = X0 + x - CX, ty = Y0 + y - CY;
            const bool in = tx >= 0 && tx < 6 && ty >= 0 && ty < 2;
            uint16_t want = 0;
            if (in) {
                want = art[ty * 6 + tx] ? g_pal[art[ty * 6 + tx]] : 0;
            }
            if ((y == CY - Y0 && x == CX - X0) || (y == CY + 1 - Y0 && x == CX + 3 - X0)) {
                want = 0x1234;
            } else if (in) {
                written += want != 0;
                zeros += want == 0;
            }
            assert(dst[y * SW + x] == want);
        }
    }
    assert(dst[(CY - Y0) * SW + (CX + 1 - X0)] == 0);
    assert(dst[(CY - Y0) * SW + (CX + 2 - X0)] == g_pal[2]);
    g_pal[0] = 0;
    assert(written == 7 && zeros == 3);
    printf("X1: 0x1234 kept at 2 pixels, %d index pixels written, %d index-0 pixels stay 0\n", written, zeros);
}

static ap_pack_t golden(void)
{
    ap_pack_t p;
    assert(ap_parse(APACK_GOLDEN_PACK, sizeof APACK_GOLDEN_PACK, sizeof APACK_GOLDEN_PACK, &p) == AP_OK);
    return p;
}

/* Clipped copy of a decoded crop into dst, each index widened to scale x scale pixels; the oracle for the old render path. */
static void oracle_copy(const uint8_t *buf, const ap_clip_t *c, uint8_t scale, const ag_place_t *pl, uint8_t *dst, int dst_stride)
{
    if (pl->x0 >= pl->x1 || pl->y0 >= pl->y1 || scale < 1) {
        return;
    }
    for (int y = pl->y0; y < pl->y1; y++) {
        const uint8_t *row = buf + (size_t)((y - pl->oy) / scale) * c->w;
        uint8_t *out = dst + (size_t)y * dst_stride;
        for (int x = pl->x0; x < pl->x1; x++) {
            out[x] = row[(x - pl->ox) / scale];
        }
    }
}

/* Reference path as the old render did it: clipped copy into a zeroed scene model, then the exact rotblit. */
static void reference(const uint8_t *buf, const ap_clip_t *c, int scale, const ag_place_t *place, int tenths)
{
    ag_place_t pl = *place;
    pl.x0 = pl.ox < 0 ? 0 : pl.ox;
    pl.y0 = pl.oy < 0 ? 0 : pl.oy;
    pl.x1 = pl.ox + c->w * scale > SCENE ? SCENE : pl.ox + c->w * scale;
    pl.y1 = pl.oy + c->h * scale > SCENE ? SCENE : pl.oy + c->h * scale;
    memset(g_model, 0, sizeof g_model);
    oracle_copy(buf, c, (uint8_t)scale, &pl, g_model, SCENE);
    const irect_t all = {0, 0, SCENE, SCENE};
    rotblit_spans(&all, SCENE, OUT, tenths, g_clear);
    memset(g_ref, 0, sizeof g_ref);
    rotblit(g_model, SCENE, g_pal, g_ref, OUT, OUT, tenths, g_clear, g_clear, 0, OUT);
}

/* X2: composing on a zero frame equals the old path at right angles. */
static void x2(void)
{
    ap_pack_t p = golden();
    const ap_clip_t *c = &p.clip[1];
    uint8_t buf[192];
    ap_frame_t f;
    assert(c->w * c->h == 192 && ap_frame(&p, 1, 0, &f) && ap_decode(&f, buf, 192) && !memcmp(buf, APACK_GOLDEN_CLIP1, 192));
    const int ex = ORBIT_X0_PX + (ORBIT_COLS - 1) * ORBIT_PITCH_PX, ey = ORBIT_Y0_PX + (ORBIT_ROWS - 1) * ORBIT_PITCH_PX;
    const int pts[3][2] = {{ORBIT_X0_PX, ORBIT_Y0_PX}, {ex, ey}, {ORBIT_X0_PX + 8, ORBIT_Y0_PX + 12}};
    long cases = 0, lit = 0;
    for (int scale = 1; scale <= 2; scale++) {
        p.scale = (uint8_t)scale;
        for (int k = 0; k < 3; k++) {
            const ag_place_t pl = ag_sticker_place(&p, c, pts[k][0], pts[k][1], SCENE);
            for (int t = 0; t < 3600; t += 900) {
                reference(buf, c, scale, &pl, t);
                memset(g_dst, 0, sizeof g_dst);
                const acomp_art_t a = {buf, c->w, c->h, c->w, scale, pl.ox, pl.oy, t, SCENE, OUT};
                compose(&a, g_dst);
                assert(!memcmp(g_dst, g_ref, sizeof g_dst));
                for (int i = 0; i < OUT * OUT; i++) {
                    lit += g_dst[i] != 0;
                }
                cases++;
            }
        }
    }
    assert(lit > 0);
    printf("X2: %ld cases equal the old path (%ld lit pixels in all)\n", cases, lit);
}

typedef struct {
    uint8_t *mem, *buf;
    int w, h, stride;
} art_buf_t;

/* A crop of random indices 0..9 inside a ring of sentinel index. */
static art_buf_t make_art(int w, int h, int guard, uint32_t seed)
{
    art_buf_t b = {0, 0, w, h, w + 2 * guard};
    b.mem = malloc((size_t)b.stride * (size_t)(h + 2 * guard));
    assert(b.mem);
    memset(b.mem, SENTINEL, (size_t)b.stride * (size_t)(h + 2 * guard));
    b.buf = b.mem + (size_t)guard * b.stride + guard;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            b.buf[(size_t)y * b.stride + x] = (uint8_t)(rnd(&seed) % 10);
        }
    }
    return b;
}

static void fill_overlay(uint16_t *d, uint32_t seed)
{
    for (int i = 0; i < OUT * OUT; i++) {
        d[i] = rnd(&seed) % 4 == 0 ? (uint16_t)(0x1000 + (rnd(&seed) & 0xFF)) : 0;
    }
}

/* X3: any chunk split equals the whole frame, and nothing outside the chunk is written. */
static void x3(void)
{
    art_buf_t b = make_art(100, 90, 2, 7);
    static uint16_t over[OUT * OUT];
    fill_overlay(over, 99);
    long chunks = 0;
    for (int t = 0; t < 3600; t += 1234) {
        const acomp_art_t a = {b.buf, b.w, b.h, b.stride, 2, 140, 150, t, SCENE, OUT};
        memcpy(g_whole, over, sizeof over);
        compose(&a, g_whole);
        assert(memcmp(g_whole, over, sizeof over));
        const int cws[] = {1, 7, 64, 100, 233, 466}, gap = 5;
        for (int ch = 1; ch <= 32; ch++) {
            for (size_t k = 0; k < sizeof cws / sizeof cws[0]; k++) {
                if (ch > 3 && ch < 32 && k % 2) {
                    continue;
                }
                const int cw = cws[k], stride = cw + gap;
                memcpy(g_dst, over, sizeof over);
                for (int y0 = 0; y0 < OUT; y0 += ch) {
                    const int y1 = y0 + ch < OUT ? y0 + ch : OUT;
                    for (int x0 = 0; x0 < OUT; x0 += cw) {
                        const int x1 = x0 + cw < OUT ? x0 + cw : OUT;
                        static uint16_t mem[64 + 40 * (466 + 5) + 64];
                        const size_t n = (size_t)(y1 - y0) * stride;
                        memset(mem, 0, sizeof mem);
                        uint16_t *c = mem + 64;
                        for (int y = y0; y < y1; y++) {
                            memcpy(c + (size_t)(y - y0) * stride, g_dst + (size_t)y * OUT + x0, (size_t)(x1 - x0) * 2);
                        }
                        acomp_rows(&a, g_sp, g_pal, c, stride, x0, y0, x1, y1);
                        for (size_t i = 0; i < 64; i++) {
                            assert(mem[i] == 0 && mem[64 + n + i] == 0);
                        }
                        for (int y = y0; y < y1; y++) {
                            for (int x = 0; x < gap && x1 - x0 + x < stride; x++) {
                                assert(c[(size_t)(y - y0) * stride + (x1 - x0) + x] == 0);
                            }
                            memcpy(g_dst + (size_t)y * OUT + x0, c + (size_t)(y - y0) * stride, (size_t)(x1 - x0) * 2);
                        }
                        chunks++;
                    }
                }
                assert(!memcmp(g_dst, g_whole, sizeof g_dst));
            }
        }
    }
    free(b.mem);
    printf("X3: %ld chunks over heights 1..32 and widths 1..466 equal the whole frame, guards intact\n", chunks);
}

static void exact_cs(int t, double *s, double *c)
{
    static const double CO[4] = {1, 0, -1, 0}, SI[4] = {0, 1, 0, -1};
    if (t % 900 == 0) {
        *c = CO[t / 900];
        *s = SI[t / 900];
        return;
    }
    *c = cos(t * 0.1 * M_PI / 180.0);
    *s = sin(t * 0.1 * M_PI / 180.0);
}

static long g_used, g_near, g_px, g_lit;

static void x4_config(int w, int h, int scale, int ox, int oy, uint32_t seed)
{
    art_buf_t b = make_art(w, h, GUARD_PX, seed);
    const double lim = MISS_PX;
    for (int t = 0; t < 3600; t++) {
        const acomp_art_t a = {b.buf, w, h, b.stride, scale, ox, oy, t, SCENE, OUT};
        memset(g_dst, 0, sizeof g_dst);
        compose(&a, g_dst);
        double s, c;
        exact_cs(t, &s, &c);
        for (int y = 0; y < OUT; y++) {
            const double dy = y + 0.5 - OUT / 2.0;
            for (int x = 0; x < OUT; x++) {
                const double dx = x + 0.5 - OUT / 2.0;
                const double u = dx * c + dy * s + SCENE / 2.0 - ox, v = -dx * s + dy * c + SCENE / 2.0 - oy;
                const double fu = u / scale, fv = v / scale;
                const int tx = (int)floor(fu), ty = (int)floor(fv);
                const uint16_t got = g_dst[y * OUT + x];
                assert(got != SENTINEL_COLOR);
                g_px++;
                if (x < g_sp[y].x0 || x >= g_sp[y].x1) {
                    assert(got == 0);
                    assert(!(u > lim && u < w * scale - lim && v > lim && v < h * scale - lim));
                    continue;
                }
                assert(tx >= 0 && tx < w && ty >= 0 && ty < h);
                const double lu = (fu - tx) * scale, hu = (tx + 1 - fu) * scale, lv = (fv - ty) * scale, hv = (ty + 1 - fv) * scale;
                const int dxl = lu <= TOL_PX && tx > 0, dxh = hu <= TOL_PX && tx + 1 < w;
                const int dyl = lv <= TOL_PX && ty > 0, dyh = hv <= TOL_PX && ty + 1 < h;
                const uint8_t exact = b.buf[(size_t)ty * b.stride + tx];
                const uint16_t want = exact ? g_pal[exact] : 0;
                g_lit += got != 0;
                if (got == want) {
                    g_near += dxl || dxh || dyl || dyh;
                    continue;
                }
                bool ok = false;
                for (int ey = -dyl; ey <= dyh && !ok; ey++) {
                    for (int ex = -dxl; ex <= dxh && !ok; ex++) {
                        const uint8_t i = b.buf[(size_t)(ty + ey) * b.stride + tx + ex];
                        ok = got == (i ? g_pal[i] : 0);
                    }
                }
                assert(ok);
                g_used++;
                g_near++;
            }
        }
    }
    free(b.mem);
}

/* X4: every angle, the art never reads outside its buffer and each pixel is the texel under the exact point. */
static void x4(void)
{
    x4_config(250, 230, 2, -10, 20, 1);
    x4_config(300, 280, 1, 200, 150, 2);
    printf("X4: 7,200 angle frames, %ld pixels checked, %ld lit; %ld pixels within 1/64 px of a texel edge, %ld needed the neighbour texel\n",
           g_px, g_lit, g_near, g_used);
}

/* X5: an arc drawn into a zero frame and then cleared to 0 composes as if never drawn. */
static void x5(void)
{
    art_buf_t b = make_art(120, 110, 0, 5);
    const acomp_art_t a = {b.buf, 120, 110, 120, 2, 120, 130, 450, SCENE, OUT};
    static uint16_t never[OUT * OUT];
    memset(never, 0, sizeof never);
    compose(&a, never);
    memset(g_dst, 0, sizeof g_dst);
    long arc_px = 0, over_art = 0;
    const int c = OUT / 2;
    for (int y = 0; y < OUT; y++) {
        for (int x = 0; x < OUT; x++) {
            const double dx = x + 0.5 - c, dy = y + 0.5 - c, r = sqrt(dx * dx + dy * dy);
            if (r > 90 && r < 100 && dx > 0) {
                g_dst[y * OUT + x] = 0xF77E;
                arc_px++;
                over_art += never[y * OUT + x] != 0;
            }
        }
    }
    memcpy(g_whole, g_dst, sizeof g_dst);
    compose(&a, g_dst);
    for (int i = 0; i < OUT * OUT; i++) {
        assert(g_whole[i] == 0 || g_dst[i] == 0xF77E);
    }
    assert(over_art > 0);
    for (int i = 0; i < OUT * OUT; i++) {
        if (g_whole[i] == 0xF77E) {
            g_whole[i] = 0;
        }
    }
    compose(&a, g_whole);
    assert(!memcmp(g_whole, never, sizeof never));
    free(b.mem);
    printf("X5: %ld arc pixels, %ld of them over art, cleared frame equals the never-drawn frame\n", arc_px, over_art);
}

typedef struct {
    bool on;
    acomp_art_t a;
    xspan_t sp[OUT];
} state_t;

static art_buf_t g_arts[2];
static uint16_t g_sout[OUT * OUT], g_panel[OUT * OUT], g_want[OUT * OUT];

static state_t mk(bool on, int art, int scale, int ox, int oy, int t)
{
    state_t s;
    memset(&s, 0, sizeof s);
    s.on = on;
    if (on) {
        const art_buf_t *b = &g_arts[art];
        s.a = (acomp_art_t){b->buf, b->w, b->h, b->stride, scale, ox, oy, t, SCENE, OUT};
        acomp_spans(&s.a, s.sp);
    }
    return s;
}

static void flush(const state_t *st, const irect_t *r, int n)
{
    static uint16_t chunk[OUT * OUT];
    for (int i = 0; i < n; i++) {
        const int w = r[i].x1 - r[i].x0;
        for (int y = r[i].y0; y < r[i].y1; y++) {
            memcpy(chunk + (size_t)(y - r[i].y0) * w, g_sout + (size_t)y * OUT + r[i].x0, (size_t)w * 2);
        }
        if (st->on) {
            acomp_rows(&st->a, st->sp, g_pal, chunk, w, r[i].x0, r[i].y0, r[i].x1, r[i].y1);
        }
        for (int y = r[i].y0; y < r[i].y1; y++) {
            memcpy(g_panel + (size_t)y * OUT + r[i].x0, chunk + (size_t)(y - r[i].y0) * w, (size_t)w * 2);
        }
    }
}

static void full(const state_t *st)
{
    const irect_t all = {0, 0, OUT, OUT};
    flush(st, &all, 1);
}

static void step(const state_t *prev, const state_t *next, bool use_union)
{
    xspan_t d[OUT];
    if (use_union) {
        acomp_dirty(prev->sp, next->sp, d, OUT);
    } else {
        memcpy(d, next->sp, sizeof d);
    }
    irect_t r[16];
    const int n = acomp_bands(d, OUT, DIRTY_BAND_ROWS, r, 16);
    assert(n <= 16);
    flush(next, r, n);
}

static void step_rows(const state_t *prev, const state_t *next)
{
    xspan_t d[OUT];
    acomp_dirty(prev->sp, next->sp, d, OUT);
    for (int y = 0; y < OUT; y++) {
        if (d[y].x0 < d[y].x1) {
            const irect_t r = {d[y].x0, y, d[y].x1, y + 1};
            flush(next, &r, 1);
        }
    }
}

static int g_stale_controls, g_tick_cases;

static void transition(const state_t *a, const state_t *b)
{
    full(a);
    uint16_t before[OUT * OUT];
    memcpy(before, g_panel, sizeof before);
    step(a, b, true);
    memcpy(g_want, g_panel, sizeof g_want);
    full(b);
    assert(!memcmp(g_want, g_panel, sizeof g_want));
    memcpy(g_panel, before, sizeof before);
    step(a, b, false);
    assert(memcmp(g_panel, g_want, sizeof g_panel));
    g_stale_controls++;
    memcpy(g_panel, before, sizeof before);
    step_rows(a, b);
    assert(!memcmp(g_want, g_panel, sizeof g_want));
}

/* X6: flushing only the dirty rows over a scene-only frame gives the panel a full recomposition gives. */
static void x6(void)
{
    g_arts[0] = make_art(120, 100, 2, 11);
    g_arts[1] = make_art(90, 130, 2, 12);
    fill_overlay(g_sout, 5);
    const state_t a0 = mk(true, 0, 2, 160, 170, 0), off = mk(false, 0, 2, 0, 0, 0), crop = mk(true, 1, 2, 150, 140, 0);
    const state_t orbit = mk(true, 0, 2, 164, 166, 0), a900 = mk(true, 0, 2, 160, 170, 900), a450 = mk(true, 0, 2, 160, 170, 450);
    transition(&a0, &off);
    transition(&a0, &crop);
    transition(&a0, &orbit);
    transition(&a0, &a900);
    transition(&a900, &a450);
    srand(3);
    for (int i = 0; i < OUT * OUT; i++) {
        g_panel[i] = (uint16_t)(rand() | 1);
    }
    full(&a450);
    memcpy(g_want, g_panel, sizeof g_want);
    memset(g_panel, 0, sizeof g_panel);
    full(&a450);
    assert(!memcmp(g_want, g_panel, sizeof g_panel));
    free(g_arts[0].mem);
    free(g_arts[1].mem);
    printf("X6: 5 transitions and panel off -> on equal the full recomposition; %d stale controls differ without the union\n",
           g_stale_controls);
}

static acomp_state_t g_st, g_ctl;

/* Runs one tick from a to b and flushes its strips over a panel showing a; the panel must equal a full recomposition of b. */
static void tick_case(const state_t *a, bool active, const acomp_art_t *art_arg, const state_t *b, bool with_scene)
{
    static xspan_t strip[OUT];
    memset(strip, 0, sizeof strip);
    for (int y = 20; y < 60; y++) {
        strip[y] = (xspan_t){300, 380};
    }
    uint16_t sout_a[OUT * OUT];
    memcpy(sout_a, g_sout, sizeof sout_a);
    memset(&g_st, 0, sizeof g_st);
    acomp_step(&g_st, a->on, &a->a, NULL);
    assert(!memcmp(g_st.art, a->sp, sizeof g_st.art));
    full(a);
    uint16_t before[OUT * OUT];
    memcpy(before, g_panel, sizeof before);
    if (with_scene) {
        for (int y = 20; y < 60; y++) {
            for (int x = 300; x < 380; x++) {
                g_sout[y * OUT + x] ^= 0x0F0F;
            }
        }
    }
    const int n = acomp_step(&g_st, active, art_arg, with_scene ? strip : NULL);
    assert(n >= 0 && n <= ACOMP_MAX_BANDS && n == g_st.n_bands);
    assert(!memcmp(g_st.art, b->sp, sizeof g_st.art));
    xspan_t want[OUT];
    acomp_dirty(a->sp, b->sp, want, OUT);
    assert(!memcmp(g_st.dirty, want, sizeof want));
    if (!active) {
        xspan_t none[OUT];
        memset(none, 0, sizeof none);
        assert(!memcmp(g_st.art, none, sizeof none) && !memcmp(g_st.dirty, a->sp, sizeof want));
    }
    flush(b, g_st.bands, n);
    memcpy(g_want, g_panel, sizeof g_want);
    full(b);
    assert(!memcmp(g_want, g_panel, sizeof g_want));
    /* Red proof: the same tick with the previous spans dropped (or the scene strip omitted) leaves a stale panel. */
    memcpy(g_panel, before, sizeof before);
    memset(&g_ctl, 0, sizeof g_ctl);
    const int nc = a->on ? acomp_step(&g_ctl, active, art_arg, NULL) : 0;
    flush(b, g_ctl.bands, nc);
    assert(memcmp(g_panel, g_want, sizeof g_panel) != 0);
    if (with_scene) {
        /* Red proof: the same tick with the previous spans kept but no scene strip misses the strip's rows. */
        memcpy(g_panel, before, sizeof before);
        memset(&g_ctl, 0, sizeof g_ctl);
        acomp_step(&g_ctl, a->on, &a->a, NULL);
        flush(b, g_ctl.bands, acomp_step(&g_ctl, active, art_arg, NULL));
        assert(memcmp(g_panel, g_want, sizeof g_panel) != 0);
    }
    g_tick_cases++;
    memcpy(g_sout, sout_a, sizeof sout_a);
}

/* X9: acomp_step from every transition equals a full recomposition; each control without the previous spans differs. */
static void x9(void)
{
    g_arts[0] = make_art(120, 100, 2, 31);
    g_arts[1] = make_art(90, 130, 2, 32);
    fill_overlay(g_sout, 7);
    const state_t a0 = mk(true, 0, 2, 160, 170, 0), off = mk(false, 0, 2, 0, 0, 0), crop = mk(true, 1, 2, 150, 140, 0);
    const state_t orbit = mk(true, 0, 2, 164, 166, 0), a900 = mk(true, 0, 2, 160, 170, 900), a450 = mk(true, 0, 2, 160, 170, 450);
    /* In order: skin off, unmapped state, decode failure, fill set, fill cleared, panel off, panel on, clip cut, orbit step, 0 -> 900, 900 -> 450, scene strip. */
    tick_case(&a0, false, NULL, &off, false);
    tick_case(&a0, false, &a0.a, &off, false);
    tick_case(&a0, false, &crop.a, &off, false);
    tick_case(&a0, false, &a0.a, &off, false);
    tick_case(&off, true, &a0.a, &a0, false);
    tick_case(&a0, false, &a0.a, &off, false);
    tick_case(&off, true, &a0.a, &a0, false);
    tick_case(&a0, true, &crop.a, &crop, false);
    tick_case(&a0, true, &orbit.a, &orbit, false);
    tick_case(&a0, true, &a900.a, &a900, false);
    tick_case(&a900, true, &a450.a, &a450, false);
    tick_case(&a0, true, &a900.a, &a900, true);
    tick_case(&a0, false, NULL, &off, true);
    /* An output size other than the frame's is no art. */
    acomp_art_t odd = a0.a;
    odd.out = OUT - 1;
    memset(&g_st, 0, sizeof g_st);
    assert(acomp_step(&g_st, true, &odd, NULL) == 0);
    free(g_arts[0].mem);
    free(g_arts[1].mem);
    printf("X9: %d transitions equal the full recomposition; every control without the previous spans or the scene differs\n", g_tick_cases);
}

/* X10: the rows a chunk holds, split in two, compose to the whole chunk. */
static void x10(void)
{
    g_arts[0] = make_art(120, 100, 2, 41);
    fill_overlay(g_sout, 9);
    const state_t a = mk(true, 0, 2, 160, 170, 450);
    int ya, mid, yb;
    assert(!acomp_split(a.sp, 0, 20, &ya, &mid, &yb));
    assert(acomp_split(a.sp, 0, OUT, &ya, &mid, &yb) && ya < mid && mid < yb);
    assert(a.sp[ya].x0 < a.sp[ya].x1 && a.sp[yb - 1].x0 < a.sp[yb - 1].x1);
    for (int y = ya; y < yb; y++) {
        assert(a.sp[y].x0 < a.sp[y].x1);
    }
    int chunks = 0;
    for (int y0 = 0; y0 < OUT; y0 += 32) {
        const int y1 = y0 + 32 < OUT ? y0 + 32 : OUT;
        static uint16_t whole[OUT * 32], halves[OUT * 32];
        const size_t bytes = (size_t)(y1 - y0) * OUT * 2;
        memcpy(whole, g_sout + (size_t)y0 * OUT, bytes);
        memcpy(halves, whole, bytes);
        acomp_rows(&a.a, a.sp, g_pal, whole, OUT, 0, y0, OUT, y1);
        if (acomp_split(a.sp, y0, y1, &ya, &mid, &yb)) {
            assert(ya >= y0 && yb <= y1 && mid >= ya && mid <= yb);
            acomp_rows(&a.a, a.sp, g_pal, halves + (size_t)(ya - y0) * OUT, OUT, 0, ya, OUT, mid);
            acomp_rows(&a.a, a.sp, g_pal, halves + (size_t)(mid - y0) * OUT, OUT, 0, mid, OUT, yb);
            chunks++;
        }
        assert(!memcmp(whole, halves, bytes));
    }
    /* Red proof: composing only the first half leaves the other half's art out. */
    assert(acomp_split(a.sp, 160, 192, &ya, &mid, &yb) && mid > ya && mid < yb);
    static uint16_t one[OUT * 32], two[OUT * 32];
    memcpy(one, g_sout + 160 * OUT, sizeof one);
    memcpy(two, one, sizeof two);
    acomp_rows(&a.a, a.sp, g_pal, one, OUT, 0, 160, OUT, 192);
    acomp_rows(&a.a, a.sp, g_pal, two + (size_t)(ya - 160) * OUT, OUT, 0, ya, OUT, mid);
    assert(memcmp(one, two, sizeof one) != 0);
    free(g_arts[0].mem);
    printf("X10: %d chunks split in two equal the whole chunk\n", chunks);
}

/* Transcription of the conversions in ui.c, which cannot be linked on the host. */
static uint16_t rgb565(uint32_t hex)
{
    return (uint16_t)(((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x1F));
}

static uint32_t scale_hex(uint32_t hex, int percent)
{
    uint32_t r = ((hex >> 16) & 0xFF) * percent / 100, g = ((hex >> 8) & 0xFF) * percent / 100, b = (hex & 0xFF) * percent / 100;
    return (r << 16) | (g << 8) | b;
}

/* X7: every colour drawn over the art is non-zero, so it is never mistaken for a transparent pixel. */
static void x7(void)
{
    const int ring_scale = (int)(1.0f * (float)LISTEN_RING_MIN_OPA * 100.0f / 255.0f);
    const struct {
        const char *name, *where;
        uint16_t c;
    } col[] = {
        {"bubble fill", "ui.c:1152", rgb565(BUBBLE_FILL_COLOR)},
        {"bubble ink", "ui.c:1153", rgb565(BUBBLE_INK_COLOR)},
        {"error", "ui.c:1154", rgb565(BUBBLE_ERROR_COLOR)},
        {"offline fill", "ui.c:1155", rgb565(BUBBLE_OFFLINE_FILL_COLOR)},
        {"mascot full", "ui.c:197,374", rgb565(scale_hex(MASCOT_COLOR, 100))},
        {"mascot dimmed", "ui.c:197,450", rgb565(scale_hex(MASCOT_COLOR, MASCOT_DIM_PERCENT))},
        {"volume fill", "ui.c:1156", rgb565(BUBBLE_FILL_COLOR)},
        {"volume track", "ui.c:1157", rgb565(BUBBLE_OFFLINE_FILL_COLOR)},
        {"volume mute", "ui.c:1158", rgb565(BUBBLE_ERROR_COLOR)},
        {"brightness fill", "ui.c:1159", rgb565(BRT_ARC_FILL_COLOR)},
        {"brightness track", "ui.c:1160", rgb565(BRT_ARC_TRACK_COLOR)},
        {"ring, minimum opacity, full coverage", "ui.c:781", rgb565(scale_hex(MASCOT_COLOR, ring_scale))},
    };
    for (size_t i = 0; i < sizeof col / sizeof col[0]; i++) {
        assert(col[i].c != 0);
        printf("X7:   %-38s %-13s 0x%04X\n", col[i].name, col[i].where, col[i].c);
    }
    assert(PAL_BG == 0 && g_pal[PAL_BG] == 0);
    printf("X7: %zu overlay colours non-zero; background entry PAL_BG is 0 (by inspection ui.c:88,197,356,1152-1155 never write it)\n",
           sizeof col / sizeof col[0]);
}

/* The strip loop of ui.c present(), transcribed as the oracle for acomp_bands. */
static int oracle_bands(const xspan_t *sc, int n, int rows, irect_t *out)
{
    int ymin = n, ymax = 0, count = 0;
    for (int y = 0; y < n; y++) {
        if (!(sc[y].x0 >= sc[y].x1)) {
            ymin = y < ymin ? y : ymin;
            ymax = y + 1;
        }
    }
    for (int y0 = ymin; y0 < ymax; y0 += rows) {
        int y1 = y0 + rows < ymax ? y0 + rows : ymax, xa = n, xb = 0;
        for (int y = y0; y < y1; y++) {
            if (!(sc[y].x0 >= sc[y].x1)) {
                xa = sc[y].x0 < xa ? sc[y].x0 : xa;
                xb = sc[y].x1 > xb ? sc[y].x1 : xb;
            }
        }
        if (xa < xb) {
            out[count++] = (irect_t){xa, y0, xb, y1};
        }
    }
    return count;
}

/* X8: acomp_bands equals the loop it replaces; rects are x1/y1-exclusive and extra strips are counted, not stored. */
static void x8(void)
{
    uint32_t seed = 21;
    long cases = 0;
    static xspan_t sp[OUT];
    for (int k = 0; k < 400; k++) {
        const int rows[] = {1, 7, DIRTY_BAND_ROWS, 40, 500};
        const int br = rows[k % 5];
        for (int y = 0; y < OUT; y++) {
            const bool live = k % 7 == 0 ? false : (y > 30 + k % 50 && y < 400 - k % 90 && rnd(&seed) % (1 + k % 4) == 0);
            const int a = (int)(rnd(&seed) % 400), b = a + 1 + (int)(rnd(&seed) % 60);
            sp[y] = live ? (xspan_t){(int16_t)a, (int16_t)b} : (xspan_t){0, 0};
        }
        irect_t want[OUT], got[OUT];
        const int nw = oracle_bands(sp, OUT, br, want);
        assert(acomp_bands(sp, OUT, br, got, OUT) == nw && !memcmp(want, got, sizeof(irect_t) * (size_t)nw));
        if (nw > 1) {
            irect_t two[2] = {{9, 9, 9, 9}, {9, 9, 9, 9}};
            assert(acomp_bands(sp, OUT, br, two, 1) == nw && !memcmp(two, want, sizeof(irect_t)) && two[1].x0 == 9);
        }
        cases++;
    }
    static xspan_t one[OUT];
    one[200] = (xspan_t){10, 20};
    irect_t r[2];
    assert(acomp_bands(one, OUT, 40, r, 2) == 1 && r[0].x0 == 10 && r[0].x1 == 20 && r[0].y0 == 200 && r[0].y1 == 201);
    printf("X8: %ld span sets equal the ui.c strip loop, overflow counted without storing\n", cases);
}

int main(void)
{
    init_pal();
    x1();
    x2();
    x3();
    x4();
    x5();
    x6();
    x7();
    x8();
    x9();
    x10();
    return 0;
}
