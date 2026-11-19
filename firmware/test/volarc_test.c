#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arcvis.h"
#include "dpower.h"
#include "volarc.h"

#define PI_D 3.14159265358979323846
#define LAST (LCD_SIZE - 1)

static uint32_t g_store[VOLARC_CAP];
static volarc_t g_arc;

static int d2_of(int x, int y) { int dx = 2 * x - LAST, dy = 2 * y - LAST; return dx * dx + dy * dy; }

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
        assert(t >= prev && t >= 1 && t <= 100 && !seen[y][x] && !ui_gap(x, y));
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
                assert(!seen[y][x]);
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

/* Corner radius of the one bounding rectangle ui.c clears and rotates, per state and orbit position, with its 1 px margin. */
static double worst_corner(void)
{
    static const scene_case_t CASES[] = {
        {0, 0, -4 * SCUTTLE_DISTANCE_UNITS, 4 * SCUTTLE_DISTANCE_UNITS, 0, 4},  /* IDLE */
        {34, 0, 0, 0, 0, 0},                                                      /* LISTENING */
        {33, 1, 0, 0, 0, 0},                                                      /* THINKING */
        {34, 0, 0, 0, -2 * BUBBLE_UNIT_PX, 0},                                    /* SPEAKING */
        {36, 0, -BUBBLE_UNIT_PX, BUBBLE_UNIT_PX, 0, 0},                           /* ERROR */
        {24, 0, 0, 0, 0, BUBBLE_UNIT_PX},                                         /* OFFLINE */
    };
    const int u = BUBBLE_UNIT_PX, c = MASCOT_CELL_PX, half = SCENE_SIZE_PX / 2;
    double worst = 0;
    for (size_t k = 0; k < sizeof(CASES) / sizeof(CASES[0]); k++) {
        const scene_case_t *s = &CASES[k];
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
        assert(a >= BRT_ARC_START_DEG && a < BRT_ARC_START_DEG + BRT_ARC_SWEEP_DEG && t >= prev && t >= 1 && t <= BRT_STEPS + 1 && !seen[y][x]);
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

/* Areas of one frame as ui.c spends them: the arc, the scene's 10, and the ring's 21 only when the arc has at most VOLARC_RING_MAX_AREAS. */
static int frame_total(int arc)
{
    return arc + 10 + (arc <= VOLARC_RING_MAX_AREAS ? 21 : 0);
}

static void test_a6_segment_budget(void)
{
    volarc_build(&g_arc, g_store, VOLARC_CAP);
    seg_build(&g_seg, g_seg_store, BRT_STEPS + 1);
    int px, step_px = 0, worst = 0;
    int whole = seg_areas(&g_seg, 0, 0, BRT_STEPS + 1, BRT_ARC_CHUNK_CELLS, &px);
    printf("brightness arc: whole %d areas %d px", whole, px);
    assert(whole <= 4 && frame_total(whole) <= 32);
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
                int t = frame_total(seg_areas(&g_seg, q, lo, hi, BRT_ARC_CHUNK_CELLS, &px));
                worst = t > worst ? t : worst;
            }
        }
    }
    int vol_whole = frame_areas(0, 0, 100, &px);
    int takeover = vol_whole + whole;
    printf("; 1 step 1 area, at most %d px; any step at most %d of 32; takeover %d (+10 scene = %d of 32)\n", step_px, worst, takeover, takeover + 10);
    assert(worst <= 32 && frame_total(takeover) <= 32 && takeover <= 22);
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
                int t = frame_total(frame_areas(q, lo, hi, &px));
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

static void test_a9_blink(void)
{
    const uint32_t t = 0xFFFFFF80u;
    arcvis_t v;
    av_init(&v);
    av_arm_kind(&v, AV_BRT, BRT_STEPS, 0);
    av_frame(&v, t, 0);
    av_blink(&v, t + 16);
    avact_t a = av_frame(&v, t + 16, 0);
    assert(a.flags == AV_CLEAR_OLD && a.old_kind == AV_BRT);
    for (uint32_t d = 32; d < 16 + BRT_END_BLINK_MS; d += 16) {
        assert(av_frame(&v, t + d, 0).flags == 0);
    }
    a = av_frame(&v, t + 16 + BRT_END_BLINK_MS, 0);
    assert(a.flags == AV_DRAW_NEW && a.kind == AV_BRT && a.val == BRT_STEPS);

    av_blink(&v, t + 400);
    assert(av_frame(&v, t + 400, 0).flags == AV_CLEAR_OLD);
    av_end(&v, BRT_STEPS, t + 450);
    assert(av_frame(&v, t + 450, 0).flags == 0);
    assert(av_frame(&v, t + 400 + BRT_END_BLINK_MS, 0).flags == AV_DRAW_NEW);
    for (uint32_t d = 400 + BRT_END_BLINK_MS + 16; d < 450 + VOL_ARC_LINGER_MS; d += 16) {
        assert(av_visible(&v, t + d) && av_frame(&v, t + d, 0).flags == 0);
    }
    assert(av_frame(&v, t + 450 + VOL_ARC_LINGER_MS, 0).flags == AV_CLEAR_OLD);

    av_arm_kind(&v, AV_BRT, 0, 0);
    av_frame(&v, t + 2000, 0);
    av_blink(&v, t + 2016);
    av_frame(&v, t + 2016, 0);
    av_arm_kind(&v, AV_BRT, 0, 0);
    assert(av_frame(&v, t + 2032, 0).flags == AV_DRAW_NEW);
}

int main(void)
{
    test_a1_band_and_order();
    test_a2_scene_clear_of_arc();
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
    test_a9_blink();
    puts("ok");
    return 0;
}
