#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "volknob.h"

#define PI_D 3.14159265358979323846
#define CTR 232.5

static int px(double r, double deg) { return (int)lround(CTR + r * cos(deg * PI_D / 180.0)); }
static int py(double r, double deg) { return (int)lround(CTR + r * sin(deg * PI_D / 180.0)); }

typedef struct { volknob_t k; uint32_t t; int ev; double deg, r; } drag_t;

static void drag_start(drag_t *d, double r, double deg, bool dark, int vol)
{
    d->t = 1000; d->ev = 0; d->r = r; d->deg = deg;
    vk_press(&d->k, px(r, deg), py(r, deg), d->t, dark, (uint8_t)vol);
}

/* Moves by delta_deg in steps of step_deg, one sample per dt ms; ORs every event. */
static void drag_turn(drag_t *d, double delta_deg, double step_deg, uint32_t dt)
{
    int n = (int)ceil(fabs(delta_deg) / step_deg);
    for (int i = 0; i < n; i++) {
        d->deg += delta_deg / n;
        d->t += dt;
        d->ev |= vk_move(&d->k, px(d->r, d->deg), py(d->r, d->deg), d->t);
        d->ev |= vk_tick(&d->k, d->t);
    }
}

static void test_k1_edge_never_long(void)
{
    drag_t d;
    drag_start(&d, 190, 0, false, 50);
    uint32_t end = d.t + 10000;
    while (d.t < end) { d.t += 16; d.ev |= vk_tick(&d.k, d.t); }
    assert(!(d.ev & VK_EV_LONG) && !(vk_release(&d.k, d.t) & VK_EV_TAP));
    drag_start(&d, 190, 0, false, 50);
    for (int i = 0; i < 60; i++) { drag_turn(&d, 0.5, 0.5, 1000); }
    assert(!(d.ev & VK_EV_LONG));
    /* a slop-only rule would still send LONG at 1500 ms: the finger has moved well under the slop */
    drag_start(&d, 190, 0, false, 50);
    drag_turn(&d, 0.75, 0.75, 1500);
    assert(!d.k.moved && d.k.max_disp < TOUCH_SLOP_PX && !(d.ev & VK_EV_LONG));
}

static void test_k2_moved_never_tap(void)
{
    volknob_t k;
    vk_press(&k, 233, 233, 0, false, 50);
    vk_move(&k, 273, 233, 100);
    assert(vk_release(&k, 200) == 0);
    vk_press(&k, 423, 232, 0, false, 50);
    vk_move(&k, 423, 272, 100);
    assert(vk_release(&k, 200) == 0);
    vk_press(&k, 233, 233, 0, false, 50);
    vk_move(&k, 240, 233, 100);
    assert(vk_release(&k, 200) == VK_EV_TAP);
}

static void test_k3_centre_hold_and_tap(void)
{
    volknob_t k;
    vk_press(&k, 233, 233, 5000, false, 50);
    assert(vk_tick(&k, 5000 + LONG_PRESS_MS - 1) == 0);
    assert(vk_tick(&k, 5000 + LONG_PRESS_MS) == VK_EV_LONG);
    assert(vk_tick(&k, 5000 + LONG_PRESS_MS + 16) == 0);
    assert(vk_release(&k, 7000) == 0);
    vk_press(&k, 233, 233, 0xFFFFFF00u, false, 50);
    assert(vk_release(&k, 0xFFFFFF00u + TAP_MAX_MS - 1) == VK_EV_TAP);
    vk_press(&k, 233, 233, 0, false, 50);
    assert(vk_release(&k, TAP_MAX_MS) == 0);
    vk_press(&k, 423, 232, 0, false, 50);
    assert(vk_release(&k, TAP_MAX_MS - 1) == VK_EV_TAP);
    vk_press(&k, 233, 233, 0, false, 50);
    assert(vk_move(&k, 238, 236, 400) == 0);
    assert(vk_tick(&k, LONG_PRESS_MS) == VK_EV_LONG);
}

static void test_k4_dark_does_nothing(void)
{
    drag_t d;
    drag_start(&d, 190, 0, true, 50);
    drag_turn(&d, 180, 1, 16);
    assert(d.ev == 0 && vk_release(&d.k, d.t) == 0);
    vk_press(&d.k, 233, 233, 0, true, 50);
    assert(vk_release(&d.k, 100) == 0);
}

static void test_k5_no_volume_before_arm(void)
{
    drag_t d;
    drag_start(&d, 190, 0, false, 50);
    for (int i = 0; i < 1000; i++) { drag_turn(&d, 14, 2, 16); drag_turn(&d, -28, 2, 16); drag_turn(&d, 14, 2, 16); }
    assert(!(d.ev & VK_EV_ARM) && d.k.v == 50);
    drag_turn(&d, 15.5, 1, 16);
    assert((d.ev & VK_EV_ARM) && d.k.v == 50 && !(d.ev & VK_EV_VOL));
}

static void test_k6_clockwise_and_unwrap(void)
{
    for (int a = 0; a < 360; a += 90) {
        drag_t d;
        drag_start(&d, 190, a, false, 30);
        drag_turn(&d, 15, 1, 16);
        drag_turn(&d, 31, 1, 16);
        assert((d.ev & VK_EV_ARM) && d.k.v == 41);
    }
    drag_t d;
    drag_start(&d, 190, 160, false, 30);
    drag_turn(&d, 10, 10, 16);
    int ev = vk_move(&d.k, px(190, -170), py(190, -170), d.t + 16);
    assert((ev & VK_EV_ARM) && d.k.travel > 28.5 && d.k.travel < 31.5);
}

static void test_k7_ends_saturate(void)
{
    drag_t d;
    drag_start(&d, 190, 0, false, 70);
    drag_turn(&d, 1080, 1, 16);
    assert(d.k.v == 100);
    d.ev = 0;
    drag_turn(&d, -3, 1, 16);
    assert(d.k.v == 99);
    drag_start(&d, 190, 0, false, 30);
    drag_turn(&d, -1080, 1, 16);
    assert(d.k.v == 0);
    drag_turn(&d, 3, 1, 16);
    assert(d.k.v == 1);
}

static void test_k8_no_flicker(void)
{
    drag_t d;
    drag_start(&d, 190, 0, false, 50);
    drag_turn(&d, 16, 1, 16);
    assert(d.k.v == 50);
    for (int i = 0; i < 1000; i++) { drag_turn(&d, 0.3, 0.3, 16); drag_turn(&d, -0.3, 0.3, 16); assert(d.k.v == 50); }
}

static void test_k9_jump_cancels(void)
{
    drag_t d;
    drag_start(&d, 190, 0, false, 40);
    drag_turn(&d, 15, 1, 16);
    drag_turn(&d, 81, 1, 16);
    assert(d.k.v == 70 || d.k.v == 69);
    d.ev = 0;
    d.deg += 90;
    d.t += 200;
    d.ev |= vk_move(&d.k, px(190, d.deg), py(190, d.deg), d.t);
    assert((d.ev & VK_EV_CANCEL) && d.k.v == 40 && vk_release(&d.k, d.t + 10) == 0);
    drag_start(&d, 190, 0, false, 40);
    drag_turn(&d, 15, 1, 16);
    drag_turn(&d, 20, 20, 16);
    assert(!(d.ev & VK_EV_CANCEL));
    drag_turn(&d, 25, 25, 16);
    assert(d.ev & VK_EV_CANCEL);
}

static void test_k10_repeats_do_not_cancel(void)
{
    drag_t d;
    drag_start(&d, 190, 0, false, 10);
    drag_turn(&d, 15, 1, 16);
    for (int i = 0; i < 40; i++) {
        d.deg += 28.8;
        int x = px(190, d.deg), y = py(190, d.deg);
        for (int j = 0; j < 3; j++) { d.t += 16; d.ev |= vk_move(&d.k, j == 2 ? x : d.k.lx, j == 2 ? y : d.k.ly, d.t); }
    }
    assert(!(d.ev & VK_EV_CANCEL) && d.k.v > 10);
}

static void test_k11_leaving_zone_kills(void)
{
    volknob_t k;
    int ev = 0;
    vk_press(&k, 413, 232, 0, false, 50);
    for (int x = 413, t = 16; x >= 53; x -= 10, t += 16) { ev |= vk_move(&k, x, 232, (uint32_t)t); }
    for (int i = 0; i < 40; i++) { ev |= vk_move(&k, 53 + i, 232 - i, 700 + (uint32_t)i * 16); }
    assert(!(ev & VK_EV_ARM) && vk_release(&k, 2000) == 0);
}

static void test_commit_and_lost(void)
{
    drag_t d;
    drag_start(&d, 190, 0, false, 50);
    drag_turn(&d, 30, 1, 16);
    assert(vk_release(&d.k, d.t) == VK_EV_COMMIT);
    drag_start(&d, 190, 0, false, 50);
    drag_turn(&d, 30, 1, 16);
    assert(vk_lost(&d.k) == VK_EV_COMMIT);
    vk_press(&d.k, 233, 233, 0, false, 50);
    assert(vk_lost(&d.k) == 0);
}

int main(void)
{
    test_k1_edge_never_long();
    test_k2_moved_never_tap();
    test_k3_centre_hold_and_tap();
    test_k4_dark_does_nothing();
    test_k5_no_volume_before_arm();
    test_k6_clockwise_and_unwrap();
    test_k7_ends_saturate();
    test_k8_no_flicker();
    test_k9_jump_cancels();
    test_k10_repeats_do_not_cancel();
    test_k11_leaving_zone_kills();
    test_commit_and_lost();
    puts("ok");
    return 0;
}
