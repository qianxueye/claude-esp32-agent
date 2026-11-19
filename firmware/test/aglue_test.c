#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aglue.h"
#include "apack_golden.h"
#include "app_config.h"
#include "motion.h"

#define SCENE SCENE_SIZE_PX
#define STEP 4u

static const uint8_t *const GOLDEN_CLIPS[2] = {APACK_GOLDEN_CLIP0, APACK_GOLDEN_CLIP1};

static ap_pack_t golden(void)
{
    ap_pack_t p;
    assert(ap_parse(APACK_GOLDEN_PACK, sizeof APACK_GOLDEN_PACK, sizeof APACK_GOLDEN_PACK, &p) == AP_OK);
    return p;
}

/* A pack view with only the fields placement reads: one crop on a canvas. */
static ap_pack_t canvas(int cw, int ch, int scale, int x, int y, int w, int h, int reach)
{
    ap_pack_t p;
    memset(&p, 0, sizeof p);
    p.canvas_w = (uint16_t)cw;
    p.canvas_h = (uint16_t)ch;
    p.scale = (uint8_t)scale;
    p.clip_count = 1;
    p.clip[0].x = (uint16_t)x;
    p.clip[0].y = (uint16_t)y;
    p.clip[0].w = (uint16_t)w;
    p.clip[0].h = (uint16_t)h;
    p.clip[0].reach = (uint16_t)reach;
    assert(ap_layout_ok(p.canvas_w, p.canvas_h, p.scale, p.clip[0].x, p.clip[0].y, p.clip[0].w, p.clip[0].h, p.clip[0].reach));
    return p;
}

/* The sticker's canvas centre is the scene centre plus the recentred orbit (x + 2, y - 10), whatever the bubble, and nothing is clipped. */
static void test_sticker_place(void)
{
    _Static_assert(ORBIT_RECENTRE_X_PX == 2 && ORBIT_RECENTRE_Y_PX == -10, "recentred orbit offset");
    const ap_pack_t big = canvas(512, 512, 1, 116, 116, 280, 280, 198);
    const ap_pack_t odd = canvas(251, 253, 2, 40, 40, 100, 90, 103);
    const ap_pack_t *const packs[2] = {&big, &odd};
    for (int k = 0; k < 2; k++) {
        const ap_pack_t *p = packs[k];
        const ap_clip_t *c = &p->clip[0];
        const int s = p->scale;
        for (int i = 0; i < ORBIT_COLS * ORBIT_ROWS; i++) {
            int gx, gy;
            dp_orbit(i, &gx, &gy);
            const ag_place_t pl = ag_sticker_place(p, c, gx, gy, SCENE);
            assert(pl.ox - c->x * s + p->canvas_w * s / 2 == SCENE / 2 + gx + 2);
            assert(pl.oy - c->y * s + p->canvas_h * s / 2 == SCENE / 2 + gy - 10);
            assert(pl.x0 == pl.ox && pl.y0 == pl.oy && pl.x1 == pl.ox + c->w * s && pl.y1 == pl.oy + c->h * s);
        }
    }
    /* On a scene smaller than the crop the origin is negative and the rectangle is not clipped to it. */
    const ag_place_t pl = ag_sticker_place(&big, &big.clip[0], -12, -4, 100);
    assert(pl.ox == 50 - 12 + 2 - 256 + 116 && pl.ox < 0 && pl.x0 == pl.ox && pl.x1 == pl.ox + 280);
    /* Red proof: a rule that adds the bubble shift moves the same crop. */
    const int shifted_oy = SCENE / 2 - 4 + ORBIT_RECENTRE_Y_PX + BUBBLE_SHIFT_UNITS * BUBBLE_UNIT_PX - 256 + 116;
    assert(shifted_oy != ag_sticker_place(&big, &big.clip[0], -12, -4, SCENE).oy);
}

/* The four contents ui.c's BUBBLES table gives: speech with dots, the error text, the offline text, and the thought bubble with its spinner. */
typedef struct {
    ag_content_t content;
    int w_px;
    bool thought;
} bub_case_t;

static const bub_case_t BUB_CASES[4] = {{{2, false, true}, 160, false}, {{3, false, false}, 168, false}, {{2, false, false}, 120, false}, {{2, true, false}, 160, true}};

static bool rect_in_scene(ag_rect_t r)
{
    return r.x >= 0 && r.y >= 0 && r.x + r.w <= SCENE && r.y + r.h <= SCENE;
}

static bool rect_in_box(ag_rect_t r, irect_t b)
{
    return r.x >= b.x0 && r.y >= b.y0 && r.x + r.w <= b.x1 && r.y + r.h <= b.y1;
}

static ag_bubble_t side(const bub_case_t *bc, bool left, int y, int gx, int gy, int pop, int phase)
{
    return ag_side_bubble(bc->content, bc->thought, left, y, gx, gy, pop, phase, SCENE);
}

/* B1: the three widths at bub_y 0 on the right put the rectangle and the tail or dots where the layout says; every cell, the spinner, the dots and the text rows stay in the scene and in the box. */
static void test_b1_side_bubble_table(void)
{
    static const int REL_X0[3] = {28, 20, 68};
    for (int i = 0; i < ORBIT_COLS * ORBIT_ROWS; i++) {
        int gx, gy;
        dp_orbit(i, &gx, &gy);
        const int cx = SCENE / 2 + gx + 2, cy = SCENE / 2 + gy - 10;
        for (int k = 0; k < 3; k++) {
            const bub_case_t *bc = &BUB_CASES[k];
            assert(ag_bubble_w_units(ag_content_units(bc->content)) * BUBBLE_UNIT_PX == bc->w_px);
            const ag_bubble_t b = side(bc, false, 0, gx, gy, 3, 0);
            assert(b.body.x == cx + REL_X0[k] && b.body.x + b.body.w == cx + 188 && b.body.y == cy - 36 && b.body.h == 72);
            /* Tail on the inner edge at the centre row: rel x 20..28, y -8..8. */
            assert(b.n_cells == 4);
            assert(b.cells[1].ink && b.cells[1].r.x == cx + REL_X0[k] - 4 && b.cells[1].r.y == cy - 8 && b.cells[1].r.h == 16);
            assert(b.cells[3].ink && b.cells[3].r.x == cx + REL_X0[k] - 8 && b.cells[3].r.y == cy - 4 && b.cells[3].r.h == 8);
        }
        /* Thought dots of the 160 px bubble: rel 8..24 x -8..8 and -4..4 x -4..4. */
        const ag_bubble_t t = side(&BUB_CASES[3], false, 0, gx, gy, 3, 0);
        assert(t.n_cells == 2 && t.cells[0].r.x == cx + 8 && t.cells[0].r.w == 16 && t.cells[0].r.y == cy - 8 && t.cells[0].r.h == 16);
        assert(t.cells[1].r.x == cx - 4 && t.cells[1].r.w == 8 && t.cells[1].r.y == cy - 4 && t.cells[1].r.h == 8);
        /* Pinned content of the 160 px dots bubble at bub_y 0: text row at rel y -24, mask at rel x 40, dots after 2 glyphs and 2 units. */
        const ag_bubble_t d = side(&BUB_CASES[0], false, 0, gx, gy, 3, 1);
        assert(d.mask.x == cx + 40 && d.mask.y == cy - 36 + 12 && d.mask.w == 96 && d.mask.h == 48);
        assert(d.n_dots == 3 && d.dots[0].w == 4 && d.dots[1].w == 8 && d.dots[2].w == 4);
        assert(d.dots[0].x == d.mask.x + 96 + 8 && d.dots[1].x == d.dots[0].x + 12 && d.dots[0].y == d.mask.y + 44 - 4 && d.dots[1].y == d.mask.y + 44 - 8);
        assert(t.spinner && t.spin.x == cx + 40 && t.spin.w == 28 && t.spin.y == t.body.y + 12 + 8);
    }
    int cells = 0;
    for (int c = 0; c < 4; c++) {
        const bub_case_t *bc = &BUB_CASES[c];
        for (int left = 0; left < 2; left++) {
            for (int y = -100; y <= 100; y += 4) {
                for (int i = 0; i < ORBIT_COLS * ORBIT_ROWS; i++) {
                    int gx, gy;
                    dp_orbit(i, &gx, &gy);
                    for (int pop = 0; pop <= 3; pop++) {
                        for (int phase = 0; phase < 3; phase++) {
                            const ag_bubble_t b = side(bc, left, y, gx, gy, pop, phase);
                            const irect_t box = ag_bubble_bbox(&b);
                            assert(rect_in_scene(b.body) && (pop == 0) == (b.n_frame == 0) && (pop == 0) == (box.x1 <= box.x0));
                            assert(pop == 0 || rect_in_box(b.body, box));
                            for (int k = 0; k < b.n_frame; k++) {
                                assert(rect_in_scene(b.frame[k].r) && rect_in_box(b.frame[k].r, box));
                                cells++;
                            }
                            for (int k = 0; k < b.n_cells; k++) {
                                assert(rect_in_scene(b.cells[k].r) && rect_in_box(b.cells[k].r, box));
                                cells++;
                            }
                            if (pop < 3) {
                                assert(b.n_cells == 0 && !b.spinner && b.n_dots == 0 && b.mask.w == 0);
                                continue;
                            }
                            assert(rect_in_box(b.mask, (irect_t){b.body.x, b.body.y, b.body.x + b.body.w, b.body.y + b.body.h}));
                            assert(rect_in_scene(b.mask) && rect_in_box(b.mask, box) && b.mask.w == bc->content.glyphs * 48 && b.mask.h == 48);
                            assert(b.spinner == bc->content.spinner && b.n_dots == (bc->content.dots ? 3 : 0));
                            const irect_t in_body = {b.body.x, b.body.y, b.body.x + b.body.w, b.body.y + b.body.h};
                            assert(!b.spinner || (rect_in_scene(b.spin) && rect_in_box(b.spin, box) && rect_in_box(b.spin, in_body)));
                            for (int k = 0; k < b.n_dots; k++) {
                                assert(rect_in_scene(b.dots[k]) && rect_in_box(b.dots[k], box) && rect_in_box(b.dots[k], in_body));
                            }
                            cells += 1 + b.spinner + b.n_dots;
                        }
                    }
                }
            }
        }
    }
    /* Red proof: the box of the body alone misses the tail cells. */
    const ag_bubble_t r = side(&BUB_CASES[0], false, 0, 0, 0, 3, 0);
    assert(!rect_in_box(r.cells[3].r, (irect_t){r.body.x, r.body.y, r.body.x + r.body.w, r.body.y + r.body.h}));
    assert(ag_bubble_bbox(&r).x0 == r.cells[3].r.x);
    printf("B1: %d cells inside the scene and the box\n", cells);
}

/* B2: the left side mirrors the right about the art centre, body and cells. */
static void test_b2_left_mirrors_right(void)
{
    for (int c = 0; c < 4; c++) {
        const bub_case_t *bc = &BUB_CASES[c];
        for (int y = -96; y <= 96; y += 4) {
            for (int i = 0; i < ORBIT_COLS * ORBIT_ROWS; i++) {
                int gx, gy;
                dp_orbit(i, &gx, &gy);
                const int cx = SCENE / 2 + gx + 2;
                for (int pop = 1; pop <= 3; pop++) {
                    const ag_bubble_t r = side(bc, false, y, gx, gy, pop, 0);
                    const ag_bubble_t l = side(bc, true, y, gx, gy, pop, 0);
                    assert(l.body.y == r.body.y && l.body.w == r.body.w && l.body.h == r.body.h);
                    assert(l.body.x - cx == -(r.body.x + r.body.w - cx));
                    assert(l.n_cells == r.n_cells && l.n_frame == r.n_frame);
                    for (int k = 0; k < r.n_frame; k++) {
                        assert(l.frame[k].ink == r.frame[k].ink && l.frame[k].r.y == r.frame[k].r.y && l.frame[k].r.h == r.frame[k].r.h);
                        assert(l.frame[k].r.x - cx == -(r.frame[k].r.x + r.frame[k].r.w - cx));
                    }
                    for (int k = 0; k < r.n_cells; k++) {
                        assert(l.cells[k].ink == r.cells[k].ink && l.cells[k].r.y == r.cells[k].r.y && l.cells[k].r.w == r.cells[k].r.w);
                        assert(l.cells[k].r.x - cx == -(r.cells[k].r.x + r.cells[k].r.w - cx));
                    }
                }
            }
        }
    }
    /* Red proof: dividing by the unit after adding the orbit moves a bubble 2 px, so its mirror differs. */
    const ag_bubble_t r = side(&BUB_CASES[0], false, 0, -12, -4, 3, 0);
    const int snapped = (SCENE / 2 + -12 + 188) / BUBBLE_UNIT_PX * BUBBLE_UNIT_PX;
    assert(snapped != r.body.x + r.body.w);
}

/* B3: bub_y rounds toward zero to a multiple of 4 and clamps to +-96; the outer edge follows the fit circle. */
static void test_b3_bub_y_snap_and_edge(void)
{
    assert(ag_bub_y_snap(-6) == -4 && ag_bub_y_snap(6) == 4 && ag_bub_y_snap(-96) == -96 && ag_bub_y_snap(96) == 96);
    assert(ag_bub_y_snap(-3) == 0 && ag_bub_y_snap(3) == 0 && ag_bub_y_snap(0) == 0 && ag_bub_y_snap(4) == 4 && ag_bub_y_snap(-4) == -4);
    assert(ag_bub_y_snap(-97) == -96 && ag_bub_y_snap(97) == 96 && ag_bub_y_snap(100) == 96 && ag_bub_y_snap(-200) == -96 && ag_bub_y_snap(10000) == 96);
    for (int y = -96; y <= 96; y++) {
        const int v = ag_bub_y_snap(y);
        assert(v % 4 == 0 && abs(v) <= abs(y) && abs(y) - abs(v) < 4);
    }
    static const int Y[9] = {-96, -72, -48, -24, 0, 24, 48, 72, 96};
    static const int EDGE[9] = {136, 156, 172, 184, 188, 184, 172, 156, 136};
    for (int k = 0; k < 9; k++) {
        assert(ag_bubble_edge(Y[k], 36, ORBIT_HALF_X_PX, ORBIT_HALF_Y_PX) == EDGE[k]);
    }
    /* A bubble requested at 6 sits at 4 and one at -6 at -4, rectangle included. */
    const ag_bubble_t a = side(&BUB_CASES[0], false, 6, 0, 0, 3, 0);
    const ag_bubble_t b = side(&BUB_CASES[0], false, 4, 0, 0, 3, 0);
    assert(a.body.y == b.body.y && a.body.x == b.body.x);
    assert(side(&BUB_CASES[0], false, 500, 0, 0, 3, 0).body.y == side(&BUB_CASES[0], false, 96, 0, 0, 3, 0).body.y);
    /* Red proof: a one-pixel looser radius moves the y 0 edge from 188 to 192. */
    assert(ag_bubble_edge(0, 36, ORBIT_HALF_X_PX - 4, ORBIT_HALF_Y_PX) == 192);
}

/* Reference copy of the ui.c draw_bubble rectangle formula. */
static ag_rect_t reference_top_bubble(int content, bool thought, int pop, int gx, int gy)
{
    const int u = BUBBLE_UNIT_PX, c = MASCOT_CELL_PX;
    int w = content + 2 * BUBBLE_PAD_UNITS + 2;
    w = (w + 1) & ~1;
    const int h = 18;
    int ws = pop == 3 ? w : ((w * pop / 3) + 1) & ~1, hs = pop == 3 ? h : ((h * pop / 3) + 1) & ~1;
    int below = thought ? 8 : 2;
    int anchor_top = SCENE / 2 - 5 * c + BUBBLE_SHIFT_UNITS * u + gy;
    int bottom_u = (anchor_top - BUBBLE_GAP_UNITS * u) / u - below;
    int cx = (SCENE / 2 + gx) / u;
    return (ag_rect_t){(cx - ws / 2) * u, (bottom_u - hs) * u, ws * u, hs * u};
}

/* B4: Clawd's top bubble equals the ui.c formula at 48 orbit points, 5 states and every pop step. */
static void test_b4_top_bubble_unchanged(void)
{
    static const struct {
        int content;
        bool thought;
    } STATES[5] = {{34, false}, {33, true}, {34, false}, {36, false}, {24, false}};
    int n = 0;
    for (int s = 0; s < 5; s++) {
        for (int i = 0; i < ORBIT_COLS * ORBIT_ROWS; i++) {
            int gx, gy;
            dp_orbit(i, &gx, &gy);
            for (int pop = 0; pop <= 3; pop++) {
                const ag_rect_t want = reference_top_bubble(STATES[s].content, STATES[s].thought, pop, gx, gy);
                const ag_top_bubble_t got = ag_top_bubble(ag_bubble_w_units(STATES[s].content), STATES[s].thought, pop, gx, gy, SCENE);
                assert(got.body.x == want.x && got.body.y == want.y && got.body.w == want.w && got.body.h == want.h);
                n++;
            }
        }
    }
    /* Pinned values: the 160 px LISTENING bubble at orbit (-12, -4) with the full pop. */
    const ag_top_bubble_t t = ag_top_bubble(40, false, 3, -12, -4, SCENE);
    assert(t.body.w == 160 && t.body.h == 72 && t.cx_u == (SCENE / 2 - 12) / 4 && t.body.x == (t.cx_u - 20) * 4);
    printf("B4: %d cases\n", n);
}

static void test_bring_follows_the_decode_rules(void)
{
    const ap_pack_t g = golden();
    uint8_t buf[192];
    memset(buf, 0xEE, sizeof buf);
    ag_held_t h = {-1, -1, 0, 0};
    const uint32_t a0 = (uint32_t)g.clip[0].w * g.clip[0].h;
    assert(ag_bring(&g, 0, 0, buf, &h) && !memcmp(buf, GOLDEN_CLIPS[0], a0) && h.frames == 1 && h.seeks == 0);
    assert(ag_bring(&g, 0, 1, buf, &h) && !memcmp(buf, GOLDEN_CLIPS[0] + a0, a0) && h.frames == 2);
    /* Held frame: nothing decoded, the buffer stays. */
    buf[0] ^= 0x55;
    assert(ag_bring(&g, 0, 1, buf, &h) && h.frames == 2);
    buf[0] ^= 0x55;
    /* Loop wrap and clip change start at key frame 0. */
    assert(ag_bring(&g, 0, 0, buf, &h) && !memcmp(buf, GOLDEN_CLIPS[0], a0) && h.seeks == 0);
    assert(ag_bring(&g, 1, 0, buf, &h) && !memcmp(buf, GOLDEN_CLIPS[1], 192) && h.clip == 1);
    /* A jump into the middle of a clip decodes forward from frame 0 and counts a seek. */
    memset(buf, 0xEE, sizeof buf);
    assert(ag_bring(&g, 0, 1, buf, &h) && !memcmp(buf, GOLDEN_CLIPS[0] + a0, a0) && h.seeks == 1);
    /* Red proof: applying only the wanted delta over another clip's buffer shows a wrong picture. */
    memcpy(buf, GOLDEN_CLIPS[1], 192);
    ap_frame_t f;
    assert(ap_frame(&g, 0, 1, &f) && ap_decode(&f, buf, a0) && memcmp(buf, GOLDEN_CLIPS[0] + a0, a0) != 0);
    /* A bad request leaves nothing held, so the next one decodes from frame 0. */
    assert(!ag_bring(&g, 0, 5, buf, &h) && h.clip == -1 && h.frame == -1);
    assert(!ag_bring(&g, 7, 0, buf, &h) && !ag_bring(NULL, 0, 0, buf, &h));
}

/* A wrong rule for the red proof: the stage cast as is, so the fade before display-off hides the layer. */
static aplay_stage_t stage_cast(bool panel_on, dp_stage_t st)
{
    return panel_on ? (aplay_stage_t)st : APS_OFF;
}

static void test_stage_map(void)
{
    assert(ag_stage(true, DP_ACTIVE, false) == APS_ACTIVE);
    assert(ag_stage(true, DP_DIM, false) == APS_DIM);
    assert(ag_stage(true, DP_OFF, false) == APS_DIM);
    assert(ag_stage(false, DP_OFF, false) == APS_OFF && ag_stage(false, DP_ACTIVE, true) == APS_OFF);
    assert(ag_stage(true, DP_DIM, true) == APS_ACTIVE && ag_stage(true, DP_OFF, true) == APS_ACTIVE);
    assert(stage_cast(true, DP_OFF) != ag_stage(true, DP_OFF, false));
}

enum { C_IDLE, C_TALK, C_ONCE, C_COUNT };
enum { RULE_EDGE, RULE_LEVEL, RULE_BRIGHTEN, RULE_PLAYER_FIRST };

static uint8_t g_tbl[C_COUNT][4 * AP_FRAME_ENTRY_BYTES];
static const uint8_t g_payload[1];
static ap_pack_t g_pack;

static const ap_pack_t *sim_pack(void)
{
    memset(&g_pack, 0, sizeof g_pack);
    g_pack.base = g_payload;
    g_pack.clip_count = C_COUNT;
    for (int k = 0; k < C_COUNT; k++) {
        ap_clip_t *c = &g_pack.clip[k];
        c->w = c->h = 1;
        c->frames = 4;
        c->colors = 1;
        c->loop = k != C_ONCE;
        c->frame_tbl = g_tbl[k];
        for (int i = 0; i < 4; i++) {
            const uint16_t meta = (uint16_t)(100 | (i == 0 ? AP_FRAME_KEY : 0));
            memset(g_tbl[k] + i * AP_FRAME_ENTRY_BYTES, 0, AP_FRAME_ENTRY_BYTES);
            g_tbl[k][i * AP_FRAME_ENTRY_BYTES + 12] = (uint8_t)meta;
            g_tbl[k][i * AP_FRAME_ENTRY_BYTES + 13] = (uint8_t)(meta >> 8);
        }
    }
    memset(g_pack.state_clip, AP_NO_CLIP, sizeof g_pack.state_clip);
    g_pack.state_clip[DP_STATE_IDLE] = C_IDLE;
    g_pack.state_clip[DP_STATE_LISTENING] = C_TALK;
    g_pack.pickup_clip = C_ONCE;
    return &g_pack;
}

typedef struct {
    int starts;         /* ticks where the once-clip begins */
    int with_wake;      /* of those, ticks that also brightened the screen */
    dp_stage_t before;  /* stage just before the lift */
} sim_t;

/* The UI's power and player ticks against the IMU task: a lift at lift_ms held for 3 s, in the given state. */
static sim_t simulate(int state, uint32_t lift_ms, int rule)
{
    motion_t m;
    motion_init(&m, NULL);
    dpower_t d;
    dp_init(&d, 0);
    dp_state(&d, 0, state);
    aplay_t a;
    aplay_init(&a, sim_pack());
    uint32_t word = 0;
    dp_stage_t shown = DP_ACTIVE;
    bool panel_on = true;
    sim_t r = {0, 0, DP_ACTIVE};
    aplay_out_t prev = {APLAY_CLAWD, 0};
    for (uint32_t t = 0; t < lift_ms + 3000; t += STEP) {
        if (t % IMU_PERIOD_MS == 0) {
            const bool lifted_now = t >= lift_ms;
            const float g[3] = {lifted_now ? 0.4f : 0.0f, 0.0f, lifted_now ? 0.92f : 1.0f};
            const int ev = motion_feed(&m, g, t, false);
            const bool lifted = m.state == MOTION_LIFTED;
            uint32_t bits = ag_imu_bits(ev, lifted);
            if (rule == RULE_LEVEL && lifted) {
                bits |= AG_PICKUP_BIT;
            }
            word |= bits;
        }
        if (t % UI_LEVEL_TIMER_MS != 0) {
            continue;
        }
        const uint32_t bits = word;
        word = 0;
        aplay_out_t o = {APLAY_CLAWD, 0};
        if (rule == RULE_PLAYER_FIRST) {
            o = aplay_tick(&a, t, state, ag_stage(panel_on, shown, ANIM_DIM_PLAYS));
        }
        for (int c = 1; c < DP_CAUSE_N; c++) {
            if (c != DP_CAUSE_SETDOWN && (bits & (1u << c))) {
                dp_activity(&d, t, (dp_cause_t)c);
            }
        }
        if (bits & (1u << DP_CAUSE_SETDOWN)) {
            dp_setdown(&d, t);
        }
        const dp_out_t out = dp_update(&d, t);
        const bool brightened = out.stage < shown;
        if (t + UI_LEVEL_TIMER_MS > lift_ms && t <= lift_ms) {
            r.before = shown;
        }
        const bool pickup = rule == RULE_BRIGHTEN ? brightened && out.stage == DP_ACTIVE && out.cause == DP_CAUSE_MOTION : (bits & AG_PICKUP_BIT) != 0;
        if (pickup) {
            aplay_pickup(&a);
        }
        shown = out.stage;
        panel_on = out.panel_on;
        if (rule != RULE_PLAYER_FIRST) {
            o = aplay_tick(&a, t, state, ag_stage(panel_on, shown, ANIM_DIM_PLAYS));
        }
        if (o.clip == C_ONCE && prev.clip != C_ONCE) {
            r.starts++;
            r.with_wake += brightened;
        }
        prev = o;
    }
    return r;
}

static void test_pickup_glue(void)
{
    const uint32_t lit = 5000, dim = DISP_HOLD_MS + 5000, off = DISP_HOLD_MS + DISP_DIM_TO_OFF_MS + 5000;
    sim_t r = simulate(DP_STATE_IDLE, lit, RULE_EDGE);
    assert(r.before == DP_ACTIVE && r.starts == 1 && r.with_wake == 0);
    r = simulate(DP_STATE_IDLE, dim, RULE_EDGE);
    assert(r.before == DP_DIM && r.starts == 1 && r.with_wake == 1);
    r = simulate(DP_STATE_IDLE, off, RULE_EDGE);
    assert(r.before == DP_OFF && r.starts == 1 && r.with_wake == 1);
    r = simulate(DP_STATE_LISTENING, lit, RULE_EDGE);
    assert(r.starts == 0);
    /* Red proofs: the held level replays; brightening alone misses a lit screen; the player before the power tick starts a tick after the wake. */
    assert(simulate(DP_STATE_IDLE, lit, RULE_LEVEL).starts > 1);
    assert(simulate(DP_STATE_IDLE, lit, RULE_BRIGHTEN).starts == 0);
    r = simulate(DP_STATE_IDLE, dim, RULE_PLAYER_FIRST);
    assert(r.starts == 1 && r.with_wake == 0);
}

static void test_imu_bits(void)
{
    assert(ag_imu_bits(0, false) == 0);
    assert(ag_imu_bits(0, true) == 1u << DP_CAUSE_MOTION);
    assert(ag_imu_bits(MOTION_EV_PICKUP, true) == ((1u << DP_CAUSE_MOTION) | AG_PICKUP_BIT));
    assert(ag_imu_bits(MOTION_EV_REST | MOTION_EV_SETDOWN, false) == 1u << DP_CAUSE_SETDOWN);
}

/* The longest log prefix, as ESP_LOGI writes it at the largest uptime. */
#define PREFIX "I (4294967295) app: "
#define W 4294967295u

static void test_log_lines_fit(void)
{
    char line[512];
    int n = snprintf(line, sizeof line, PREFIX AG_LINE_FMT, AP_VERIFYING, 1, APLAY_NOTHING, W, W, W, W, W, W, W, W);
    assert(n > 0 && n < LOG_LINE_MAX_BYTES);
    n = snprintf(line, sizeof line, PREFIX AG_TIME_LINE_FMT, W, W, W, W, W, W, W, W, W, W);
    assert(n > 0 && n < LOG_LINE_MAX_BYTES);
    printf("log lines: anim %d, animt %d of %d bytes\n", snprintf(line, sizeof line, PREFIX AG_LINE_FMT, AP_VERIFYING, 1, APLAY_NOTHING, W, W, W, W, W, W, W, W), n, LOG_LINE_MAX_BYTES);
    /* Red proof: the same fields on one line are cut. */
    n = snprintf(line, sizeof line, PREFIX "anim: pack=%d skin=%d clip=%d frames=%u dec_us=%u/%u work_us=%u/%u late=%u cuts=%u cut_ms=%u/%u seek=%u verify_ms=%u iheap=%u/%u ilarge=%u",
                 AP_VERIFYING, 1, APLAY_NOTHING, W, W, W, W, W, W, W, W, W, W, W, W, W, W);
    assert(n >= LOG_LINE_MAX_BYTES);
}

/* BF1: the first refresh is the boot frame, reported apart and out of the work maximum; the next frame is the first sample. */
static void test_bf1_boot_frame(void)
{
    ag_work_t w;
    memset(&w, 0, sizeof w);
    /* The boot decode and its idle gap (stage 1) is dropped at the first refresh, so the next sample starts clean. */
    assert(ag_work_refresh(&w, 1, 0, 32500) == 0);
    assert(w.armed && w.boot_us == 32500 && w.work.n == 0 && w.work.max == 0);
    assert(ag_work_refresh(&w, 0, 0, 17000) == 0 && w.work.n == 0);
    assert(ag_work_refresh(&w, 1, 0, 17000) == 1 && w.work.n == 0);
    assert(ag_work_refresh(&w, 2, 9000, 17000) == 0 && w.work.n == 1 && w.work.max == 26000 && w.work.sum == 26000);
    assert(ag_work_refresh(&w, 2, 13000, 17000) == 0 && w.work.n == 2 && w.work.max == 30000 && w.work.sum == 56000);
    assert(w.boot_us == 32500);
    /* A boot refresh that already holds a presented frame counts the tick part too; the rounding is to nearest. */
    memset(&w, 0, sizeof w);
    assert(ag_work_refresh(&w, 2, 64800, 17000) == 0);
    assert(w.boot_us == 81800 && ag_work_boot_ms(&w) == 82 && w.work.n == 0 && w.work.max == 0);
    assert(ag_work_boot_ms(&(ag_work_t){true, 17499, {0, 0, 0}}) == 17 && ag_work_boot_ms(&(ag_work_t){true, 17500, {0, 0, 0}}) == 18);
    assert(ag_work_boot_ms(&(ag_work_t){0}) == 0);
    /* Red proof: the same refresh on an already armed state is a sample and raises the maximum to 81.8 ms. */
    memset(&w, 0, sizeof w);
    w.armed = true;
    ag_work_refresh(&w, 2, 64800, 17000);
    assert(w.work.max == 81800 && w.boot_us == 0);
    /* The window mean over two reads, also across a wrap of the sum. */
    assert(ag_window_mean(1000, 10, 4000, 13) == 1000 && ag_window_mean(5, 5, 9, 5) == 0);
    assert(ag_window_mean(0xFFFFFF00u, 100, 0x100u + 0x400u, 102) == 0x300u);
    puts("BF1: boot frame apart, next frame first sample");
}

/* S1: LRU data cache of 128 sets x 8 ways x 32 B lines; counts misses of a walk down every column of a size x size byte buffer. */
static long column_walk_misses(int size)
{
    enum { SETS = 128, WAYS = 8, LINE = 32 };
    static long tag[SETS][WAYS], age[SETS][WAYS];
    for (int s = 0; s < SETS; s++) {
        for (int w = 0; w < WAYS; w++) {
            tag[s][w] = -1;
            age[s][w] = 0;
        }
    }
    long misses = 0, now = 0;
    for (int x = 0; x < size; x++) {
        for (int y = 0; y < size; y++) {
            const long line = ((long)y * size + x) / LINE;
            const int set = (int)(line % SETS);
            int hit = -1, victim = 0;
            for (int w = 0; w < WAYS; w++) {
                if (tag[set][w] == line) {
                    hit = w;
                }
                if (age[set][w] < age[set][victim]) {
                    victim = w;
                }
            }
            now++;
            if (hit < 0) {
                misses++;
                hit = victim;
                tag[set][hit] = line;
            }
            age[set][hit] = now;
        }
    }
    return misses;
}

static void test_scene_stride_suits_the_cache(void)
{
    const long ideal = ((long)SCENE * SCENE + 31) / 32;
    const long m = column_walk_misses(SCENE);
    printf("S1: size %d misses %ld ideal %ld\n", SCENE, m, ideal);
    assert(m * 2 <= ideal * 3);
    /* Red proof: at 384 the rows share 32 sets and every access misses. */
    assert(column_walk_misses(384) > 1.5 * (384L * 384 / 32));
}

int main(void)
{
    test_scene_stride_suits_the_cache();
    test_log_lines_fit();
    test_bf1_boot_frame();
    test_sticker_place();
    test_b1_side_bubble_table();
    test_b2_left_mirrors_right();
    test_b3_bub_y_snap_and_edge();
    test_b4_top_bubble_unchanged();
    test_bring_follows_the_decode_rules();
    test_stage_map();
    test_imu_bits();
    test_pickup_glue();
    puts("aglue_test ok");
    return 0;
}
