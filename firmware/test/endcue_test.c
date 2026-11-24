#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arcvis.h"
#include "bright.h"
#include "volknob.h"
#include "volume.h"

#define PI_D 3.14159265358979323846
#define CTR 232.5
#define PITCH ((double)BRT_TRAVEL_PX / BRT_STEPS)
#define N ARC_OVER_CELLS

static const int QUADS[4] = {0, 90, 180, 270};

/* Wrong cell rules for E1: the divisor d = N, the floor of the closed form, and a linear ramp. */
static int wrong_cells_d_is_n(double x)
{
    int n = 0;
    for (int j = 1; j <= N; j++) {
        n += x * (N - j) >= (double)j * N;
    }
    return n;
}

static int wrong_cells_floor_form(double x)
{
    const double d = N + 1;
    return x <= 0 ? 0 : (int)floor(d * (1.0 - 1.0 / (x / d + 1.0)));
}

static int wrong_cells_linear(double x)
{
    return x <= 0 ? 0 : x > N ? N : (int)floor(x);
}

static void check_e1(int (*f)(double))
{
    const double lo[6] = {1.2, 1.5, 3.9, 4.1, 11.9, 12.1};
    const int want[6] = {0, 1, 1, 2, 2, 3};
    for (int i = 0; i < 6; i++) {
        assert(f(lo[i]) == want[i]);
    }
    assert(f(INFINITY) == N && f(1e300) == N && f(NAN) == 0 && f(-1) == 0 && f(0) == 0 && f(-INFINITY) == 0);
    int prev = 0;
    for (double x = -5; x <= 400; x += 0.25) {
        int c = f(x);
        assert(c >= prev && c >= 0 && c <= N);
        prev = c;
    }
    int gap1 = 0, gap2 = 0;
    double t1 = 0, t2 = 0, t3 = 0;
    for (double x = 0; x < 20 && !t3; x += 0.001) {
        int c = f(x);
        t1 = !t1 && c >= 1 ? x : t1;
        t2 = !t2 && c >= 2 ? x : t2;
        t3 = !t3 && c >= 3 ? x : t3;
    }
    gap1 = t2 - t1 > 0.5;
    gap2 = t3 - t2 > t2 - t1;
    assert(gap1 && gap2);
}

static void test_e1_bounds_monotone(void)
{
    check_e1(vk_over_cells);
    assert(wrong_cells_d_is_n(1e300) < N);
    assert(wrong_cells_floor_form(1e300) > N);
    assert(wrong_cells_linear(11.9) == N);
}

/* A brightness touch fed in the UI frame of angle deg; fu is the travel along UI-up from the press point. */
typedef struct {
    volknob_t k;
    uint32_t t;
    int ev, x0, y0, bi0, quad;
    double ux, uy, fu;
} sw_t;

static void sw_press(sw_t *s, int deg, int bi0)
{
    *s = (sw_t){.t = 1000, .x0 = 233, .y0 = 233, .bi0 = bi0, .quad = deg};
    double a = deg * PI_D / 180.0;
    s->ux = round(sin(a));
    s->uy = -round(cos(a));
    vk_press(&s->k, s->x0, s->y0, s->t, false, 50);
    vk_swipe_frame(&s->k, (float)deg);
}

static int sw_at(sw_t *s, double fu, uint32_t dt)
{
    s->fu = fu;
    s->t += dt;
    int x = s->x0 + (int)lround(fu * s->ux), y = s->y0 + (int)lround(fu * s->uy);
    int ev = vk_move(&s->k, x, y, s->t);
    if (ev & VK_EV_BRT_ARM) {
        vk_swipe_rebase(&s->k, s->bi0);
    }
    s->ev |= ev;
    return ev;
}

/* Moves by du in samples of at most one pixel, 8 ms apart; returns the OR of the events. */
static int sw_px(sw_t *s, double du)
{
    int ev = 0, n = (int)ceil(fabs(du));
    double u0 = s->fu;
    for (int i = 1; i <= n; i++) {
        ev |= sw_at(s, u0 + du * i / n, 8);
    }
    return ev;
}

/* Arms the swipe at the end of the ladder: sign +1 the top, -1 the bottom. */
static void sw_armed_at_end(sw_t *s, int deg, int sign)
{
    sw_press(s, deg, sign > 0 ? BRT_STEPS : 0);
    sw_px(s, sign * BRT_ARM_PX);
    assert(s->k.state == VK_BRIGHT && s->k.bi == s->bi0 && s->k.over == 0);
}

/* Wrong for E2: thresholds in rim pixels of the arc cell pitch instead of level steps. */
static int wrong_cells_rim_px(double steps)
{
    return vk_over_cells(steps * PITCH / 10.76);
}

static void test_e2_brightness_push(void)
{
    const double px[7] = {9, 11, 29, 31, 89, 91, 185};
    const int want[7] = {0, 1, 1, 2, 2, 3, 3};
    for (int q = 0; q < 4; q++) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            sw_t s;
            sw_armed_at_end(&s, QUADS[q], sign);
            double base = s.fu;
            for (int i = 0; i < 7; i++) {
                int ev = sw_px(&s, base + sign * px[i] - s.fu);
                assert(s.k.over == sign * want[i]);
                assert(!(ev & (VK_EV_BRT | VK_EV_BRT_CANCEL)));
            }
            assert(s.k.bi == (sign > 0 ? BRT_STEPS : 0));
        }
    }
    assert(wrong_cells_rim_px(11 / PITCH) == 0 && vk_over_cells(11 / PITCH) == 1);
}

static void test_e2_events_only_on_change(void)
{
    sw_t s;
    sw_armed_at_end(&s, 0, 1);
    int changes = 0, last = 0;
    for (int i = 0; i < 200; i++) {
        int ev = sw_px(&s, 1);
        assert(((ev & VK_EV_OVER) != 0) == (s.k.over != last));
        changes += (ev & VK_EV_OVER) != 0;
        last = s.k.over;
    }
    assert(changes == N);
}

typedef struct {
    volknob_t k;
    uint32_t t;
    int ev;
    double deg, r, arm_travel;
} dg_t;

static int dg_px(double r, double deg)
{
    return (int)lround(CTR + r * cos(deg * PI_D / 180.0));
}

static int dg_py(double r, double deg)
{
    return (int)lround(CTR + r * sin(deg * PI_D / 180.0));
}

static void dg_start(dg_t *d, double r, double deg, int vol)
{
    *d = (dg_t){.t = 1000, .r = r, .deg = deg};
    vk_press(&d->k, dg_px(r, deg), dg_py(r, deg), d->t, false, (uint8_t)vol);
}

static int dg_turn(dg_t *d, double delta, double step)
{
    int n = (int)ceil(fabs(delta) / step), ev_all = 0;
    for (int i = 0; i < n; i++) {
        d->deg += delta / n;
        d->t += 16;
        int ev = vk_move(&d->k, dg_px(d->r, d->deg), dg_py(d->r, d->deg), d->t);
        if (ev & VK_EV_ARM) {
            d->arm_travel = d->k.travel;
        }
        d->ev |= ev;
        ev_all |= ev;
    }
    return ev_all;
}

static void test_e3_knob_push(void)
{
    const double radii[3] = {160, 190, 225};
    for (int ri = 0; ri < 3; ri++) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            dg_t d;
            dg_start(&d, radii[ri], 10, sign > 0 ? 100 : 0);
            for (int i = 0; i < 40 && d.k.state != VK_ARMED; i++) {
                dg_turn(&d, sign, 1);
            }
            assert(d.k.state == VK_ARMED);
            int hit[N + 1] = {0};
            for (int i = 0; i < 80; i++) {
                dg_turn(&d, sign, 1);
                double p = fabs(d.k.travel - d.arm_travel);
                int c = abs(d.k.over);
                assert(d.k.over == 0 || (d.k.over > 0) == (sign > 0));
                hit[c]++;
                if (p <= 2.1) {
                    assert(c == 0);
                } else if (p >= 5.1 && p <= 9.3) {
                    assert(c == 1);
                } else if (p >= 12.3 && p <= 30.9) {
                    assert(c == 2);
                } else if (p >= 33.9) {
                    assert(c == N);
                }
                assert(fabs(d.k.opush * VOL_SWEEP_DEG / 100.0 - p) < 1.5);
            }
            for (int c = 0; c <= N; c++) {
                assert(hit[c] > 0);
            }
        }
    }
    /* counting from the press instead of the arming sample shows cells while the knob arms */
    dg_t d;
    dg_start(&d, 190, 10, 100);
    dg_turn(&d, 16, 1);
    assert(d.k.state == VK_ARMED && d.k.over == 0 && vk_over_cells(16 / (VOL_SWEEP_DEG / 100.0)) > 0);
}

/* Reference copy of the brightness ladder before the end cue: level, STOP and BRT bits only. */
typedef struct {
    double up, up_last, bacc, push;
    int bi, dir;
    bool bumped;
} refb_t;

static int ref_brt_step(refb_t *k)
{
    const double pitch = PITCH;
    double d = BRT_UP_SIGN * (k->up - k->up_last);
    k->up_last = k->up;
    int dir = d > 0 ? 1 : d < 0 ? -1 : 0;
    k->dir = dir != 0 ? dir : k->dir;
    k->bacc += d;
    int n = (int)(fabs(k->bacc) / pitch);
    n = k->bacc < 0 ? -n : n;
    k->bacc -= n * pitch;
    int nb = k->bi + n;
    if (nb > BRT_STEPS || (nb == BRT_STEPS && k->bacc > 0)) {
        k->push += nb - BRT_STEPS + k->bacc / pitch;
        nb = BRT_STEPS;
        k->bacc = 0;
    } else if (nb < 0 || (nb == 0 && k->bacc < 0)) {
        k->push += -nb - k->bacc / pitch;
        nb = 0;
        k->bacc = 0;
    } else if (dir != 0) {
        k->push = 0;
    }
    if (nb != 0 && nb != BRT_STEPS) {
        k->bumped = false;
    }
    int ev = nb != k->bi ? VK_EV_BRT : 0;
    k->bi = nb;
    if (k->push >= 1.0 && !k->bumped) {
        k->bumped = true;
        ev |= VK_EV_BRT_STOP;
    }
    return ev;
}

/* Reference copy of the knob accumulator before the end cue. */
typedef struct {
    double acc, theta_last;
    int v;
} refk_t;

static double ref_angle(int x, int y)
{
    return atan2(y - CTR, x - CTR) * 180.0 / PI_D;
}

static double ref_wrap(double d)
{
    return fmod(fmod(d + 540.0, 360.0) + 360.0, 360.0) - 180.0;
}

static int ref_knob_step(refk_t *k, int x, int y)
{
    double th = ref_angle(x, y), d = ref_wrap(th - k->theta_last);
    k->theta_last = th;
    k->acc += VOL_CW_SIGN * d * 100.0 / VOL_SWEEP_DEG;
    int n = (int)k->acc, nv = k->v + n < 0 ? 0 : k->v + n > 100 ? 100 : k->v + n;
    k->acc -= n;
    if (nv >= 100 && k->acc > 0) {
        k->acc = 0;
    }
    if (nv <= 0 && k->acc < 0) {
        k->acc = 0;
    }
    int ev = nv != k->v ? VK_EV_VOL : 0;
    k->v = nv;
    return ev;
}

static void test_e4_outcomes_unchanged(void)
{
    srand(73);
    int over_events = 0;
    for (int run = 0; run < 2000; run++) {
        int quad = QUADS[run & 3];
        if (run & 1) {
            int v0 = rand() % 101;
            double deg = rand() % 360;
            dg_t d;
            dg_start(&d, 190, deg, v0);
            dg_turn(&d, (rand() & 1 ? 1 : -1) * 20, 1);
            if (d.k.state != VK_ARMED) {
                continue;
            }
            refk_t ref = {.v = d.k.v, .acc = d.k.acc, .theta_last = ref_angle(dg_px(d.r, d.deg), dg_py(d.r, d.deg))};
            int sgn = rand() & 1 ? 1 : -1;
            for (int i = 0; i < 300; i++) {
                if (rand() % 40 == 0) {
                    sgn = -sgn;
                }
                d.deg += sgn * (rand() % 600) / 100.0;
                d.t += 16;
                int x = dg_px(d.r, d.deg), y = dg_py(d.r, d.deg);
                int ev = vk_move(&d.k, x, y, d.t);
                if (ev & VK_EV_CANCEL) {
                    break;
                }
                int want = ref_knob_step(&ref, x, y);
                assert((ev & ~VK_EV_OVER) == want && d.k.v == ref.v);
                over_events += (ev & VK_EV_OVER) != 0;
            }
        } else {
            int bi0 = rand() % 3 == 0 ? 0 : rand() % 3 == 1 ? BRT_STEPS : rand() % (BRT_STEPS + 1);
            sw_t s;
            sw_press(&s, quad, bi0);
            int sgn = bi0 == BRT_STEPS ? 1 : -1;
            sw_px(&s, sgn * BRT_ARM_PX);
            if (s.k.state != VK_BRIGHT) {
                continue;
            }
            refb_t ref = {.up = s.k.up, .up_last = s.k.up, .bi = bi0};
            for (int i = 0; i < 300; i++) {
                if (rand() % 25 == 0) {
                    sgn = -sgn;
                }
                double fu = s.fu + sgn * (rand() % 2000) / 100.0;
                s.t += 8;
                s.fu = fu;
                int x = s.x0 + (int)lround(fu * s.ux), y = s.y0 + (int)lround(fu * s.uy);
                int ev = vk_move(&s.k, x, y, s.t);
                if (ev & VK_EV_BRT_CANCEL) {
                    break;
                }
                ref.up = s.k.up;
                int want = ref_brt_step(&ref);
                assert((ev & ~VK_EV_OVER) == want && s.k.bi == ref.bi);
                over_events += (ev & VK_EV_OVER) != 0;
            }
        }
    }
    assert(over_events > 100);
    /* the retained part of the push kept in the accumulator would make -3 degrees leave 100 */
    dg_t d;
    dg_start(&d, 190, 0, 70);
    dg_turn(&d, 1080, 1);
    assert(d.k.v == 100 && d.k.over == N);
    d.ev = 0;
    dg_turn(&d, -2, 1);
    assert(d.k.v == 100 && d.k.over == N && d.ev == 0);
    dg_turn(&d, -1, 1);
    assert(d.k.v == 99 && d.k.over == 0 && (d.ev & VK_EV_OVER) && (d.ev & VK_EV_VOL));
}

static void test_e5_hold_with_noise(void)
{
    sw_t s;
    sw_armed_at_end(&s, 0, 1);
    sw_px(&s, 100);
    assert(s.k.over == N);
    int ev = 0;
    for (int i = 0; i < 1000; i++) {
        s.t += 8;
        s.fu += (i & 1) ? 1 : -1;
        ev |= vk_move(&s.k, s.x0, s.y0 - (int)lround(s.fu), s.t);
        assert(s.k.over == N);
        /* a push that any reversed sample resets would show nothing on the first one */
        if (i == 0) {
            assert(s.k.push == 0 && vk_over_cells(s.k.push) == 0);
        }
    }
    assert(ev == 0);
    dg_t d;
    dg_start(&d, 190, 0, 70);
    dg_turn(&d, 400, 1);
    assert(d.k.over == N);
    d.ev = 0;
    for (int i = 0; i < 1000; i++) {
        dg_turn(&d, (i & 1) ? 2 : -2, 2);
        assert(d.k.over == N && d.k.v == 100);
    }
    assert(d.ev == 0);
}

static void test_e6_first_reversed_step(void)
{
    sw_t s;
    sw_armed_at_end(&s, 90, 1);
    sw_px(&s, 91);
    assert(s.k.over == N);
    double peak = s.k.opush;
    for (int i = 1; i <= 7; i++) {
        int ev = sw_px(&s, -1);
        assert(ev == 0 && s.k.over == N);
        if (i == 2) {
            /* unwinding the push gradually would already show a lower count */
            assert(vk_over_cells(peak - 2 / PITCH) == N - 1);
        }
    }
    int ev = sw_px(&s, -1);
    assert((ev & VK_EV_BRT) && (ev & VK_EV_OVER) && s.k.bi == BRT_STEPS - 1 && s.k.over == 0);
}

/* The glue's reaction to a cancel at the low end: the stretch event, then the end. */
static int e7_next_frame_over(bool apply_over_event)
{
    arcvis_t v;
    av_init(&v);
    av_arm_kind(&v, AV_BRT, 0, 0);
    av_over(&v, -N);
    av_frame(&v, 1000, 0);
    if (apply_over_event) {
        av_over(&v, 0);
    }
    av_end(&v, 0, 1016);
    avact_t a = av_frame(&v, 1016, 0);
    return (a.flags & AV_OVER) ? a.over_to : -N;
}

static void test_e7_cancel(void)
{
    assert(e7_next_frame_over(true) == 0);
    /* a cancel that keeps the stretch springs through a smaller one */
    assert(e7_next_frame_over(false) == -(N - 1));
    sw_t s;
    sw_armed_at_end(&s, 180, -1);
    sw_px(&s, -100);
    assert(s.k.over == -N);
    int ev = sw_at(&s, s.fu - 150, 16);
    assert((ev & VK_EV_BRT_CANCEL) && (ev & VK_EV_OVER) && s.k.over == 0 && s.k.bi == s.bi0);
    dg_t d;
    dg_start(&d, 190, 0, 100);
    dg_turn(&d, 60, 1);
    assert(d.k.over == N);
    int ev2 = dg_turn(&d, 60, 60);
    assert((ev2 & VK_EV_CANCEL) && (ev2 & VK_EV_OVER) && d.k.over == 0 && d.k.v == 100);
}

static void test_e8_release(void)
{
    sw_t s;
    sw_armed_at_end(&s, 0, 1);
    sw_px(&s, 100);
    assert(s.k.over == N);
    assert(vk_release(&s.k, s.t + 8) == VK_EV_BRT_COMMIT && s.k.over == N);
    sw_armed_at_end(&s, 270, -1);
    sw_px(&s, -100);
    assert(vk_lost(&s.k) == VK_EV_BRT_COMMIT && s.k.over == -N);
    dg_t d;
    dg_start(&d, 190, 0, 100);
    dg_turn(&d, 60, 1);
    assert(d.k.over == N && vk_release(&d.k, d.t + 16) == VK_EV_COMMIT && d.k.over == N);
    dg_start(&d, 190, 0, 0);
    dg_turn(&d, -60, 1);
    assert(d.k.over == -N && vk_lost(&d.k) == VK_EV_COMMIT && d.k.over == -N);
}

static void test_e9_both_ends_in_one_sample(void)
{
    sw_t s;
    sw_armed_at_end(&s, 0, -1);
    sw_px(&s, -100);
    assert(s.k.over == -N && s.k.bi == 0);
    double carried = s.k.opush;
    s.t += 50;
    s.fu += 255;
    int ev = vk_move(&s.k, s.x0, s.y0 - (int)lround(s.fu), s.t);
    assert((ev & VK_EV_BRT) && (ev & VK_EV_OVER) && s.k.bi == BRT_STEPS && s.k.over == 1 && fabs(s.k.opush - 2.0) < 1e-9);
    /* a push carried across the level change would show the full count at the far end */
    assert(vk_over_cells(carried + 2.0) == N);
}

/* Wrong rule for E10: the ov counters printed without the cap. */
static int wrong_uncapped_bright(char *out, size_t cap, const uint32_t *ov)
{
    return snprintf(out, cap, BRIGHT_LINE_FMT, 100, 100, 100, 1, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, ov[0], ov[1], ov[2]);
}

static void test_e10_line_width(void)
{
    /* Widest ESP_LOGI prefix and suffix: colour codes, a 10-digit timestamp and the app tag. */
    const size_t frame = strlen("\033[0;32mI (4294967295) app: ") + strlen("\033[0m");
    const uint32_t big[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    char line[512];
    int n = snprintf(line, sizeof(line), BRIGHT_LINE_FMT, 100, 100, 100, 1, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, vk_ov_log(big[0]),
                     vk_ov_log(big[1]), vk_ov_log(big[2]));
    printf("widest bright line %d + log frame %zu = %zu of %d\n", n, frame, n + frame, LOG_LINE_MAX_BYTES - 1);
    assert(n + frame <= LOG_LINE_MAX_BYTES - 1 && strstr(line, " ov=999999/999999/999999"));
    assert(wrong_uncapped_bright(line, sizeof(line), big) + frame > LOG_LINE_MAX_BYTES - 1);
    n = snprintf(line, sizeof(line), VOLUME_LINE_FMT, 100, 100, 1, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, vk_ov_log(big[0]), vk_ov_log(big[1]), vk_ov_log(big[2]));
    assert(n + frame <= LOG_LINE_MAX_BYTES - 1);
    assert(vk_ov_log(ARC_OV_LOG_MAX) == ARC_OV_LOG_MAX && vk_ov_log(ARC_OV_LOG_MAX + 1) == ARC_OV_LOG_MAX && vk_ov_log(7) == 7);
}

typedef struct {
    const char *name;
    void (*fn)(void);
} case_t;

/* With an argument, runs only the named rules (used to show a wrong variant red). */
int main(int argc, char **argv)
{
    static const case_t CASES[] = {
        {"e1", test_e1_bounds_monotone}, {"e2", test_e2_brightness_push}, {"e2b", test_e2_events_only_on_change}, {"e3", test_e3_knob_push},
        {"e4", test_e4_outcomes_unchanged}, {"e5", test_e5_hold_with_noise}, {"e6", test_e6_first_reversed_step}, {"e7", test_e7_cancel},
        {"e8", test_e8_release}, {"e9", test_e9_both_ends_in_one_sample}, {"e10", test_e10_line_width},
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        if (argc < 2 || strcmp(argv[1], CASES[i].name) == 0) {
            CASES[i].fn();
        }
    }
    puts("ok");
    return 0;
}
