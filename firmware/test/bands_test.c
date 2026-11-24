#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "acomp.h"
#include "aglue.h"
#include "apack_golden.h"
#include "app_config.h"
#include "battstate.h"
#include "motion.h"
#include "volarc.h"

#define SCENE SCENE_SIZE_PX
#define OUT SCENE_OUT_PX
#define POINTS (ORBIT_COLS * ORBIT_ROWS)
#define U BUBBLE_UNIT_PX
#define C MASCOT_CELL_PX

/* Bubble states by content in units and shape, as ui.c's BUBBLES table sizes them; IDLE has none. beside: the battery icon sits by the bubble. */
typedef struct {
    int content_u;
    bool thought, beside;
} state_t;

static const state_t STATES[] = {{0, false, false}, {34, false, false}, {33, true, false}, {34, false, false}, {36, false, true}, {24, false, true}};
#define N_STATES ((int)(sizeof STATES / sizeof STATES[0]))

static irect_t grown(irect_t b, int x0, int y0, int x1, int y1)
{
    b.x0 = x0 < b.x0 ? x0 : b.x0;
    b.y0 = y0 < b.y0 ? y0 : b.y0;
    b.x1 = x1 > b.x1 ? x1 : b.x1;
    b.y1 = y1 > b.y1 ? y1 : b.y1;
    return b;
}

static void scene_spans(irect_t bb, int tenths, xspan_t *sp)
{
    irect_t m = {bb.x0 - 1, bb.y0 - 1, bb.x1 + 1, bb.y1 + 1};
    m.x0 = m.x0 < 0 ? 0 : m.x0;
    m.y0 = m.y0 < 0 ? 0 : m.y0;
    m.x1 = m.x1 > SCENE ? SCENE : m.x1;
    m.y1 = m.y1 > SCENE ? SCENE : m.y1;
    rotblit_spans(&m, SCENE, OUT, tenths, sp);
}

static int count(const xspan_t *sp)
{
    irect_t r[ACOMP_MAX_BANDS];
    return acomp_bands(sp, OUT, DIRTY_BAND_ROWS, r, ACOMP_MAX_BANDS);
}

static irect_t with_icon(irect_t b, ag_rect_t icon)
{
    const ag_rect_t p = ag_batt_plate(icon);
    return grown(b, p.x, p.y, p.x + p.w, p.y + p.h);
}

/* The bounding rectangle ui.c grows for the static mascot at pop 3 (pop 0 for IDLE): body, claws and legs, the top bubble and the battery icon.
   The icon box includes the sticker skin's plate, a bound for this skin. */
static irect_t clawd_bb(const state_t *s, int gx, int gy)
{
    const int shift = s->content_u ? BUBBLE_SHIFT_UNITS * U : 0;
    const int ox = SCENE / 2 - 8 * C + gx, oy = SCENE / 2 - 5 * C + gy + shift;
    irect_t b = {SCENE, SCENE, 0, 0};
    b = grown(b, ox, oy, ox + 16 * C, oy + 10 * C);
    if (s->content_u) {
        const ag_top_bubble_t t = ag_top_bubble(ag_bubble_w_units(s->content_u), s->thought, 3, gx, gy, SCENE);
        b = grown(b, t.body.x, t.body.y, t.body.x + t.body.w, t.body.y + t.body.h);
        if (s->beside) {
            b = with_icon(b, ag_batt_beside_top(&t, BATT_BOLT_W, BATT_BOLT_H, SCENE));
        }
    } else {
        b = with_icon(b, ag_batt_below(BATT_BOLT_W, BATT_BOLT_H, gx, gy, SCENE));
    }
    return b;
}

typedef struct {
    int worst, over_ring;
    long cases;
    int at_tenths, at_point;
} worst_t;

static void note(worst_t *w, int n, int t, int pt)
{
    w->cases++;
    w->over_ring += n > VOLARC_RING_MAX_BANDS;
    if (n > w->worst) {
        w->worst = n;
        w->at_tenths = t;
        w->at_point = pt;
    }
}

static void clawd_sweep(void)
{
    static xspan_t sp[OUT];
    worst_t w[N_STATES];
    memset(w, 0, sizeof w);
    int worst = 0, who = 0;
    for (int s = 0; s < N_STATES; s++) {
        for (int i = 0; i < POINTS; i++) {
            int gx, gy;
            dp_orbit(i, &gx, &gy);
            const irect_t bb = clawd_bb(&STATES[s], gx, gy);
            for (int t = 0; t < 3600; t++) {
                scene_spans(bb, t, sp);
                note(&w[s], count(sp), t, i);
            }
        }
        assert(w[s].worst <= VOLARC_RING_MAX_BANDS && w[s].over_ring == 0);
        if (w[s].worst > worst) {
            worst = w[s].worst;
            who = s;
        }
    }
    /* A scene with no content hands present() the inverted margin; it must cover no row at any angle. */
    for (int t = 0; t < 3600; t++) {
        scene_spans((irect_t){SCENE, SCENE, 0, 0}, t, sp);
        assert(count(sp) == 0);
        for (int y = 0; y < OUT; y++) {
            assert(sp[y].x0 >= sp[y].x1);
        }
    }
    /* Red proof: the whole scene as the rectangle exceeds the limit, so the assertion above can fail. */
    scene_spans((irect_t){0, 0, SCENE, SCENE}, 0, sp);
    assert(count(sp) > VOLARC_RING_MAX_BANDS);
    printf("W1: Clawd, %d states x %d points x 3600 angles: worst %d bands (state %d, tenths %d, point %d), ring limit %d\n", N_STATES, POINTS,
           worst, who, w[who].at_tenths, w[who].at_point, VOLARC_RING_MAX_BANDS);
}

static ap_pack_t synthetic(int w, int h)
{
    ap_pack_t p;
    memset(&p, 0, sizeof p);
    p.canvas_w = p.canvas_h = 476;
    p.scale = 1;
    p.clip_count = 1;
    p.clip[0].x = (uint16_t)((476 - w) / 2);
    p.clip[0].y = (uint16_t)((476 - h) / 2);
    p.clip[0].w = (uint16_t)w;
    p.clip[0].h = (uint16_t)h;
    return p;
}

/* Side bubbles at pop 3 for the contents and sides in use: the pure box over the body and the tail or dots. */
typedef struct {
    ag_content_t content;
    bool thought, left;
    int bub_y;
} bub_t;

static irect_t bubble_bb(const bub_t *b, int gx, int gy)
{
    const ag_bubble_t g = ag_side_bubble(b->content, b->thought, b->left, b->bub_y, gx, gy, 3, 0, SCENE);
    return ag_bubble_bbox(&g);
}

static void sticker_sweep(void)
{
    static uint8_t dummy[512 * 512];
    memset(dummy, 1, sizeof dummy);
    ap_pack_t g;
    assert(ap_parse(APACK_GOLDEN_PACK, sizeof APACK_GOLDEN_PACK, sizeof APACK_GOLDEN_PACK, &g) == AP_OK);
    /* Crops: the golden clip, and synthetic ones whose far corner is at the reach cap (square, wide, tall). */
    const ap_pack_t syn[3] = {synthetic(292, 292), synthetic(376, 168), synthetic(168, 376)};
    const ap_pack_t *const packs[4] = {&g, &syn[0], &syn[1], &syn[2]};
    const char *const names[4] = {"golden", "292x292", "376x168", "168x376"};
    const int ys[] = {-96, 0, 48};
    static xspan_t art[4][OUT], scn[OUT], un[OUT];
    worst_t w[4];
    memset(w, 0, sizeof w);
    const ag_content_t contents[4] = {{2, false, true}, {3, false, false}, {2, false, false}, {2, true, false}}; /* the last is the thought bubble */
    for (int i = 0; i < POINTS; i++) {
        int gx, gy;
        dp_orbit(i, &gx, &gy);
        irect_t bbs[4 * 2 * 3 + 1];
        int nb = 0;
        for (int wi = 0; wi < 4; wi++) {
            for (int side = 0; side < 2; side++) {
                for (size_t yi = 0; yi < sizeof ys / sizeof ys[0]; yi++) {
                    const bub_t b = {contents[wi], wi == 3, side == 1, ys[yi]};
                    bbs[nb] = bubble_bb(&b, gx, gy);
                    if (wi == 1 || wi == 2) { /* the ERROR and OFFLINE contents carry the icon beside them */
                        const ag_bubble_t g = ag_side_bubble(b.content, b.thought, b.left, b.bub_y, gx, gy, 3, 0, SCENE);
                        bbs[nb] = with_icon(bbs[nb], ag_batt_beside_side(&g, b.left, BATT_BOLT_W, BATT_BOLT_H));
                    }
                    nb++;
                }
            }
        }
        bbs[nb++] = with_icon((irect_t){SCENE, SCENE, 0, 0}, ag_batt_below(BATT_BOLT_W, BATT_BOLT_H, gx, gy, SCENE)); /* pop 0: the icon alone */
        for (int t = 0; t < 3600; t++) {
            for (int k = 0; k < 4; k++) {
                const ap_clip_t *c = k == 0 ? &g.clip[1] : &packs[k]->clip[0];
                const ag_place_t pl = ag_sticker_place(packs[k], c, gx, gy, SCENE);
                const acomp_art_t a = {dummy, c->w, c->h, c->w, packs[k]->scale, pl.ox, pl.oy, t, SCENE, OUT};
                acomp_spans(&a, art[k]);
                note(&w[k], count(art[k]), t, i);
            }
            for (int b = 0; b < nb; b++) {
                scene_spans(bbs[b], t, scn);
                for (int k = 0; k < 4; k++) {
                    acomp_dirty(art[k], scn, un, OUT);
                    note(&w[k], count(un), t, i);
                }
            }
        }
    }
    int worst = 0;
    for (int k = 0; k < 4; k++) {
        printf("W2: sticker %s over %ld cases: worst %d bands (tenths %d, point %d), %d cases above the ring limit %d\n", names[k], w[k].cases,
               w[k].worst, w[k].at_tenths, w[k].at_point, w[k].over_ring, VOLARC_RING_MAX_BANDS);
        worst = w[k].worst > worst ? w[k].worst : worst;
    }
    /* Above the ring limit the ring waits, so the frame holds arc + bands areas; every arc count up to 20 stays within LVGL's 32. */
    for (int arc = 0; arc <= 20; arc++) {
        for (int bands = 0; bands <= worst; bands++) {
            assert(arc + bands + (volarc_ring_ok(arc, bands) ? VOLARC_RING_AREAS : 0) <= 32);
        }
    }
    assert(worst <= ACOMP_MAX_BANDS);
}

/* R1: a ring redraw owed on every tick of a scene over the ring's strip limit waits at most one tick. */
static void ring_wait(void)
{
    for (int bands = 0; bands <= ACOMP_MAX_BANDS; bands++) {
        for (int arc = 0; arc <= VOLARC_RING_MAX_AREAS; arc++) {
            bool waited = false, old_waited = false;
            int run = 0, worst = 0, old_run = 0, old_worst = 0;
            for (int tick = 0; tick < 8; tick++) {
                const bool room = volarc_ring_takes_screen(bands, waited) || volarc_ring_ok(arc, bands);
                waited = !room;
                run = room ? 0 : run + 1;
                worst = run > worst ? run : worst;
                old_waited = !volarc_ring_ok(arc, bands);
                old_run = old_waited ? old_run + 1 : 0;
                old_worst = old_run > old_worst ? old_run : old_worst;
            }
            assert(worst <= 1);
            /* Red proof: without the whole-screen tick the ring waits on every tick above the limit. */
            assert(bands <= VOLARC_RING_MAX_BANDS || old_worst == 8);
        }
    }
    printf("R1: above %d strips the ring waits at most one tick\n", VOLARC_RING_MAX_BANDS);
}

int main(void)
{
    clawd_sweep();
    sticker_sweep();
    ring_wait();
    return 0;
}
