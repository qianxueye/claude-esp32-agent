#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "volknob.h"

#define CTR 232.5
#define PITCH ((double)BRT_TRAVEL_PX / BRT_STEPS)
#define BRT_ANY (VK_EV_BRT_ARM | VK_EV_BRT | VK_EV_BRT_STOP | VK_EV_BRT_COMMIT | VK_EV_BRT_CANCEL)

static const int QUADS[4] = {0, 90, 180, 270};

/* One touch fed in the UI frame of angle deg: (fu, fl) is the travel along UI-up and UI-right from the press point. */
typedef struct {
    volknob_t k;
    uint32_t t;
    int ev, x0, y0, bi0, stops, steps;
    double ux, uy, fu, fl;
} sw_t;

static void sw_press(sw_t *s, int x, int y, int deg, bool dark, int bi0)
{
    *s = (sw_t){.t = 1000, .x0 = x, .y0 = y, .bi0 = bi0};
    double a = deg * 3.14159265358979323846 / 180.0;
    s->ux = round(sin(a));
    s->uy = -round(cos(a));
    vk_press(&s->k, x, y, s->t, dark, 50);
    vk_swipe_frame(&s->k, (float)deg);
}

/* As the UI glue: the swipe is rebased to its start index when it arms. */
static int sw_feed(sw_t *s, int ev)
{
    if (ev & VK_EV_BRT_ARM) {
        vk_swipe_rebase(&s->k, s->bi0);
    }
    s->stops += (ev & VK_EV_BRT_STOP) != 0;
    s->steps += (ev & VK_EV_BRT) != 0;
    s->ev |= ev;
    return ev;
}

static int sw_at(sw_t *s, double fu, double fl, uint32_t dt)
{
    s->fu = fu;
    s->fl = fl;
    s->t += dt;
    int x = s->x0 + (int)lround(fu * s->ux - fl * s->uy), y = s->y0 + (int)lround(fu * s->uy + fl * s->ux);
    int ev = sw_feed(s, vk_move(&s->k, x, y, s->t));
    return ev | sw_feed(s, vk_tick(&s->k, s->t));
}

/* Moves by (du, dl) in n equal samples dt ms apart. */
static void sw_slide(sw_t *s, double du, double dl, int n, uint32_t dt)
{
    double u0 = s->fu, l0 = s->fl;
    for (int i = 1; i <= n; i++) {
        sw_at(s, u0 + du * i / n, l0 + dl * i / n, dt);
    }
}

static void centre(sw_t *s, int deg, int bi0)
{
    sw_press(s, 233, 233, deg, false, bi0);
}

/* Red proof for G2: a rule that looks only at duration sends a tap for any short touch. */
static bool wrong_tap_duration_only(uint32_t dur)
{
    return dur < TAP_MAX_MS;
}

/* Red proof for G2: the centre transition without the long-press test. */
static int wrong_centre_next(bool moved, bool long_sent)
{
    (void)long_sent;
    return moved ? VK_SWIPE : VK_CENTRE;
}

static void test_g2_tap_and_long_rotated(void)
{
    for (int q = 0; q < 4; q++) {
        sw_t s;
        centre(&s, QUADS[q], 16);
        for (int i = 0; s.t + 16 < 1000 + LONG_PRESS_MS; i++) {
            sw_at(&s, (i & 1) ? 13 : -13, 0, 16);
        }
        assert(s.ev == 0 && !s.k.moved);
        assert(vk_tick(&s.k, 1000 + LONG_PRESS_MS - 1) == 0 && vk_tick(&s.k, 1000 + LONG_PRESS_MS) == VK_EV_LONG);

        centre(&s, QUADS[q], 16);
        sw_slide(&s, 13, 0, 13, 16);
        assert(vk_release(&s.k, 1000 + TAP_MAX_MS - 1) == VK_EV_TAP);

        const int far[2] = {14, 47};
        for (int j = 0; j < 2; j++) {
            centre(&s, QUADS[q], 16);
            sw_slide(&s, far[j], 0, far[j], 8);
            assert(!(s.ev & BRT_ANY) && vk_release(&s.k, s.t + 1) == 0);
            assert(wrong_tap_duration_only(s.t + 1 - 1000));
        }

        centre(&s, QUADS[q], 16);
        s.t = 1000 + LONG_PRESS_MS;
        sw_feed(&s, vk_tick(&s.k, s.t));
        assert(s.ev == VK_EV_LONG);
        sw_slide(&s, 100, 0, 25, 16);
        assert(!(s.ev & BRT_ANY) && s.k.state == VK_DEAD && s.k.why == VK_WHY_MOVED);
        assert(wrong_centre_next(true, true) == VK_SWIPE);
    }
}

/* Red proof for G3: steps counted from the press point instead of the arming point. */
static int wrong_gain_from_press(int bi0, double up)
{
    return bi0 + (int)(fabs(up) / PITCH);
}

static void test_g3_arming_never_steps(void)
{
    for (int q = 0; q < 4; q++) {
        sw_t s;
        centre(&s, QUADS[q], 16);
        sw_slide(&s, 47, 0, 47, 8);
        assert(!(s.ev & BRT_ANY) && s.k.state == VK_SWIPE);
        sw_slide(&s, 1, 0, 1, 8);
        assert((s.ev & VK_EV_BRT_ARM) && !(s.ev & VK_EV_BRT) && s.k.bi == 16 && s.k.bi0 == 16 && s.k.state == VK_BRIGHT);
        assert(wrong_gain_from_press(16, s.k.up) == 16 + 6);
    }
}

/* Red proof for G4: a rule that tests only the travel along UI-up. */
static bool wrong_vertical_only(double up_peak)
{
    return up_peak >= BRT_ARM_PX;
}

static void test_g4_cone(void)
{
    const int deg_ok = 20, deg_bad[3] = {30, 45, 90};
    for (int q = 0; q < 4; q++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            sw_t s;
            double a = deg_ok * 3.14159265358979323846 / 180.0;
            centre(&s, QUADS[q], 16);
            sw_slide(&s, sign * 80 * cos(a), 80 * sin(a), 80, 4);
            assert(s.ev & VK_EV_BRT_ARM);
            for (int j = 0; j < 3; j++) {
                a = deg_bad[j] * 3.14159265358979323846 / 180.0;
                centre(&s, QUADS[q], 16);
                sw_slide(&s, sign * 80 * cos(a), 80 * sin(a), 80, 4);
                assert(!(s.ev & BRT_ANY) && s.k.state == VK_DEAD && s.k.why == VK_WHY_LATERAL);
                if (deg_bad[j] == 45) {
                    assert(wrong_vertical_only(s.k.up_peak));
                }
            }
        }
    }
}

/* Red proof for G5: no deadline. */
static bool wrong_no_deadline(double up_peak)
{
    return up_peak >= BRT_ARM_PX;
}

static void test_g5_deadline(void)
{
    for (int q = 0; q < 4; q++) {
        sw_t s;
        centre(&s, QUADS[q], 16);
        sw_at(&s, 14, 0, 16);
        uint32_t t_moved = s.t;
        assert(s.k.state == VK_SWIPE && s.k.t_moved == t_moved);
        sw_slide(&s, 33, 0, 33, 15);
        sw_at(&s, 48, 0, BRT_ARM_MAX_MS - 1 - (s.t - t_moved));
        assert((s.ev & VK_EV_BRT_ARM) && s.k.arm_ms == BRT_ARM_MAX_MS - 1);

        centre(&s, QUADS[q], 16);
        sw_at(&s, 14, 0, 16);
        sw_slide(&s, 33, 0, 33, 15);
        sw_at(&s, 48, 0, BRT_ARM_MAX_MS + 1 - (s.t - s.k.t_moved));
        assert(!(s.ev & BRT_ANY) && s.k.why == VK_WHY_SLOW);

        centre(&s, QUADS[q], 16);
        sw_slide(&s, 150, 0, 150, 17);
        assert(!(s.ev & BRT_ANY) && s.k.state == VK_DEAD && s.k.why == VK_WHY_SLOW);
        assert(wrong_no_deadline(s.k.up_peak));
    }
}

static void test_g6_edge_starts_never_adjust(void)
{
    const int radii[3] = {152, 176, 200};
    for (int q = 0; q < 4; q++) {
        for (int j = 0; j < 3; j++) {
            sw_t s;
            double a = QUADS[q] * 3.14159265358979323846 / 180.0;
            int x = (int)lround(CTR - radii[j] * round(sin(a))), y = (int)lround(CTR + radii[j] * round(cos(a)));
            sw_press(&s, x, y, QUADS[q], false, 16);
            assert(s.k.cls == VK_EDGE);
            bool wrong_armed = false;
            for (int i = 1; i <= 75; i++) {
                sw_at(&s, 2 * i, 0, 8);
                wrong_armed |= s.k.r1 < VOL_R_MIN_PX && fabs(s.k.up) >= BRT_ARM_PX;
            }
            assert(!(s.ev & BRT_ANY) && !(vk_release(&s.k, s.t) & BRT_ANY));
            /* Re-classing by the current radius arms on the same samples: the red proof. */
            assert(wrong_armed);
        }
    }
    volknob_t k;
    int ev = 0;
    uint32_t t = 1000;
    vk_press(&k, (int)lround(CTR + 190), (int)lround(CTR), t, false, 50);
    vk_swipe_frame(&k, 0);
    for (int a = 1; a <= 60; a++) {
        double r = a * 3.14159265358979323846 / 180.0;
        ev |= vk_move(&k, (int)lround(CTR + 190 * cos(r)), (int)lround(CTR + 190 * sin(r)), t += 16);
    }
    ev |= vk_release(&k, t);
    assert((ev & VK_EV_ARM) && (ev & VK_EV_COMMIT) && !(ev & BRT_ANY));
}

/* Red proof for G7: darkness tested when the swipe would arm, after the press has lit the screen. */
static bool wrong_dark_at_arm(bool dark_now, double up_peak)
{
    return !dark_now && up_peak >= BRT_ARM_PX;
}

static void test_g7_dark_press(void)
{
    for (int q = 0; q < 4; q++) {
        sw_t s;
        sw_press(&s, 233, 233, QUADS[q], true, 16);
        sw_slide(&s, 150, 0, 50, 8);
        assert(s.ev == 0 && vk_release(&s.k, s.t) == 0 && s.k.why == VK_WHY_DARK);
        assert(wrong_dark_at_arm(false, s.k.up_peak));
    }
}

/* Red proof for G8: travel read in the panel frame, ignoring the UI angle. */
static int wrong_panel_frame(int dx, int dy, int bi0)
{
    if (abs(dx) > BRT_LAT_PX) {
        return -1;
    }
    return bi0 + (int)((-dy - BRT_ARM_PX) / PITCH);
}

static void test_g8_direction_follows_ui(void)
{
    for (int q = 0; q < 4; q++) {
        sw_t up, down;
        centre(&up, QUADS[q], 16);
        sw_slide(&up, BRT_ARM_PX + 10 * PITCH, 0, 41, 8);
        centre(&down, QUADS[q], 16);
        sw_slide(&down, -(BRT_ARM_PX + 10 * PITCH), 0, 41, 8);
        assert(up.k.bi == 26 && down.k.bi == 6);
        assert(vk_release(&up.k, up.t) == VK_EV_BRT_COMMIT && vk_release(&down.k, down.t) == VK_EV_BRT_COMMIT);
        int dx = up.k.lx - up.k.x0, dy = up.k.ly - up.k.y0;
        if (QUADS[q] == 180) {
            assert(wrong_panel_frame(dx, dy, 16) < 16);
        } else if (QUADS[q] == 90) {
            assert(wrong_panel_frame(dx, dy, 16) == -1);
        }
    }
}

static void test_g9_frame_latched(void)
{
    sw_t s;
    centre(&s, 0, 16);
    sw_slide(&s, BRT_ARM_PX, 0, 12, 8);
    assert(s.ev & VK_EV_BRT_ARM);
    vk_swipe_frame(&s.k, 90);
    assert(s.k.ux == 0 && s.k.uy == -1 && s.k.quad == 0);
    int dy_at_arm = s.k.ly - s.k.y0;
    sw_slide(&s, 4 * PITCH, 0, 10, 8);
    assert(s.k.bi == 20);
    /* Re-projecting onto the late 90 degree frame turns the same vertical travel into none. */
    int dx = s.k.lx - s.k.x0, dy = s.k.ly - s.k.y0;
    double wrong_up = dx * 1.0 + dy * 0.0, wrong_up_at_arm = 0 * 1.0 + dy_at_arm * 0.0;
    assert((int)((wrong_up - wrong_up_at_arm) / PITCH) == 0);
}

/* Red proof for G10: a zone that must hold until arming. */
static bool wrong_zone_kept(double r_at_arm)
{
    return r_at_arm < VOL_R_MIN_PX;
}

static void test_g10_zone_is_start_only(void)
{
    sw_t s;
    sw_press(&s, 233, 92, 0, false, 16);
    assert(s.k.cls == VK_CENTRE && s.k.r0 < 141);
    double r_at_arm = -1;
    for (int i = 1; i <= 84; i++) {
        if (sw_at(&s, i, 0, 4) & VK_EV_BRT_ARM) {
            r_at_arm = s.k.r1;
        }
    }
    assert(r_at_arm > 188 && r_at_arm < 189 && s.k.r1 > 224 && s.k.r1 < 225);
    assert(s.k.bi == 20 && vk_release(&s.k, s.t) == VK_EV_BRT_COMMIT);
    assert(!wrong_zone_kept(r_at_arm));
}

/* Red proof for G11: an index that keeps counting past the end, clamped only for display. */
static int wrong_unclamped(int bi0, double live_px)
{
    int i = bi0 + (int)(live_px / PITCH);
    return i > BRT_STEPS ? BRT_STEPS : i < 0 ? 0 : i;
}

static void test_g11_ends_saturate(void)
{
    for (int dir = 1; dir >= -1; dir -= 2) {
        int bi = dir > 0 ? 0 : BRT_STEPS;
        sw_t s;
        for (int n = 0; n < 3; n++) {
            sw_press(&s, 233, 233 + dir * 140, 0, false, bi);
            sw_slide(&s, dir * 400, 0, 50, 16);
            bi = s.k.bi;
            if (n < 2) {
                assert(vk_release(&s.k, s.t) == VK_EV_BRT_COMMIT);
            }
        }
        assert(bi == (dir > 0 ? BRT_STEPS : 0));
        sw_slide(&s, -dir * 15, 0, 5, 16);
        assert(s.k.bi == (dir > 0 ? BRT_STEPS - 2 : 2));
        assert(wrong_unclamped(dir > 0 ? BRT_STEPS : 0, dir * (400 - BRT_ARM_PX - 15)) == (dir > 0 ? BRT_STEPS : 0));
    }
}

/* Red proof for G12: the level rounded to the nearest step of the travel since arming. */
static int wrong_round(int bi0, double live_px)
{
    return bi0 + (int)lround(live_px / PITCH);
}

static void test_g12_no_flicker(void)
{
    sw_t s;
    centre(&s, 0, 16);
    sw_slide(&s, BRT_ARM_PX, 0, 12, 8);
    sw_slide(&s, 11, 0, 11, 8);
    assert(s.k.bi == 17);
    const int noise[4] = {3, -3, -3, 3};
    int wrong_lo = 99, wrong_hi = -1;
    for (int i = 0; i < 1000; i++) {
        sw_at(&s, s.fu + noise[i & 3], 0, 16);
        assert(s.k.bi == 17);
        int w = wrong_round(16, s.fu - BRT_ARM_PX);
        wrong_lo = w < wrong_lo ? w : wrong_lo;
        wrong_hi = w > wrong_hi ? w : wrong_hi;
    }
    assert(wrong_lo == 17 && wrong_hi == 18);
}

static void test_g13_one_stop_per_arrival(void)
{
    sw_t s;
    centre(&s, 0, BRT_STEPS);
    sw_slide(&s, BRT_ARM_PX, 0, 12, 8);
    sw_slide(&s, 8, 0, 2, 8);
    assert(s.stops == 1 && s.k.bi == BRT_STEPS);
    int wrong_stops = 0;
    for (int i = 0; i < 50; i++) {
        sw_at(&s, s.fu + 4, 0, 8);
        wrong_stops += s.k.bi == BRT_STEPS;
    }
    assert(s.stops == 1);
    /* One event per outward sample at the end would send 50 more: the red proof. */
    assert(wrong_stops == 50);
    sw_slide(&s, -8, 0, 2, 8);
    assert(s.k.bi == BRT_STEPS - 1 && s.k.push == 0);
    sw_slide(&s, 16, 0, 4, 8);
    assert(s.stops == 2 && s.k.bi == BRT_STEPS);

    centre(&s, 0, BRT_STEPS - 1);
    sw_slide(&s, BRT_ARM_PX, 0, 12, 8);
    sw_slide(&s, 8, 0, 2, 8);
    assert(s.k.bi == BRT_STEPS && s.stops == 0);

    centre(&s, 0, 0);
    sw_slide(&s, -BRT_ARM_PX, 0, 12, 8);
    sw_slide(&s, -8, 0, 2, 8);
    assert(s.stops == 1 && s.k.bi == 0);
}

/* Red proof for G14: without the distance limit the spike is committed. */
static int wrong_no_limit(int bi, double spike_px)
{
    int i = bi + (int)(spike_px / PITCH);
    return i > BRT_STEPS ? BRT_STEPS : i;
}

static void test_g14_jump(void)
{
    for (int q = 0; q < 4; q++) {
        sw_t s;
        centre(&s, QUADS[q], 10);
        sw_at(&s, 14, 0, 16);
        sw_at(&s, 174, 0, 100);
        assert(!(s.ev & BRT_ANY) && s.k.why == VK_WHY_JUMP);

        centre(&s, QUADS[q], 10);
        sw_slide(&s, BRT_ARM_PX, 0, 12, 16);
        sw_slide(&s, 10 * PITCH, 0, 15, 16);
        assert(s.k.bi == 20);
        int before = s.k.bi;
        s.ev = 0;
        sw_at(&s, s.fu + 160, 0, 200);
        assert((s.ev & VK_EV_BRT_CANCEL) && s.k.bi == 10 && s.k.bi0 == 10 && vk_release(&s.k, s.t + 10) == 0);
        assert(wrong_no_limit(before, 160) != 10);

        centre(&s, QUADS[q], 10);
        sw_slide(&s, 3 * 48, 0, 3, 16);
        assert((s.ev & VK_EV_BRT_ARM) && !(s.ev & VK_EV_BRT_CANCEL));
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 2; j++) {
                s.t += 16;
                sw_feed(&s, vk_move(&s.k, s.k.lx, s.k.ly, s.t));
            }
            sw_at(&s, s.fu + 3 * 48, 0, 16);
        }
        assert(!(s.ev & VK_EV_BRT_CANCEL) && s.k.state == VK_BRIGHT);
    }
}

static void test_g15_lost(void)
{
    sw_t s;
    centre(&s, 0, 16);
    sw_slide(&s, 60, 0, 15, 8);
    assert(s.k.state == VK_BRIGHT && vk_lost(&s.k) == VK_EV_BRT_COMMIT);
    centre(&s, 0, 16);
    sw_slide(&s, 30, 0, 10, 8);
    assert(s.k.state == VK_SWIPE && vk_lost(&s.k) == 0);
    centre(&s, 0, 16);
    sw_slide(&s, 30, 0, 10, 8);
    assert(vk_release(&s.k, s.t) == 0);
}

int main(void)
{
    test_g2_tap_and_long_rotated();
    test_g3_arming_never_steps();
    test_g4_cone();
    test_g5_deadline();
    test_g6_edge_starts_never_adjust();
    test_g7_dark_press();
    test_g8_direction_follows_ui();
    test_g9_frame_latched();
    test_g10_zone_is_start_only();
    test_g11_ends_saturate();
    test_g12_no_flicker();
    test_g13_one_stop_per_arrival();
    test_g14_jump();
    test_g15_lost();
    puts("ok");
    return 0;
}
