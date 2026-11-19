#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aglue.h"
#include "arcvis.h"
#include "dpower.h"
#include "volarc.h"

#define PI_D 3.14159265358979323846
#define LAST (LCD_SIZE - 1)

static uint32_t g_store[VOLARC_CAP];
static volarc_t g_arc;

static int d2_of(int x, int y) { int dx = 2 * x - LAST, dy = 2 * y - LAST; return dx * dx + dy * dy; }

/* Geometric oracle of the table rules: the meter sector, its half-disc end caps and the stretch cells, from angles and distances only. */
enum { GK_VOL, GK_BRT };

static double geo_start(int kind) { return kind == GK_BRT ? BRT_ARC_START_DEG : VOL_ARC_START_DEG; }
static double geo_sweep(int kind) { return kind == GK_BRT ? BRT_ARC_SWEEP_DEG : VOL_SWEEP_DEG; }
static int geo_steps(int kind) { return kind == GK_BRT ? BRT_STEPS + 1 : 100; }

static double geo_t(int x, int y, double start)
{
    double a = atan2(2.0 * y - LAST, 2.0 * x - LAST) * 180.0 / PI_D, t = a - start;
    return t - 360.0 * floor(t / 360.0);
}

static int geo_in_band(int x, int y)
{
    int d2 = d2_of(x, y);
    return d2 >= 4 * VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX && d2 <= 4 * VOL_ARC_R_OUT_PX * VOL_ARC_R_OUT_PX;
}

/* Whether the pixel lies in the half-disc k cells past the meter end (high: 1) and outward of it. */
static int geo_in_cap(int kind, int x, int y, int high, int k)
{
    double start = geo_start(kind), sweep = geo_sweep(kind), p = sweep / geo_steps(kind), t = geo_t(x, y, start);
    double th = (high ? start + sweep + k * p : start - k * p) * PI_D / 180.0;
    double cx = (VOL_ARC_R_IN_PX + VOL_ARC_R_OUT_PX) * cos(th), cy = (VOL_ARC_R_IN_PX + VOL_ARC_R_OUT_PX) * sin(th);
    double r = VOL_ARC_R_OUT_PX - VOL_ARC_R_IN_PX, ex = (2.0 * x - LAST) - cx, ey = (2.0 * y - LAST) - cy;
    double rel = high ? t - (sweep + k * p) : (t > 180.0 ? t - 360.0 : t) + k * p;
    return (high ? rel >= 0 : rel < 0) && ex * ex + ey * ey <= r * r;
}

/* Cap code: 0 none, else 1 + k at the low end and 1 + ARC_OVER_CELLS + 1 + k at the high end. */
static int geo_cap(int kind, int x, int y)
{
    for (int high = 0; high < 2; high++) {
        for (int k = 0; k <= ARC_OVER_CELLS; k++) {
            if (geo_in_cap(kind, x, y, high, k)) {
                return 1 + high * (ARC_OVER_CELLS + 1) + k;
            }
        }
    }
    return 0;
}

static int geo_cap_end(int code) { return code > ARC_OVER_CELLS + 1; }
static int geo_cap_k(int code) { return code - 1 - geo_cap_end(code) * (ARC_OVER_CELLS + 1); }

static int geo_sector_thr(int kind, int x, int y)
{
    double t = geo_t(x, y, geo_start(kind)), sweep = geo_sweep(kind);
    return t >= sweep ? 0 : (int)floor(t * geo_steps(kind) / sweep) + 1;
}

/* The meter's pixel threshold: its sector, plus the resting caps at the end cells. */
static int geo_main_thr(int kind, int x, int y, int caps)
{
    int t = geo_sector_thr(kind, x, y), c = caps && !t ? geo_cap(kind, x, y) : 0;
    if (c && geo_cap_k(c) == 0) {
        return geo_cap_end(c) ? geo_steps(kind) : 1;
    }
    return t;
}

/* The stretch cell of a pixel by its sector alone. */
static int geo_over_sector_thr(int kind, int x, int y)
{
    double start = geo_start(kind), sweep = geo_sweep(kind), p = sweep / geo_steps(kind), t = geo_t(x, y, start), run = ARC_OVER_CELLS * p;
    if (t >= sweep && t - sweep < run) {
        int j = (int)floor((t - sweep) / p) + 1;
        return ARC_OVER_CELLS + (j > ARC_OVER_CELLS ? ARC_OVER_CELLS : j);
    }
    if (t > 0 && 360.0 - t < run) {
        int j = (int)floor((360.0 - t) / p) + 1;
        return j > ARC_OVER_CELLS ? ARC_OVER_CELLS : j;
    }
    return 0;
}

/* The stretch-table threshold: sector cells, with the tip caps relabelled to the cell count they sit at and the resting caps left to the meter. */
static int geo_over_thr(int kind, int x, int y, int caps)
{
    if (!caps) {
        return geo_over_sector_thr(kind, x, y);
    }
    int c = geo_cap(kind, x, y);
    if (c && geo_sector_thr(kind, x, y) == 0) {
        int k = geo_cap_k(c);
        return k == 0 ? 0 : (geo_cap_end(c) ? ARC_OVER_CELLS : 0) + k;
    }
    return geo_over_sector_thr(kind, x, y);
}

static int ui_gap(int x, int y)
{
    double a = atan2(2.0 * y - LAST, 2.0 * x - LAST) * 180.0 / PI_D, t = a - VOL_ARC_START_DEG;
    t -= 360.0 * floor(t / 360.0);
    return t >= VOL_SWEEP_DEG;
}

static void test_a1_band_and_order(void)
{
    int n = volarc_build(&g_arc, g_store, VOLARC_CAP);
    double ideal = 0.75 * PI_D * ((double)VOL_ARC_R_OUT_PX * VOL_ARC_R_OUT_PX - (double)VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX);
    printf("arc r %d-%d: %d px, ideal %.0f\n", VOL_ARC_R_IN_PX, VOL_ARC_R_OUT_PX, n, ideal);
    assert(n > 0 && fabs(n - ideal) <= 0.03 * ideal);
    static unsigned char seen[LCD_SIZE][LCD_SIZE];
    int prev = 0;
    for (int i = 0; i < n; i++) {
        int x, y, t = volarc_thr(g_arc.e[i]);
        volarc_xy(g_arc.e[i], 0, &x, &y);
        int d2 = d2_of(x, y);
        assert(d2 >= 4 * VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX && d2 <= 4 * VOL_ARC_R_OUT_PX * VOL_ARC_R_OUT_PX);
        assert(t >= prev && t >= 1 && t <= 100 && !seen[y][x] && t == geo_main_thr(GK_VOL, x, y, 1) && (!ui_gap(x, y) || geo_cap(GK_VOL, x, y)));
        seen[y][x] = 1;
        prev = t;
    }
    for (int t = 1; t <= 100; t++) { assert(g_arc.cnt[t] - g_arc.cnt[t - 1] >= 20); }
    assert(g_arc.cnt[100] == n);
    int gap_in_band = 0;
    for (int y = 0; y < LCD_SIZE; y++) {
        for (int x = 0; x < LCD_SIZE; x++) {
            int d2 = d2_of(x, y);
            if (d2 >= 4 * VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX && d2 <= 4 * VOL_ARC_R_OUT_PX * VOL_ARC_R_OUT_PX && ui_gap(x, y)) {
                assert(seen[y][x] == (geo_main_thr(GK_VOL, x, y, 1) > 0));
                gap_in_band++;
            }
        }
    }
    assert(gap_in_band > 0);
    for (int q = 1; q < 4; q++) {
        static unsigned char map[LCD_SIZE][LCD_SIZE];
        memset(map, 0, sizeof(map));
        for (int i = 0; i < n; i++) {
            int x0, y0, x, y;
            volarc_xy(g_arc.e[i], 0, &x0, &y0);
            volarc_xy(g_arc.e[i], q, &x, &y);
            assert(!map[y][x] && d2_of(x, y) == d2_of(x0, y0));
            map[y][x] = 1;
            if (q == 1) { assert(x == LAST - y0 && y == x0); }
            if (q == 2) { assert(x == LAST - x0 && y == LAST - y0); }
        }
    }
    /* a band reaching the ring's outermost rewritten pixels would be hit by the ring redraw */
    assert(4 * VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX > 4 * (LISTEN_RING_DIAMETER / 2) * (LISTEN_RING_DIAMETER / 2));
}

typedef struct { int content_u, thought, dx_min, dx_max, dy_min, dy_max; } scene_case_t;

static const scene_case_t CLAWD_CASES[] = {
    {0, 0, -4 * SCUTTLE_DISTANCE_UNITS, 4 * SCUTTLE_DISTANCE_UNITS, 0, 4},  /* IDLE */
    {34, 0, 0, 0, 0, 0},                                                      /* LISTENING */
    {33, 1, 0, 0, 0, 0},                                                      /* THINKING */
    {34, 0, 0, 0, -2 * BUBBLE_UNIT_PX, 0},                                    /* SPEAKING */
    {36, 0, -BUBBLE_UNIT_PX, BUBBLE_UNIT_PX, 0, 0},                           /* ERROR */
    {24, 0, 0, 0, 0, BUBBLE_UNIT_PX},                                         /* OFFLINE */
};
enum { CASE_LISTENING = 1, CASE_THINKING = 2 };

/* Corner radius of the one bounding rectangle ui.c clears and rotates, for one state over the orbit, with its 1 px margin. */
static double case_corner(const scene_case_t *s)
{
    const int u = BUBBLE_UNIT_PX, c = MASCOT_CELL_PX, half = SCENE_SIZE_PX / 2;
    double worst = 0;
    int shift = s->content_u ? BUBBLE_SHIFT_UNITS * u : 0;
    for (int i = 0; i < ORBIT_COLS * ORBIT_ROWS; i++) {
        int gx, gy;
        dp_orbit(i, &gx, &gy);
        for (int dxi = 0; dxi < 2; dxi++) {
            for (int dyi = 0; dyi < 2; dyi++) {
                int dx = dxi ? s->dx_max : s->dx_min, dy = dyi ? s->dy_max : s->dy_min;
                int ox = half - 8 * c + gx + dx, oy = half - 5 * c + gy + shift + dy;
                int x0 = ox, x1 = ox + 16 * c, y0 = oy, y1 = oy + 10 * c;
                if (s->content_u) {
                    int w = (s->content_u + 2 * BUBBLE_PAD_UNITS + 2 + 1) & ~1, cx = (half + gx) / u;
                    int anchor_top = half - 5 * c + shift + gy, below = s->thought ? 8 : 2;
                    int bottom_u = (anchor_top - BUBBLE_GAP_UNITS * u) / u - below;
                    x0 = x0 < (cx - w / 2) * u ? x0 : (cx - w / 2) * u;
                    x1 = x1 > (cx - w / 2 + w) * u ? x1 : (cx - w / 2 + w) * u;
                    y0 = y0 < (bottom_u - 18) * u ? y0 : (bottom_u - 18) * u;
                }
                double ax = fmax(abs(x0 - 1 - half), abs(x1 + 1 - half)), ay = fmax(abs(y0 - 1 - half), abs(y1 + 1 - half));
                worst = fmax(worst, hypot(ax, ay));
            }
        }
    }
    return worst;
}

static double worst_corner(void)
{
    double worst = 0;
    for (size_t k = 0; k < sizeof(CLAWD_CASES) / sizeof(CLAWD_CASES[0]); k++) {
        worst = fmax(worst, case_corner(&CLAWD_CASES[k]));
    }
    return worst;
}

static void test_a2_scene_clear_of_arc(void)
{
    double w = worst_corner();
    printf("scene worst corner %.1f\n", w);
    assert(w + 2 < VOL_ARC_R_IN_PX && w <= SCENE_MAX_RADIUS_PX);
    assert(!(w + 2 < 211));
}

static double rect_corner(ag_rect_t r)
{
    const int half = SCENE_SIZE_PX / 2;
    return hypot(fmax(abs(r.x - 1 - half), abs(r.x + r.w + 1 - half)), fmax(abs(r.y - 1 - half), abs(r.y + r.h + 1 - half)));
}

/* A2s: the sticker skin's bubble, tail and dots, over every width, side, bub_y, pop and orbit point. */
static void test_a2s_sticker_scene_clear_of_arc(void)
{
    static const struct {
        int content_u;
        bool thought;
    } W[4] = {{34, false}, {36, false}, {24, false}, {33, true}};
    double worst = 0;
    long n = 0;
    for (int k = 0; k < 4; k++) {
        const int w = ag_bubble_w_units(W[k].content_u);
        for (int left = 0; left < 2; left++) {
            for (int y = -AG_BUBBLE_Y_MAX_PX; y <= AG_BUBBLE_Y_MAX_PX; y += 4) {
                for (int i = 0; i < ORBIT_COLS * ORBIT_ROWS; i++) {
                    int gx, gy;
                    dp_orbit(i, &gx, &gy);
                    for (int pop = 1; pop <= 3; pop++) {
                        const ag_bubble_t b = ag_side_bubble(w, W[k].content_u, W[k].thought, left, y, gx, gy, pop, SCENE_SIZE_PX);
                        worst = fmax(worst, rect_corner(b.body));
                        for (int c = 0; c < b.n_cells; c++) {
                            worst = fmax(worst, rect_corner(b.cells[c].r));
                        }
                        n++;
                    }
                }
            }
        }
    }
    printf("A2s: %ld sticker scenes, worst corner %.2f\n", n, worst);
    assert(worst + 2 < SCENE_RING_CLEAR_PX && worst + 2 < VOL_ARC_R_IN_PX);
    /* Red proofs: outer edge 192 at bub_y 0 gives 211.3; the raw lattice on the left, without the recentre, 212.05. */
    double edge192 = 0, raw_left = 0;
    for (int i = 0; i < ORBIT_COLS * ORBIT_ROWS; i++) {
        int gx, gy;
        dp_orbit(i, &gx, &gy);
        const int half = SCENE_SIZE_PX / 2, w = 160;
        edge192 = fmax(edge192, rect_corner((ag_rect_t){half + gx + ORBIT_RECENTRE_X_PX + 192 - w, half + gy + ORBIT_RECENTRE_Y_PX - 36, w, 72}));
        raw_left = fmax(raw_left, rect_corner((ag_rect_t){half + gx - 188, half + gy - 36, w, 72}));
    }
    printf("A2s red: edge 192 %.2f, raw lattice left %.2f\n", edge192 + 2, raw_left + 2);
    assert(!(edge192 + 2 < 211) && fabs(edge192 + 2 - 211.3) < 0.05);
    assert(!(raw_left + 2 < 211) && fabs(raw_left + 2 - 212.05) < 0.05);
}

/* A2r: the LISTENING scene in both skins; the sticker side sweeps every bub_y. */
static void test_a2r_listening_clear_of_arc(void)
{
    const double clawd = case_corner(&CLAWD_CASES[CASE_LISTENING]);
    double sticker = 0;
    for (int left = 0; left < 2; left++) {
        for (int i = 0; i < ORBIT_COLS * ORBIT_ROWS; i++) {
            int gx, gy;
            dp_orbit(i, &gx, &gy);
            for (int y = -AG_BUBBLE_Y_MAX_PX; y <= AG_BUBBLE_Y_MAX_PX; y += 4) {
                const ag_bubble_t b = ag_side_bubble(ag_bubble_w_units(34), 34, false, left, y, gx, gy, 3, SCENE_SIZE_PX);
                sticker = fmax(sticker, rect_corner(b.body));
            }
        }
    }
    const double thinking = case_corner(&CLAWD_CASES[CASE_THINKING]);
    printf("A2r: LISTENING Clawd %.1f sticker %.1f; THINKING Clawd %.1f\n", clawd, sticker, thinking);
    assert(clawd + 2 < 211 && sticker + 2 < 211);
    /* Red proof: the THINKING scene, with its spinner and thought dots, is the one that does not fit. */
    assert(!(thinking + 2 < 211));
}

static int rounded_px(const volarc_rect_t *r, int n)
{
    int px = 0;
    for (int i = 0; i < n; i++) {
        int w = ((r[i].x2 | 1) - (r[i].x1 & ~1) + 1), h = ((r[i].y2 | 1) - (r[i].y1 & ~1) + 1);
        px += w * h;
    }
    return px;
}

static int frame_areas(int quad, int lo, int hi, int *px)
{
    volarc_rect_t r[VOLARC_MAX_AREAS];
    int n = volarc_areas(&g_arc, quad, lo, hi, r, VOLARC_MAX_AREAS);
    *px = rounded_px(r, n);
    return n;
}

/* One frame's arc areas must leave room in LVGL's 32 slots for the scene (10) and, when the arc has one area, the ring (21). */
static void test_area_budget(void)
{
    volarc_build(&g_arc, g_store, VOLARC_CAP);
    int px, worst_single = 0;
    int full = frame_areas(0, 0, 100, &px);
    printf("r_out %d: full draw/clear %d areas %d px", VOL_ARC_R_OUT_PX, full, px);
    assert(full <= VOLARC_MAX_AREAS && full > 1);
    int skip_ring_frame = full + 10;
    assert(skip_ring_frame <= 32);
    int step_max = 0, step_px_max = 0;
    for (int v = 0; v < 100; v++) {
        int lo, hi, n;
        volarc_change_range(v, v + 1, &lo, &hi);
        for (int q = 0; q < 4; q++) {
            n = frame_areas(q, lo, hi, &px);
            assert(n == 1);
            step_max = n > step_max ? n : step_max;
            step_px_max = px > step_px_max ? px : step_px_max;
        }
    }
    printf("; 1%% step %d area, at most %d px", step_max, step_px_max);
    assert(step_max + 10 + 21 <= 32);
    int lo, hi;
    volarc_change_range(0, 1, &lo, &hi);
    assert(lo == 0 && hi == VOL_ARC_MUTE_THR);
    int a, b;
    for (int q = 0; q < 4; q++) {
        a = frame_areas(q, 0, 100, &px);
        b = frame_areas((q + 1) & 3, 0, 100, &px);
        worst_single = a + b > worst_single ? a + b : worst_single;
    }
    printf("; re-anchor %d areas (+10 scene = %d of 32)\n", worst_single, worst_single + 10);
    assert(worst_single + 10 <= 32);
    int big = frame_areas(0, 5, 100, &px);
    assert(big <= VOLARC_MAX_AREAS);
}

static void test_class(void)
{
    assert(volarc_class(1, 0) == VOLARC_MUTE && volarc_class(VOL_ARC_MUTE_THR, 0) == VOLARC_MUTE && volarc_class(VOL_ARC_MUTE_THR + 1, 0) == VOLARC_TRACK);
    assert(volarc_class(5, 5) == VOLARC_FILL && volarc_class(6, 5) == VOLARC_TRACK && volarc_class(1, 1) == VOLARC_FILL);
}

static void test_linger(void)
{
    arcvis_t v;
    av_init(&v);
    uint32_t t = 0xFFFFFF00u;
    av_arm(&v, 40);
    avact_t a = av_frame(&v, t, 0);
    assert(a.flags == AV_DRAW_NEW && a.val == 40);
    av_set(&v, 55);
    a = av_frame(&v, t + 16, 0);
    assert(a.flags == AV_DIFF && a.from == 40 && a.to == 55);
    av_end(&v, 55, t + 100);
    for (uint32_t d = 100; d < 100 + VOL_ARC_LINGER_MS; d += 16) {
        assert(av_visible(&v, t + d) && av_frame(&v, t + d, 0).flags == 0);
    }
    assert(av_visible(&v, t + 100 + VOL_ARC_LINGER_MS - 1));
    assert(!av_visible(&v, t + 100 + VOL_ARC_LINGER_MS));
    a = av_frame(&v, t + 100 + VOL_ARC_LINGER_MS, 0);
    assert(a.flags == AV_CLEAR_OLD && a.old_quad == 0);
    assert(av_frame(&v, t + 100 + VOL_ARC_LINGER_MS + 16, 0).flags == 0);
}

static void test_rearm_takes_over(void)
{
    arcvis_t v;
    av_init(&v);
    av_arm(&v, 40);
    av_frame(&v, 0, 0);
    av_end(&v, 40, 1000);
    assert(av_frame(&v, 1500, 0).flags == 0);
    av_arm(&v, 40);
    for (uint32_t t = 1500; t < 4000; t += 16) {
        assert(!(av_frame(&v, t, 0).flags & AV_CLEAR_OLD));
    }
    av_end(&v, 40, 4000);
    assert(av_frame(&v, 4000 + VOL_ARC_LINGER_MS, 0).flags == AV_CLEAR_OLD);
}

static void test_cancel_shows_restored_value(void)
{
    arcvis_t v;
    av_init(&v);
    av_arm(&v, 40);
    av_frame(&v, 0, 0);
    av_set(&v, 70);
    av_frame(&v, 16, 0);
    av_end(&v, 40, 32);
    avact_t a = av_frame(&v, 48, 0);
    assert(a.flags == AV_DIFF && a.from == 70 && a.to == 40);
    assert(av_visible(&v, 32 + VOL_ARC_LINGER_MS - 1));
    assert(av_frame(&v, 32 + VOL_ARC_LINGER_MS, 0).flags == AV_CLEAR_OLD);
}

static void test_quadrant_and_force(void)
{
    arcvis_t v;
    av_init(&v);
    av_arm(&v, 40);
    av_frame(&v, 0, 0);
    avact_t a = av_frame(&v, 16, 1);
    assert(a.flags == (AV_CLEAR_OLD | AV_DRAW_NEW) && a.old_quad == 0 && a.quad == 1);
    av_end(&v, 40, 20);
    a = av_frame(&v, 400, 2);
    assert(a.flags == (AV_CLEAR_OLD | AV_DRAW_NEW) && a.old_quad == 1 && a.quad == 2);
    av_init(&v);
    av_force(&v, 50);
    assert(av_frame(&v, 0, 0).flags == AV_DRAW_NEW);
    av_force(&v, -1);
    assert(av_frame(&v, 1, 0).flags == AV_CLEAR_OLD);
}

static uint32_t g_seg_store[VOLARC_CAP];
static volarc_t g_seg;

/* Smallest visible cell: 60 px in the 222-230 band, scaled with the band area for other bands. */
#define SEG_CELL_MIN_PX (60 * (VOL_ARC_R_OUT_PX * VOL_ARC_R_OUT_PX - VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX) / (230 * 230 - 222 * 222))

static int seg_build(volarc_t *a, uint32_t *store, int steps)
{
    return volarc_build_seg(a, store, VOLARC_CAP, BRT_ARC_START_DEG, BRT_ARC_SWEEP_DEG, steps);
}

static void test_a4_segment_geometry(void)
{
    int n = seg_build(&g_seg, g_seg_store, BRT_STEPS + 1);
    printf("brightness arc: %d px", n);
    assert(n > 0 && g_seg.steps == BRT_STEPS + 1 && g_seg.cnt[BRT_STEPS + 1] == n);
    static unsigned char seen[LCD_SIZE][LCD_SIZE];
    int prev = 0, min_cell = n, bx1 = LCD_SIZE, by1 = LCD_SIZE, bx2 = -1, by2 = -1;
    for (int i = 0; i < n; i++) {
        int x, y, t = volarc_thr(g_seg.e[i]);
        volarc_xy(g_seg.e[i], 0, &x, &y);
        int d2 = d2_of(x, y);
        double a = atan2(2.0 * y - LAST, 2.0 * x - LAST) * 180.0 / PI_D;
        a += a < 0 ? 360.0 : 0.0;
        assert(d2 >= 4 * VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX && d2 <= 4 * VOL_ARC_R_OUT_PX * VOL_ARC_R_OUT_PX);
        assert(((a >= BRT_ARC_START_DEG && a < BRT_ARC_START_DEG + BRT_ARC_SWEEP_DEG) || geo_cap(GK_BRT, x, y)) && t == geo_main_thr(GK_BRT, x, y, 1) && t >= prev && t >= 1 && t <= BRT_STEPS + 1 && !seen[y][x]);
        seen[y][x] = 1;
        prev = t;
        bx1 = x < bx1 ? x : bx1;
        by1 = y < by1 ? y : by1;
        bx2 = x > bx2 ? x : bx2;
        by2 = y > by2 ? y : by2;
    }
    for (int t = 1; t <= BRT_STEPS + 1; t++) {
        int c = g_seg.cnt[t] - g_seg.cnt[t - 1];
        min_cell = c < min_cell ? c : min_cell;
    }
    printf(", smallest cell %d px, box x %d-%d y %d-%d\n", min_cell, bx1, bx2, by1, by2);
    assert(min_cell >= SEG_CELL_MIN_PX);
    int x_lo, y_lo, x_hi, y_hi;
    volarc_xy(g_seg.e[0], 0, &x_lo, &y_lo);
    volarc_xy(g_seg.e[n - 1], 0, &x_hi, &y_hi);
    assert(x_lo < LCD_SIZE / 2 && x_hi < LCD_SIZE / 2 && y_lo > y_hi);
    for (int q = 1; q < 4; q++) {
        static unsigned char map[LCD_SIZE][LCD_SIZE];
        memset(map, 0, sizeof(map));
        for (int i = 0; i < n; i++) {
            int x0, y0, x, y;
            volarc_xy(g_seg.e[i], 0, &x0, &y0);
            volarc_xy(g_seg.e[i], q, &x, &y);
            assert(!map[y][x] && d2_of(x, y) == d2_of(x0, y0));
            map[y][x] = 1;
        }
    }
    /* One cell per percent over the same sweep leaves cells too small to see: the red proof. */
    static uint32_t wrong_store[VOLARC_CAP];
    volarc_t wrong;
    seg_build(&wrong, wrong_store, 100);
    int wrong_min = n;
    for (int t = 1; t <= 100; t++) {
        int c = wrong.cnt[t] - wrong.cnt[t - 1];
        wrong_min = c < wrong_min ? c : wrong_min;
    }
    assert(wrong_min < SEG_CELL_MIN_PX);
}

/* Red proof for A5: fill up to the index itself. */
static int wrong_seg_class(int thr, int bi)
{
    return thr <= bi ? VOLARC_FILL : VOLARC_TRACK;
}

static void test_a5_minimum_is_visible(void)
{
    for (int bi = 0; bi <= BRT_STEPS; bi++) {
        int fill = 0;
        for (int t = 1; t <= BRT_STEPS + 1; t++) {
            int c = volarc_seg_class(t, bi);
            assert(c == VOLARC_FILL || c == VOLARC_TRACK);
            fill += c == VOLARC_FILL;
            assert((c == VOLARC_FILL) == (t <= bi + 1));
        }
        assert(fill == bi + 1);
    }
    assert(volarc_seg_class(1, 0) == VOLARC_FILL && volarc_seg_class(2, 0) == VOLARC_TRACK && volarc_seg_class(BRT_STEPS + 1, BRT_STEPS) == VOLARC_FILL);
    for (int a = 0; a <= BRT_STEPS; a++) {
        for (int b = 0; b <= BRT_STEPS; b++) {
            int lo, hi;
            volarc_seg_change_range(a, b, &lo, &hi);
            for (int t = 1; t <= BRT_STEPS + 1; t++) {
                assert((volarc_seg_class(t, a) != volarc_seg_class(t, b)) == (t > lo && t <= hi));
            }
        }
    }
    assert(wrong_seg_class(1, 0) == VOLARC_TRACK);
}

static int seg_areas(const volarc_t *a, int quad, int lo, int hi, int chunk, int *px)
{
    volarc_rect_t r[BRT_STEPS + 1];
    int n = volarc_areas_chunk(a, quad, lo, hi, chunk, r, BRT_STEPS + 1);
    *px = rounded_px(r, n);
    return n;
}

/* Areas of one frame as ui.c spends them: the arc, the scene's bands, and the ring's 21 only when volarc_ring_ok. */
static int frame_total(int arc, int bands)
{
    return arc + bands + (volarc_ring_ok(arc, bands) ? VOLARC_RING_AREAS : 0);
}

/* A6 and A12: arc + bands + ring stays within LVGL's 32 areas for every band count and arc area count; the band condition is what keeps it so. */
static void test_ring_band_budget(void)
{
    int worst = 0, wrong = 0;
    for (int bands = 0; bands <= 12; bands++) {
        for (int arc = 0; arc <= 20; arc++) {
            const int t = frame_total(arc, bands);
            worst = t > worst ? t : worst;
            const int no_band = arc + bands + (arc <= VOLARC_RING_MAX_AREAS ? VOLARC_RING_AREAS : 0);
            wrong = no_band > wrong ? no_band : wrong;
        }
    }
    printf("ring budget: bands 0..12, arc 0..20: at most %d of 32; without the band condition %d\n", worst, wrong);
    assert(worst <= 32 && VOLARC_RING_MAX_BANDS == 10);
    assert(wrong == 34);
    assert(volarc_ring_ok(1, 10) && !volarc_ring_ok(1, 11) && !volarc_ring_ok(2, 10) && volarc_ring_ok(0, 0));
}

static void test_a6_segment_budget(void)
{
    volarc_build(&g_arc, g_store, VOLARC_CAP);
    seg_build(&g_seg, g_seg_store, BRT_STEPS + 1);
    int px, step_px = 0, worst = 0;
    int whole = seg_areas(&g_seg, 0, 0, BRT_STEPS + 1, BRT_ARC_CHUNK_CELLS, &px);
    printf("brightness arc: whole %d areas %d px", whole, px);
    assert(whole <= 4 && frame_total(whole, 10) <= 32);
    for (int q = 0; q < 4; q++) {
        assert(seg_areas(&g_seg, q, 0, BRT_STEPS + 1, BRT_ARC_CHUNK_CELLS, &px) == whole);
        for (int bi = 0; bi < BRT_STEPS; bi++) {
            int lo, hi;
            volarc_seg_change_range(bi, bi + 1, &lo, &hi);
            assert(seg_areas(&g_seg, q, lo, hi, BRT_ARC_CHUNK_CELLS, &px) == 1);
            step_px = px > step_px ? px : step_px;
        }
        for (int a = 0; a <= BRT_STEPS; a++) {
            for (int b = 0; b <= BRT_STEPS; b++) {
                int lo, hi;
                volarc_seg_change_range(a, b, &lo, &hi);
                int t = frame_total(seg_areas(&g_seg, q, lo, hi, BRT_ARC_CHUNK_CELLS, &px), 10);
                worst = t > worst ? t : worst;
            }
        }
    }
    int vol_whole = frame_areas(0, 0, 100, &px);
    int takeover = vol_whole + whole;
    printf("; 1 step 1 area, at most %d px; any step at most %d of 32; takeover %d (+10 scene = %d of 32)\n", step_px, worst, takeover, takeover + 10);
    assert(worst <= 32 && frame_total(takeover, 10) <= 32 && takeover <= 22);
    /* One area per cell breaks the takeover sum: the red proof. */
    assert(vol_whole + seg_areas(&g_seg, 0, 0, BRT_STEPS + 1, 1, &px) > 22);
}

/* Every volume change in one frame, however many chunks it crosses, stays inside LVGL's 32 areas. */
static void test_volume_multi_step_budget(void)
{
    volarc_build(&g_arc, g_store, VOLARC_CAP);
    int worst = 0, px;
    for (int q = 0; q < 4; q++) {
        for (int a = 0; a <= 100; a++) {
            for (int b = a + 1; b <= 100; b++) {
                int lo, hi;
                volarc_change_range(a, b, &lo, &hi);
                int t = frame_total(frame_areas(q, lo, hi, &px), 10);
                worst = t > worst ? t : worst;
            }
        }
    }
    printf("volume arc: any step at most %d of 32\n", worst);
    assert(worst <= 32);
}

static void test_a7_one_meter(void)
{
    arcvis_t v;
    av_init(&v);
    av_arm(&v, 40);
    av_frame(&v, 0, 0);
    av_end(&v, 40, 100);
    assert(av_frame(&v, 200, 0).flags == 0);
    av_arm_kind(&v, AV_BRT, 16, 0);
    avact_t a = av_frame(&v, 216, 0);
    assert(a.flags == (AV_CLEAR_OLD | AV_DRAW_NEW) && a.old_kind == AV_VOL && a.old_quad == 0 && a.kind == AV_BRT && a.val == 16);
    av_set(&v, 17);
    a = av_frame(&v, 232, 0);
    assert(a.flags == AV_DIFF && a.kind == AV_BRT && a.from == 16 && a.to == 17);
    av_end(&v, 17, 248);
    assert(av_frame(&v, 300, 0).flags == 0);
    av_arm(&v, 55);
    a = av_frame(&v, 316, 0);
    assert(a.flags == (AV_CLEAR_OLD | AV_DRAW_NEW) && a.old_kind == AV_BRT && a.kind == AV_VOL && a.val == 55);
    av_end(&v, 55, 400);
    assert(av_frame(&v, 400 + VOL_ARC_LINGER_MS, 0).flags == AV_CLEAR_OLD);
    /* One instance per kind draws the second meter while the first still lingers: the red proof. */
    arcvis_t wrong_vol, wrong_brt;
    av_init(&wrong_vol);
    av_init(&wrong_brt);
    av_arm(&wrong_vol, 40);
    av_frame(&wrong_vol, 0, 0);
    av_end(&wrong_vol, 40, 100);
    av_arm_kind(&wrong_brt, AV_BRT, 16, 0);
    assert(av_frame(&wrong_vol, 216, 0).flags == 0 && av_frame(&wrong_brt, 216, 0).flags == AV_DRAW_NEW);
}

static void test_a8_latched_quadrant(void)
{
    arcvis_t v;
    av_init(&v);
    av_arm_kind(&v, AV_BRT, 16, 0);
    assert(av_frame(&v, 0, 0).flags == AV_DRAW_NEW);
    assert(av_frame(&v, 16, 1).flags == 0);
    av_set(&v, 17);
    avact_t a = av_frame(&v, 32, 1);
    assert(a.flags == AV_DIFF && a.quad == 0);
    av_end(&v, 17, 48);
    assert(av_frame(&v, 64, 2).flags == 0);
    av_arm(&v, 40);
    a = av_frame(&v, 80, 1);
    assert(a.flags == (AV_CLEAR_OLD | AV_DRAW_NEW) && a.old_quad == 0 && a.quad == 1);
    a = av_frame(&v, 96, 2);
    assert(a.flags == (AV_CLEAR_OLD | AV_DRAW_NEW) && a.old_quad == 1 && a.quad == 2);
    /* Following the target re-anchors the segment mid-swipe: the red proof. */
    arcvis_t wrong;
    av_init(&wrong);
    av_arm_kind(&wrong, AV_BRT, 16, -1);
    av_frame(&wrong, 0, 0);
    assert(av_frame(&wrong, 16, 1).flags == (AV_CLEAR_OLD | AV_DRAW_NEW));
    av_init(&v);
    av_force_kind(&v, AV_BRT, 11);
    a = av_frame(&v, 0, 3);
    assert(a.flags == AV_DRAW_NEW && a.kind == AV_BRT && a.quad == 3 && a.val == 11);
}

/* Wrong rule for A9: one cell per frame instead of one per ARC_SPRING_STEP_MS. */
static int wrong_spring_per_frame(int k0, int frames)
{
    int left = k0 - 1 - frames + 1;
    return left < 0 ? 0 : left;
}

static void test_a9_spring(void)
{
    const uint32_t t = 0xFFFFFF80u, dts[5] = {16, 37, 50, 100, 250};
    const int kinds[2] = {AV_VOL, AV_BRT};
    for (int ki = 0; ki < 2; ki++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            for (int k0 = 1; k0 <= ARC_OVER_CELLS; k0++) {
                for (int di = 0; di < 5; di++) {
                    arcvis_t v;
                    av_init(&v);
                    int val = sign > 0 ? (kinds[ki] == AV_BRT ? BRT_STEPS : 100) : 0;
                    av_arm_kind(&v, kinds[ki], val, 0);
                    av_over(&v, sign * k0);
                    avact_t a = av_frame(&v, t, 0);
                    assert(a.flags == AV_DRAW_NEW && a.over_to == sign * k0);
                    av_end(&v, val, t + 10);
                    int shown = sign * k0, frames = 0;
                    bool gone = false;
                    for (uint32_t e = dts[di]; e < VOL_ARC_LINGER_MS; e += dts[di]) {
                        a = av_frame(&v, t + 10 + e, 0);
                        frames++;
                        int want = av_spring(sign * k0, e);
                        int left = k0 - 1 - (int)(e / ARC_SPRING_STEP_MS);
                        assert(want == sign * (left > 0 ? left : 0));
                        if (want != shown) {
                            assert(a.flags == AV_OVER && a.over_from == shown && a.over_to == want);
                            shown = want;
                        } else {
                            assert(a.flags == 0);
                        }
                        if (shown == 0 && !gone) {
                            gone = true;
                            assert(e <= (uint32_t)(k0 - 1) * ARC_SPRING_STEP_MS + dts[di]);
                        }
                        if (k0 == 3 && dts[di] == 16 && frames == 3) {
                            assert(shown == sign * 2 && wrong_spring_per_frame(k0, frames) == 0);
                        }
                    }
                    assert(shown == 0 && gone);
                    a = av_frame(&v, t + 10 + VOL_ARC_LINGER_MS, 0);
                    assert(a.flags == AV_CLEAR_OLD && a.old_over == 0);
                }
            }
        }
    }
    assert(av_spring(3, 16) == 2 && av_spring(3, 48) == 2 && av_spring(3, 64) == 1 && av_spring(3, 112) == 1 && av_spring(3, 128) == 0);
    assert(av_spring(3, 100) == 1 && av_spring(3, 200) == 0 && av_spring(2, 100) == 0 && av_spring(-3, 100) == -1 && av_spring(0, 100) == 0);
}

/* Wrong rule for A10: spring from the requested count. */
static int wrong_spring_from_requested(int requested, uint32_t elapsed)
{
    return av_spring(requested, elapsed);
}

static void test_a10_no_growth(void)
{
    arcvis_t v;
    av_init(&v);
    av_arm(&v, 100);
    av_over(&v, 1);
    av_frame(&v, 1000, 0);
    av_over(&v, ARC_OVER_CELLS);
    av_end(&v, 100, 1016);
    assert(v.over == 1);
    assert(av_frame(&v, 1046, 0).flags == AV_OVER && v.drawn_over == 0);
    assert(wrong_spring_from_requested(ARC_OVER_CELLS, 30) == ARC_OVER_CELLS - 1);
    av_init(&v);
    av_arm(&v, 100);
    av_over(&v, ARC_OVER_CELLS);
    av_end(&v, 100, 5);
    assert(v.over == 0);
    av_init(&v);
    av_arm(&v, 100);
    av_over(&v, 2);
    av_frame(&v, 0, 0);
    av_over(&v, -2);
    av_end(&v, 100, 5);
    assert(v.over == 0);
}

static uint32_t g_ov_store[2][VOLARC_OVER_CAP];
static volarc_t g_ov[2];

static double pitch_of(int kind)
{
    return kind == AV_BRT ? (double)BRT_ARC_SWEEP_DEG / (BRT_STEPS + 1) : (double)VOL_SWEEP_DEG / 100;
}

static void setup_tables(void)
{
    volarc_build(&g_arc, g_store, VOLARC_CAP);
    seg_build(&g_seg, g_seg_store, BRT_STEPS + 1);
    assert(volarc_build_over(&g_ov[AV_VOL], g_ov_store[AV_VOL], VOLARC_OVER_CAP, VOL_ARC_START_DEG, VOL_SWEEP_DEG, 100) > 0);
    assert(volarc_build_over(&g_ov[AV_BRT], g_ov_store[AV_BRT], VOLARC_OVER_CAP, BRT_ARC_START_DEG, BRT_ARC_SWEEP_DEG, BRT_STEPS + 1) > 0);
}

static const volarc_t *main_tab(int kind) { return kind == AV_BRT ? &g_seg : &g_arc; }
static int chunk_of(int kind) { return kind == AV_BRT ? BRT_ARC_CHUNK_CELLS : VOL_ARC_CHUNK_THR; }

static double angle_t(int x, int y, double start)
{
    double a = atan2(2.0 * y - LAST, 2.0 * x - LAST) * 180.0 / PI_D, t = a - start;
    return t - 360.0 * floor(t / 360.0);
}

/* Wrong rule for A11: numbering the low-end cells away from the end. */
static int wrong_low_thr(double w, double pitch)
{
    return ARC_OVER_CELLS - (int)floor(w / pitch);
}

static void test_a11_over_geometry(void)
{
    setup_tables();
    static unsigned char main_map[LCD_SIZE][LCD_SIZE], over_map[LCD_SIZE][LCD_SIZE];
    for (int kind = AV_VOL; kind <= AV_BRT; kind++) {
        const volarc_t *m = main_tab(kind), *o = &g_ov[kind];
        double start = kind == AV_BRT ? BRT_ARC_START_DEG : VOL_ARC_START_DEG, sweep = kind == AV_BRT ? BRT_ARC_SWEEP_DEG : VOL_SWEEP_DEG, p = pitch_of(kind);
        memset(main_map, 0, sizeof(main_map));
        memset(over_map, 0, sizeof(over_map));
        for (int i = 0; i < m->n; i++) {
            int x, y;
            volarc_xy(m->e[i], 0, &x, &y);
            main_map[y][x] = 1;
        }
        assert(o->steps == 2 * ARC_OVER_CELLS && o->cnt[2 * ARC_OVER_CELLS] == o->n && o->n > 0 && o->n <= VOLARC_OVER_CAP);
        int wrong_hits = 0;
        for (int i = 0; i < o->n; i++) {
            int x, y, thr = volarc_thr(o->e[i]);
            volarc_xy(o->e[i], 0, &x, &y);
            double t = angle_t(x, y, start);
            int d2 = d2_of(x, y);
            assert(d2 >= 4 * VOL_ARC_R_IN_PX * VOL_ARC_R_IN_PX && d2 <= 4 * VOL_ARC_R_OUT_PX * VOL_ARC_R_OUT_PX);
            assert(thr >= 1 && thr <= 2 * ARC_OVER_CELLS && !main_map[y][x] && !over_map[y][x]);
            over_map[y][x] = 1;
            assert(thr == geo_over_thr(kind, x, y, 1));
            double w = 360.0 - t;
            if (thr == geo_over_sector_thr(kind, x, y)) {
                if (thr > ARC_OVER_CELLS) {
                    double u = t - sweep;
                    assert(u >= -1e-9 && u / p >= thr - ARC_OVER_CELLS - 1 - 1e-9 && u / p <= thr - ARC_OVER_CELLS + 1e-9);
                } else {
                    assert(w > 0 && w / p >= thr - 1 - 1e-9 && w / p <= thr + 1e-9);
                }
            }
            wrong_hits += thr == 1 && t > 180.0 && geo_over_sector_thr(kind, x, y) == 1 && wrong_low_thr(w, p) != 1;
        }
        assert(wrong_hits > 0);
        for (int c = 1; c <= 2 * ARC_OVER_CELLS; c++) {
            assert(o->cnt[c] - o->cnt[c - 1] >= SEG_CELL_MIN_PX);
        }
        for (int q = 1; q < 4; q++) {
            static unsigned char map[LCD_SIZE][LCD_SIZE];
            memset(map, 0, sizeof(map));
            for (int i = 0; i < o->n; i++) {
                int x0, y0, x, y;
                volarc_xy(o->e[i], 0, &x0, &y0);
                volarc_xy(o->e[i], q, &x, &y);
                assert(!map[y][x] && d2_of(x, y) == d2_of(x0, y0));
                map[y][x] = 1;
            }
        }
    }
    /* the brightness high-end cells share pixels with the volume arc, so the old stretch must be cleared first */
    int shared = 0;
    for (int i = g_ov[AV_BRT].cnt[ARC_OVER_CELLS]; i < g_ov[AV_BRT].n; i++) {
        int x, y;
        volarc_xy(g_ov[AV_BRT].e[i], 0, &x, &y);
        for (int j = 0; j < g_arc.n && !shared; j++) {
            int x2, y2;
            volarc_xy(g_arc.e[j], 0, &x2, &y2);
            shared += x == x2 && y == y2;
        }
    }
    assert(shared > 0);
    /* seventeen cells on the volume arc would run into each other across its gap */
    assert(2 * 17 * VOL_SWEEP_DEG > (360 - VOL_SWEEP_DEG) * 100 && 2 * ARC_OVER_CELLS * VOL_SWEEP_DEG <= (360 - VOL_SWEEP_DEG) * 100);
}

static void test_a14_colour(void)
{
    for (int k = 1; k <= ARC_OVER_CELLS; k++) {
        assert(volarc_over_class(0, -k) == VOLARC_MUTE && volarc_over_class(0, k) == VOLARC_FILL);
        assert(volarc_over_class(1, -k) == VOLARC_FILL && volarc_over_class(1, k) == VOLARC_FILL);
    }
    for (int thr = 1; thr <= 2 * ARC_OVER_CELLS; thr++) {
        for (int over = -ARC_OVER_CELLS; over <= ARC_OVER_CELLS; over++) {
            int lit = volarc_over_lit(thr, over);
            assert(lit == (over < 0 ? thr <= -over : over > 0 && thr > ARC_OVER_CELLS && thr <= ARC_OVER_CELLS + over));
        }
    }
    /* a stretch that is always fill would show the volume minimum without its red mark */
    assert(volarc_over_class(0, -1) != VOLARC_FILL);
}

static void test_a15_latched_quadrant(void)
{
    arcvis_t v;
    av_init(&v);
    av_arm_kind(&v, AV_BRT, BRT_STEPS, 0);
    av_frame(&v, 0, 0);
    av_over(&v, 2);
    avact_t a = av_frame(&v, 16, 1);
    assert(a.flags == AV_OVER && a.quad == 0 && a.over_from == 0 && a.over_to == 2);
    arcvis_t wrong;
    av_init(&wrong);
    av_arm_kind(&wrong, AV_BRT, BRT_STEPS, -1);
    av_frame(&wrong, 0, 0);
    av_over(&wrong, 2);
    assert(av_frame(&wrong, 16, 1).flags == (AV_CLEAR_OLD | AV_DRAW_NEW));
}

/* One drawn meter state: kind, level and stretch; kind -1 is nothing drawn. */
typedef struct {
    int kind, val, over;
} st_t;

static st_t g_st[160];
static int g_nst;

/* full: every level of both meters; else a sample of levels around the ends, the chunk edges and the middle. */
static void build_states(bool full)
{
    static const int vv[] = {0, 1, 2, 3, 4, 5, 10, 11, 50, 89, 90, 91, 99, 100}, bv[] = {0, 1, 2, 8, 9, 10, 16, 23, 24, 31, 32};
    g_nst = 0;
    for (int kind = AV_VOL; kind <= AV_BRT; kind++) {
        int top = kind == AV_BRT ? BRT_STEPS : 100;
        int n = full ? top + 1 : kind == AV_BRT ? (int)(sizeof(bv) / sizeof(bv[0])) : (int)(sizeof(vv) / sizeof(vv[0]));
        for (int i = 0; i < n; i++) {
            int val = full ? i : kind == AV_BRT ? bv[i] : vv[i];
            int lo = val == 0 ? -ARC_OVER_CELLS : 0, hi = val == top ? ARC_OVER_CELLS : 0;
            for (int o = lo; o <= hi; o++) {
                g_st[g_nst++] = (st_t){kind, val, o};
            }
        }
    }
    g_st[g_nst++] = (st_t){-1, 0, 0};
}

static uint16_t g_buf[LCD_SIZE * LCD_SIZE], g_fresh[LCD_SIZE * LCD_SIZE];
static int g_paint = 1; /* 0 counts areas only */

typedef struct {
    int p;
    uint16_t old;
} wr_t;

static wr_t g_wr[60000];
static int g_nwr;

static uint16_t pxv(int kind, int cls) { return (uint16_t)(1 + kind * 3 + cls); }

static void put(uint16_t *buf, int x, int y, uint16_t v)
{
    if (!g_paint) {
        return;
    }
    if (buf == g_buf) {
        g_wr[g_nwr].p = y * LCD_SIZE + x;
        g_wr[g_nwr++].old = buf[y * LCD_SIZE + x];
    }
    buf[y * LCD_SIZE + x] = v;
}

static uint16_t main_value(int kind, int thr, int val)
{
    return pxv(kind, kind == AV_BRT ? volarc_seg_class(thr, val) : volarc_class(thr, val));
}

static void paint_main(uint16_t *buf, int kind, int quad, int val, int lo, int hi)
{
    const volarc_t *m = main_tab(kind);
    for (int i = m->cnt[lo]; i < m->cnt[hi]; i++) {
        int x, y;
        volarc_xy(m->e[i], quad, &x, &y);
        put(buf, x, y, main_value(kind, volarc_thr(m->e[i]), val));
    }
}

static void paint_over(uint16_t *buf, int kind, int quad, int over, int lo, int hi)
{
    const volarc_t *o = &g_ov[kind];
    for (int i = o->cnt[lo]; i < o->cnt[hi]; i++) {
        int x, y;
        volarc_xy(o->e[i], quad, &x, &y);
        put(buf, x, y, volarc_over_lit(volarc_thr(o->e[i]), over) ? pxv(kind, volarc_over_class(kind == AV_BRT, over)) : 0);
    }
}

static void blank_over(uint16_t *buf, int kind, int quad)
{
    const volarc_t *o = &g_ov[kind];
    for (int i = 0; i < o->n; i++) {
        int x, y;
        volarc_xy(o->e[i], quad, &x, &y);
        put(buf, x, y, 0);
    }
}

typedef struct {
    volarc_rect_t r[48];
    int nr, arc, parts_ok_for_ring;
} frame_t;

static void inv(frame_t *f, const volarc_rect_t *r, int n)
{
    for (int i = 0; i < n; i++) {
        f->r[f->nr++] = r[i];
    }
    f->arc += n;
    f->parts_ok_for_ring &= n <= VOLARC_RING_MAX_AREAS;
}

/* Tight areas of a whole meter by scanning the tables: one per chunk, the lit stretch inside the chunk holding the end it touches. */
static int expected_whole(int kind, int quad, int stretch, volarc_rect_t *out)
{
    const volarc_t *m = main_tab(kind), *o = &g_ov[kind];
    int n = 0, chunk = chunk_of(kind);
    for (int from = 0; from < m->steps; from += chunk) {
        int to = from + chunk < m->steps ? from + chunk : m->steps;
        volarc_rect_t r = {LCD_SIZE, LCD_SIZE, -1, -1};
        for (int i = m->cnt[from]; i < m->cnt[to]; i++) {
            int x, y;
            volarc_xy(m->e[i], quad, &x, &y);
            r.x1 = x < r.x1 ? x : r.x1;
            r.y1 = y < r.y1 ? y : r.y1;
            r.x2 = x > r.x2 ? x : r.x2;
            r.y2 = y > r.y2 ? y : r.y2;
        }
        if ((from == 0 && stretch < 0) || (to == m->steps && stretch > 0)) {
            for (int i = 0; i < o->n; i++) {
                int x, y;
                volarc_xy(o->e[i], quad, &x, &y);
                if (volarc_over_lit(volarc_thr(o->e[i]), stretch)) {
                    r.x1 = x < r.x1 ? x : r.x1;
                    r.y1 = y < r.y1 ? y : r.y1;
                    r.x2 = x > r.x2 ? x : r.x2;
                    r.y2 = y > r.y2 ? y : r.y2;
                }
            }
        }
        if (r.x2 >= r.x1) {
            out[n++] = r;
        }
    }
    return n;
}

static void expect_whole(int kind, int quad, int stretch, const volarc_rect_t *got, int n)
{
    if (!g_paint) {
        return;
    }
    volarc_rect_t want[VOLARC_MAX_AREAS];
    assert(expected_whole(kind, quad, stretch, want) == n);
    for (int i = 0; i < n; i++) {
        assert(got[i].x1 == want[i].x1 && got[i].y1 == want[i].y1 && got[i].x2 == want[i].x2 && got[i].y2 == want[i].y2);
    }
}

/* The painting and invalidation of arc_update in ui.c, into a host buffer. */
static void mirror_frame(arcvis_t *v, uint32_t now, int quad, frame_t *f)
{
    avact_t a = av_frame(v, now, quad);
    volarc_rect_t r[VOLARC_MAX_AREAS];
    g_nwr = 0;
    *f = (frame_t){.parts_ok_for_ring = 1};
    if ((a.flags & AV_CLEAR_OLD) && main_tab(a.old_kind)->n > 0) {
        const volarc_t *m = main_tab(a.old_kind);
        for (int i = 0; i < m->n; i++) {
            int x, y;
            volarc_xy(m->e[i], a.old_quad, &x, &y);
            put(g_buf, x, y, 0);
        }
        blank_over(g_buf, a.old_kind, a.old_quad);
        int n = volarc_whole_areas(m, &g_ov[a.old_kind], a.old_quad, chunk_of(a.old_kind), a.old_over, r, VOLARC_MAX_AREAS);
        expect_whole(a.old_kind, a.old_quad, a.old_over, r, n);
        inv(f, r, n);
    }
    if ((a.flags & AV_DRAW_NEW) && main_tab(a.kind)->n > 0) {
        const volarc_t *m = main_tab(a.kind);
        paint_main(g_buf, a.kind, a.quad, a.val, 0, m->steps);
        paint_over(g_buf, a.kind, a.quad, a.over_to, 0, 2 * ARC_OVER_CELLS);
        int n = volarc_whole_areas(m, &g_ov[a.kind], a.quad, chunk_of(a.kind), a.over_to, r, VOLARC_MAX_AREAS);
        expect_whole(a.kind, a.quad, a.over_to, r, n);
        inv(f, r, n);
    }
    if ((a.flags & AV_DIFF) && main_tab(a.kind)->n > 0) {
        int lo, hi;
        if (a.kind == AV_BRT) {
            volarc_seg_change_range(a.from, a.to, &lo, &hi);
        } else {
            volarc_change_range(a.from, a.to, &lo, &hi);
        }
        paint_main(g_buf, a.kind, a.quad, a.to, lo, hi);
        inv(f, r, volarc_areas_chunk(main_tab(a.kind), a.quad, lo, hi, chunk_of(a.kind), r, VOLARC_MAX_AREAS));
    }
    if (a.flags & AV_OVER) {
        int lo[2], hi[2], k = volarc_over_ranges(a.over_from, a.over_to, lo, hi);
        for (int i = 0; i < k; i++) {
            paint_over(g_buf, a.kind, a.quad, a.over_to, lo[i], hi[i]);
        }
        volarc_rect_t o[2];
        inv(f, o, volarc_over_areas(&g_ov[a.kind], a.quad, a.over_from, a.over_to, o));
    }
}

static void fresh_state(uint16_t *buf, const st_t *s, int quad)
{
    if (s->kind < 0) {
        return;
    }
    paint_main(buf, s->kind, quad, s->val, 0, main_tab(s->kind)->steps);
    paint_over(buf, s->kind, quad, s->over, 0, 2 * ARC_OVER_CELLS);
}

static void zero_support(uint16_t *buf, int quad)
{
    for (int kind = AV_VOL; kind <= AV_BRT; kind++) {
        const volarc_t *t[2] = {main_tab(kind), &g_ov[kind]};
        for (int j = 0; j < 2; j++) {
            for (int i = 0; i < t[j]->n; i++) {
                int x, y;
                volarc_xy(t[j]->e[i], quad, &x, &y);
                buf[y * LCD_SIZE + x] = 0;
            }
        }
    }
}

static void clear_buf(void)
{
    memset(g_buf, 0, sizeof(g_buf));
    memset(g_fresh, 0, sizeof(g_fresh));
}

/* Compares the support of the two quadrants of the buffer with a fresh render of the wanted state; the frame touched nothing outside it. */
static void expect_equals_fresh(const st_t *want, int quad, int other_quad)
{
    zero_support(g_fresh, quad);
    zero_support(g_fresh, other_quad);
    fresh_state(g_fresh, want, quad);
    for (int kind = AV_VOL; kind <= AV_BRT; kind++) {
        const volarc_t *t[2] = {main_tab(kind), &g_ov[kind]};
        for (int j = 0; j < 2; j++) {
            for (int i = 0; i < t[j]->n; i++) {
                int x, y;
                volarc_xy(t[j]->e[i], quad, &x, &y);
                assert(g_buf[y * LCD_SIZE + x] == g_fresh[y * LCD_SIZE + x]);
                volarc_xy(t[j]->e[i], other_quad, &x, &y);
                assert(g_buf[y * LCD_SIZE + x] == g_fresh[y * LCD_SIZE + x]);
            }
        }
    }
}

static bool in_rects(const frame_t *f, int p)
{
    int x = p % LCD_SIZE, y = p / LCD_SIZE;
    for (int i = 0; i < f->nr; i++) {
        if (x >= f->r[i].x1 && x <= f->r[i].x2 && y >= f->r[i].y1 && y <= f->r[i].y2) {
            return true;
        }
    }
    return false;
}

static int g_worst_skip, g_worst_ring, g_worst_total, g_worst_wrong, g_frames;

/* A12 area budget and A16 coverage of the frame just mirrored. */
static void check_frame(const frame_t *f)
{
    g_frames++;
    bool ring = volarc_ring_ok(f->arc, 10);
    int total = frame_total(f->arc, 10);
    if (ring) {
        g_worst_ring = f->arc > g_worst_ring ? f->arc : g_worst_ring;
    } else {
        g_worst_skip = f->arc > g_worst_skip ? f->arc : g_worst_skip;
        assert(f->arc <= 2 * VOLARC_MAX_AREAS);
    }
    assert(total <= 32);
    g_worst_total = total > g_worst_total ? total : g_worst_total;
    int wrong_total = f->arc + 10 + (f->parts_ok_for_ring ? 21 : 0);
    g_worst_wrong = wrong_total > g_worst_wrong ? wrong_total : g_worst_wrong;
    for (int i = 0; i < g_nwr; i++) {
        if (g_buf[g_wr[i].p] != g_wr[i].old) {
            assert(in_rects(f, g_wr[i].p));
        }
    }
}

static void arm_state(arcvis_t *v, const st_t *s, int quad)
{
    if (s->kind == AV_BRT) {
        av_arm_kind(v, AV_BRT, s->val, quad);
    } else {
        av_arm_kind(v, AV_VOL, s->val, -1);
    }
    av_over(v, s->over);
}

/* Moves the armed meter from state a to state b the way ui.c would: set, re-arm of the other kind, or end and linger out. */
static void go(arcvis_t *v, const st_t *a, const st_t *b, int quad, uint32_t *now)
{
    *now += 16;
    if (b->kind < 0) {
        av_end(v, a->val, *now);
        *now += VOL_ARC_LINGER_MS;
    } else if (b->kind != a->kind) {
        arm_state(v, b, quad);
    } else {
        av_set(v, b->val);
        av_over(v, b->over);
    }
}

static void run_frames(bool full)
{
    setup_tables();
    build_states(full);
    g_worst_skip = g_worst_ring = g_worst_total = g_worst_wrong = g_frames = 0;
    for (int q1 = 0; q1 < (full ? 1 : 4); q1++) {
        for (int q2 = 0; q2 < (full ? 1 : 4); q2++) {
            for (int i = 0; i < g_nst; i++) {
                if (g_st[i].kind < 0) {
                    continue;
                }
                for (int j = 0; j < g_nst; j++) {
                    if (q1 != q2 && g_st[j].kind == AV_BRT && g_st[i].kind == AV_BRT) {
                        continue;
                    }
                    clear_buf();
                    arcvis_t v;
                    av_init(&v);
                    uint32_t now = 1000;
                    frame_t f;
                    arm_state(&v, &g_st[i], q1);
                    mirror_frame(&v, now, q1, &f);
                    check_frame(&f);
                    if (g_paint) {
                        expect_equals_fresh(&g_st[i], q1, q2);
                    }
                    go(&v, &g_st[i], &g_st[j], q2, &now);
                    mirror_frame(&v, now, q2, &f);
                    check_frame(&f);
                    int qq = g_st[j].kind == AV_BRT && g_st[i].kind == AV_BRT ? q1 : q2;
                    if (g_paint) {
                        expect_equals_fresh(&g_st[j], qq, q1);
                    }
                }
            }
        }
    }
    printf("area budget (%d frames, %s): ring skipped at most %d arc areas (limit %d), ring updated at most %d (limit %d), worst total %d of 32, ring judged per part would reach %d\n",
           g_frames, full ? "count-only, every level, quadrant 0," : "painted, sampled levels, four quadrants,", g_worst_skip, 2 * VOLARC_MAX_AREAS, g_worst_ring, VOLARC_RING_MAX_AREAS, g_worst_total, g_worst_wrong);
    assert(g_worst_wrong > 32 && g_worst_skip > VOLARC_MAX_AREAS);
}

static void test_a12_a13_a16_frames(void)
{
    g_paint = 1;
    run_frames(false);
    g_paint = 0;
    run_frames(true);
    g_paint = 1;
}

/* Takeover while stretched and during the spring, both directions. */
static void test_a13_takeover_spring(void)
{
    setup_tables();
    const st_t ends[4] = {{AV_VOL, 100, ARC_OVER_CELLS}, {AV_VOL, 0, -ARC_OVER_CELLS}, {AV_BRT, BRT_STEPS, ARC_OVER_CELLS}, {AV_BRT, 0, -ARC_OVER_CELLS}};
    for (int a = 0; a < 4; a++) {
        for (int b = 0; b < 4; b++) {
            for (uint32_t gap = 0; gap <= 2 * ARC_SPRING_STEP_MS; gap += ARC_SPRING_STEP_MS) {
                for (int q = 0; q < 4; q++) {
                    clear_buf();
                    arcvis_t v;
                    av_init(&v);
                    frame_t f;
                    uint32_t now = 1000;
                    arm_state(&v, &ends[a], q);
                    mirror_frame(&v, now, q, &f);
                    check_frame(&f);
                    av_end(&v, ends[a].val, now + 16);
                    for (uint32_t e = 0; e <= gap; e += ARC_SPRING_STEP_MS) {
                        mirror_frame(&v, now + 16 + e, q, &f);
                        check_frame(&f);
                        st_t mid = ends[a];
                        mid.over = av_spring(ends[a].over, e);
                        expect_equals_fresh(&mid, q, q);
                    }
                    int q2 = (q + 1) & 3;
                    arm_state(&v, &ends[b], q2);
                    mirror_frame(&v, now + 16 + gap + 1, q2, &f);
                    check_frame(&f);
                    expect_equals_fresh(&ends[b], ends[b].kind == AV_BRT ? q2 : q2, q);
                }
            }
        }
    }
}

/* Round end caps: oracle per band pixel, from the geometry alone. */
typedef struct {
    int x, y, main_thr, cap, cell;
} orc_t;

static orc_t g_orc[2][12000];
static int g_orc_n[2];

static void orc_build(void)
{
    for (int kind = GK_VOL; kind <= GK_BRT; kind++) {
        int n = 0;
        for (int y = 0; y < LCD_SIZE; y++) {
            for (int x = 0; x < LCD_SIZE; x++) {
                if (geo_in_band(x, y)) {
                    assert(n < 12000);
                    g_orc[kind][n++] = (orc_t){x, y, geo_sector_thr(kind, x, y), geo_cap(kind, x, y), geo_over_sector_thr(kind, x, y)};
                }
            }
        }
        g_orc_n[kind] = n;
    }
}

static void rot_xy(int x, int y, int quad, int *ox, int *oy)
{
    for (int i = 0; i < quad; i++) {
        int nx = LAST - y;
        y = x;
        x = nx;
    }
    *ox = x;
    *oy = y;
}

static unsigned short g_c_got[LCD_SIZE * LCD_SIZE], g_c_want[LCD_SIZE * LCD_SIZE];

static unsigned short c_val(int kind, int cls) { return (unsigned short)(1 + kind * 3 + cls); }
static int c_main_cls(int kind, int thr, int level) { return kind == GK_BRT ? volarc_seg_class(thr, level) : volarc_class(thr, level); }
static unsigned short c_over_val(int kind, int over) { return c_val(kind, volarc_over_class(kind == GK_BRT, over)); }

/* Lit set at stretch over: the meter, the resting caps, the stretch cells up to the count and the tip cap at the count. */
static void c_render_oracle(unsigned short *buf, int kind, int quad, int level, int over)
{
    memset(buf, 0, sizeof(g_c_got));
    int n = over < 0 ? -over : over;
    for (int i = 0; i < g_orc_n[kind]; i++) {
        const orc_t *q = &g_orc[kind][i];
        int rx, ry, cend = q->cap ? geo_cap_end(q->cap) : 0, ck = q->cap ? geo_cap_k(q->cap) : 0, lit = 0, thr = q->main_thr;
        if (!thr && q->cap && ck == 0) {
            thr = cend ? geo_steps(kind) : 1;
        }
        rot_xy(q->x, q->y, quad, &rx, &ry);
        if (thr) {
            buf[ry * LCD_SIZE + rx] = c_val(kind, c_main_cls(kind, thr, level));
            continue;
        }
        if (over < 0) {
            lit = (q->cell >= 1 && q->cell <= ARC_OVER_CELLS && q->cell <= n) || (q->cap && !cend && ck == n);
        } else if (over > 0) {
            lit = (q->cell > ARC_OVER_CELLS && q->cell - ARC_OVER_CELLS <= n) || (q->cap && cend && ck == n);
        }
        if (lit) {
            buf[ry * LCD_SIZE + rx] = c_over_val(kind, over);
        }
    }
}

/* square: the pure-sector tables of the earlier rule, built from the same oracle without caps. */
static void c_render_table(unsigned short *buf, int kind, int quad, int level, int over, int square)
{
    memset(buf, 0, sizeof(g_c_got));
    if (square) {
        for (int i = 0; i < g_orc_n[kind]; i++) {
            const orc_t *q = &g_orc[kind][i];
            int rx, ry;
            rot_xy(q->x, q->y, quad, &rx, &ry);
            if (q->main_thr) {
                buf[ry * LCD_SIZE + rx] = c_val(kind, c_main_cls(kind, q->main_thr, level));
            } else if (q->cell && volarc_over_lit(q->cell, over)) {
                buf[ry * LCD_SIZE + rx] = c_over_val(kind, over);
            }
        }
        return;
    }
    const volarc_t *m = main_tab(kind), *o = &g_ov[kind];
    for (int i = 0; i < m->n; i++) {
        int x, y;
        volarc_xy(m->e[i], quad, &x, &y);
        buf[y * LCD_SIZE + x] = c_val(kind, c_main_cls(kind, volarc_thr(m->e[i]), level));
    }
    for (int i = 0; i < o->n; i++) {
        int x, y;
        volarc_xy(o->e[i], quad, &x, &y);
        if (volarc_over_lit(volarc_thr(o->e[i]), over)) {
            buf[y * LCD_SIZE + x] = c_over_val(kind, over);
        }
    }
}

/* Returns the index of the first differing pixel, or -1. */
static int c_first_diff(const unsigned short *a, const unsigned short *b)
{
    for (int i = 0; i < LCD_SIZE * LCD_SIZE; i++) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return -1;
}

static int c1_run(int square)
{
    int bad = 0;
    for (int kind = GK_VOL; kind <= GK_BRT; kind++) {
        int top = kind == GK_BRT ? BRT_STEPS : 100;
        for (int quad = 0; quad < 4; quad++) {
            for (int level = 0; level <= top; level++) {
                for (int over = level == 0 ? -ARC_OVER_CELLS : 0; over <= (level == top ? ARC_OVER_CELLS : 0); over++) {
                    c_render_table(g_c_got, kind, quad, level, over, square);
                    c_render_oracle(g_c_want, kind, quad, level, over);
                    int d = c_first_diff(g_c_got, g_c_want);
                    if (d >= 0) {
                        if (!square) {
                            printf("C1 mismatch kind %d quad %d level %d stretch %d at x=%d y=%d: table %d oracle %d\n", kind, quad, level, over, d % LCD_SIZE, d / LCD_SIZE, g_c_got[d], g_c_want[d]);
                        }
                        bad++;
                    }
                }
            }
        }
    }
    return bad;
}

static void test_c1_oracle(void)
{
    setup_tables();
    orc_build();
    assert(c1_run(0) == 0);
    /* the square-cut tables must be rejected */
    assert(c1_run(1) > 0);
}

static void test_c2_caps_inside_cells(void)
{
    for (int kind = GK_VOL; kind <= GK_BRT; kind++) {
        for (int high = 0; high < 2; high++) {
            for (int k = 0; k <= ARC_OVER_CELLS; k++) {
                int cnt = 0;
                for (int i = 0; i < g_orc_n[kind]; i++) {
                    const orc_t *q = &g_orc[kind][i];
                    if (!q->cap || geo_cap_end(q->cap) != high || geo_cap_k(q->cap) != k) {
                        continue;
                    }
                    cnt++;
                    assert(geo_in_band(q->x, q->y) && q->main_thr == 0);
                    if (k < ARC_OVER_CELLS) {
                        assert(q->cell == (high ? ARC_OVER_CELLS : 0) + k + 1);
                    }
                }
                printf("cap kind %d %s k=%d: %d px\n", kind, high ? "high" : "low", k, cnt);
                assert(cnt > 0);
            }
        }
    }
}

static void test_c3_no_shared_pixel(void)
{
    static unsigned char map[LCD_SIZE][LCD_SIZE];
    for (int kind = GK_VOL; kind <= GK_BRT; kind++) {
        memset(map, 0, sizeof(map));
        const volarc_t *m = main_tab(kind), *o = &g_ov[kind];
        for (int i = 0; i < m->n; i++) {
            int x, y;
            volarc_xy(m->e[i], 0, &x, &y);
            map[y][x] = 1;
        }
        for (int i = 0; i < o->n; i++) {
            int x, y;
            volarc_xy(o->e[i], 0, &x, &y);
            assert(!map[y][x]);
        }
        printf("tables kind %d: main %d/%d over %d/%d\n", kind, m->n, VOLARC_CAP, o->n, VOLARC_OVER_CAP);
        assert(m->n <= VOLARC_CAP && o->n <= VOLARC_OVER_CAP);
    }
}

/* Springing back from any stretch to any other at the same end repaints only the changed range and still equals a fresh render. */
static void test_c4_spring_back(void)
{
    for (int kind = GK_VOL; kind <= GK_BRT; kind++) {
        int top = kind == GK_BRT ? BRT_STEPS : 100;
        for (int end = 0; end < 2; end++) {
            int level = end ? top : 0, sign = end ? 1 : -1;
            for (int from = 0; from <= ARC_OVER_CELLS; from++) {
                for (int to = 0; to <= ARC_OVER_CELLS; to++) {
                    int quad = (from + to) & 3, f = sign * from, t = sign * to;
                    c_render_table(g_c_got, kind, quad, level, f, 0);
                    int lo[2], hi[2], n = volarc_over_ranges(f, t, lo, hi);
                    const volarc_t *o = &g_ov[kind];
                    for (int j = 0; j < n; j++) {
                        for (int i = o->cnt[lo[j]]; i < o->cnt[hi[j]]; i++) {
                            int x, y;
                            volarc_xy(o->e[i], quad, &x, &y);
                            g_c_got[y * LCD_SIZE + x] = volarc_over_lit(volarc_thr(o->e[i]), t) ? c_over_val(kind, t) : 0;
                        }
                    }
                    c_render_table(g_c_want, kind, quad, level, t, 0);
                    int d = c_first_diff(g_c_got, g_c_want);
                    if (d >= 0) {
                        printf("C4 kind %d end %d %d->%d at x=%d y=%d\n", kind, end, f, t, d % LCD_SIZE, d / LCD_SIZE);
                    }
                    assert(d < 0);
                    if (to == 0) {
                        int caps = 0;
                        for (int i = 0; i < g_orc_n[kind]; i++) {
                            const orc_t *q = &g_orc[kind][i];
                            int rx, ry;
                            if (q->cap && geo_cap_k(q->cap) == 0 && geo_cap_end(q->cap) == end) {
                                rot_xy(q->x, q->y, quad, &rx, &ry);
                                assert(g_c_got[ry * LCD_SIZE + rx] == c_val(kind, c_main_cls(kind, end ? geo_steps(kind) : 1, level)));
                                caps++;
                            }
                        }
                        assert(caps > 0);
                    }
                }
            }
        }
    }
}

static void test_c5_tip_keeps_colour(void)
{
    for (int kind = GK_VOL; kind <= GK_BRT; kind++) {
        int top = kind == GK_BRT ? BRT_STEPS : 100;
        for (int end = 0; end < 2; end++) {
            int level = end ? top : 0, thr = end ? geo_steps(kind) : 1;
            for (int n = 1; n <= ARC_OVER_CELLS; n++) {
                assert(c_main_cls(kind, thr, level) == volarc_over_class(kind == GK_BRT, end ? n : -n));
            }
        }
    }
}

/* Cost guard: the device builds four tables at boot without a double FPU, so a build may spend one atan2 per band pixel per pass and one cos/sin pair per cap centre. */
static long band_px(void)
{
    long n = 0;
    for (int y = 0; y < LCD_SIZE; y++) {
        for (int x = 0; x < LCD_SIZE; x++) {
            n += geo_in_band(x, y);
        }
    }
    return n;
}

/* Wrong rule: the earlier per-pixel cap test, one atan2 and one cos/sin pair per cap candidate for every band pixel of the stretch pass. */
static void wrong_per_pixel_caps(long *atan2_calls, long *cossin_calls)
{
    *atan2_calls = *cossin_calls = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < LCD_SIZE; y++) {
            for (int x = 0; x < LCD_SIZE; x++) {
                if (geo_in_band(x, y)) {
                    *atan2_calls += 1 + 2 * (ARC_OVER_CELLS + 1);
                    *cossin_calls += 2 * (ARC_OVER_CELLS + 1);
                }
            }
        }
    }
}

static void test_c6_build_cost(void)
{
    static uint32_t st[VOLARC_CAP];
    volarc_t t;
    long limit_atan = 2 * band_px(), limit_cossin = 2 * (ARC_OVER_CELLS + 1);
    for (int kind = GK_VOL; kind <= GK_BRT; kind++) {
        for (int over = 0; over < 2; over++) {
            volarc_trig = (volarc_trig_t){0, 0};
            double sw = geo_sweep(kind), start = geo_start(kind);
            int n = over ? volarc_build_over(&t, st, VOLARC_CAP, start, sw, geo_steps(kind)) : volarc_build_seg(&t, st, VOLARC_CAP, start, sw, geo_steps(kind));
            printf("C6 kind %d %s: %d px, atan2 %ld (limit %ld), cos/sin pairs %ld (limit %ld)\n", kind, over ? "over" : "main", n, volarc_trig.atan2_calls, limit_atan, volarc_trig.cossin_calls, limit_cossin);
            assert(n > 0 && volarc_trig.atan2_calls <= limit_atan && volarc_trig.cossin_calls <= limit_cossin);
        }
    }
    long wa, wc;
    wrong_per_pixel_caps(&wa, &wc);
    assert(wa > limit_atan && wc > limit_cossin);
}

int main(void)
{
    test_a1_band_and_order();
    test_a2_scene_clear_of_arc();
    test_a2s_sticker_scene_clear_of_arc();
    test_a2r_listening_clear_of_arc();
    test_ring_band_budget();
    test_area_budget();
    test_class();
    test_linger();
    test_rearm_takes_over();
    test_cancel_shows_restored_value();
    test_quadrant_and_force();
    test_a4_segment_geometry();
    test_a5_minimum_is_visible();
    test_a6_segment_budget();
    test_volume_multi_step_budget();
    test_a7_one_meter();
    test_a8_latched_quadrant();
    test_a9_spring();
    test_a10_no_growth();
    test_a11_over_geometry();
    test_a12_a13_a16_frames();
    test_a13_takeover_spring();
    test_a14_colour();
    test_a15_latched_quadrant();
    test_c1_oracle();
    test_c2_caps_inside_cells();
    test_c3_no_shared_pixel();
    test_c4_spring_back();
    test_c5_tip_keeps_colour();
    test_c6_build_cost();
    puts("ok");
    return 0;
}
