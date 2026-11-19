#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aglue.h"
#include "apack_golden.h"
#include "app_config.h"
#include "motion.h"

#define SCENE SCENE_SIZE_PX
#define GUARD 64
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

/* A wrong rule for the red proof: the pack follows the mascot's bob and scuttle offsets. */
static ag_place_t place_with_motion(const ap_pack_t *p, const ap_clip_t *c, int gx, int gy, int shift, int dx, int dy)
{
    return ag_place(p, c, gx + dx, gy + dy, shift, SCENE);
}

/* At every orbit point and bubble step, the canvas centre sits on the mascot centre and a valid crop is never clipped. */
static bool placed_well(const ap_pack_t *p, int dx, int dy)
{
    for (int k = 0; k < p->clip_count; k++) {
        const ap_clip_t *c = &p->clip[k];
        for (int pop = 0; pop <= 3; pop++) {
            const int shift = (BUBBLE_SHIFT_UNITS * pop / 3) * BUBBLE_UNIT_PX;
            for (int i = 0; i < ORBIT_COLS * ORBIT_ROWS; i++) {
                int gx, gy;
                dp_orbit(i, &gx, &gy);
                const ag_place_t pl = place_with_motion(p, c, gx, gy, shift, dx, dy);
                const int s = p->scale;
                /* Clawd's 16 x 10 cell body centre, as draw_mascot places it without dx and dy. */
                const int mx = SCENE / 2 - 8 * MASCOT_CELL_PX + gx + 8 * MASCOT_CELL_PX;
                const int my = SCENE / 2 - 5 * MASCOT_CELL_PX + gy + shift + 5 * MASCOT_CELL_PX;
                if (pl.ox - c->x * s + p->canvas_w * s / 2 != mx || pl.oy - c->y * s + p->canvas_h * s / 2 != my) {
                    return false;
                }
                if (pl.x0 != pl.ox || pl.y0 != pl.oy || pl.x1 != pl.ox + c->w * s || pl.y1 != pl.oy + c->h * s) {
                    return false;
                }
            }
        }
    }
    return true;
}

static void test_place_at_every_orbit_point(void)
{
    const ap_pack_t g = golden();
    assert(placed_well(&g, 0, 0));
    /* A 240 px canvas whose crop spans 92 px above and 96 px below the centre; reach is its farthest corner, 154 px. */
    const int top = 28, h = 188;
    const ap_pack_t full = canvas(240, 240, 1, 0, top, 240, h, 154);
    assert(placed_well(&full, 0, 0));
    const ap_pack_t half = canvas(120, 120, 2, 0, top / 2, 120, h / 2, 77);
    assert(placed_well(&half, 0, 0));
    /* Red proof: applying the mascot's one-unit bob moves the canvas off the mascot centre and, under a full bubble at the lowest orbit row, out of the scene. */
    assert(!placed_well(&g, 0, BUBBLE_UNIT_PX));
    assert(!placed_well(&full, 0, BUBBLE_UNIT_PX));
    /* A 320 px scene clips the lowest orbit row under a full bubble; the live scene is larger. */
    const int low = ORBIT_Y0_PX + (ORBIT_ROWS - 1) * ORBIT_PITCH_PX, sh = BUBBLE_SHIFT_UNITS * BUBBLE_UNIT_PX;
    const ag_place_t pl = ag_place(&full, &full.clip[0], 0, low, sh, 320);
    assert(pl.y1 == 320);
    const ag_place_t bob = ag_place(&full, &full.clip[0], 0, low + BUBBLE_UNIT_PX, sh, 320);
    assert(bob.oy + full.clip[0].h > 320 && bob.y1 == 320);
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
    /* Red proof: the old placement moves the same crop by the bubble shift and the raw lattice. */
    const ag_place_t old = ag_place(&big, &big.clip[0], -12, -4, BUBBLE_SHIFT_UNITS * BUBBLE_UNIT_PX, SCENE);
    assert(old.oy != ag_sticker_place(&big, &big.clip[0], -12, -4, SCENE).oy);
}

/* Copies into a guarded scene and checks every byte: crop pixels where placed, untouched elsewhere. */
static void check_copy(const ap_pack_t *p, const ap_clip_t *c, const uint8_t *buf, int gx, int gy)
{
    static uint8_t mem[GUARD + SCENE * SCENE + GUARD];
    memset(mem, 0xEE, sizeof mem);
    uint8_t *scene = mem + GUARD;
    const ag_place_t pl = ag_place(p, c, gx, gy, 0, SCENE);
    ag_copy(buf, c, p->scale, &pl, scene, SCENE);
    for (int i = 0; i < GUARD; i++) {
        assert(mem[i] == 0xEE && mem[GUARD + SCENE * SCENE + i] == 0xEE);
    }
    for (int y = 0; y < SCENE; y++) {
        for (int x = 0; x < SCENE; x++) {
            const bool in = x >= pl.x0 && x < pl.x1 && y >= pl.y0 && y < pl.y1;
            const uint8_t want = in ? buf[((y - pl.oy) / p->scale) * c->w + (x - pl.ox) / p->scale] : 0xEE;
            assert(scene[y * SCENE + x] == want);
        }
    }
}

static void test_copy_clips_to_the_scene(void)
{
    const ap_pack_t g = golden();
    const ap_clip_t *c1 = &g.clip[1];
    check_copy(&g, c1, APACK_GOLDEN_CLIP1, 0, 0);
    /* Moved half off each edge: the crop is clipped, nothing past the scene is written. */
    const int far = SCENE / 2 + 2;
    check_copy(&g, c1, APACK_GOLDEN_CLIP1, -far, -far);
    check_copy(&g, c1, APACK_GOLDEN_CLIP1, far, far);
    ag_place_t pl = ag_place(&g, c1, -far, 0, 0, SCENE);
    assert(pl.x0 == 0 && pl.ox < 0 && pl.x1 == pl.ox + c1->w);
    pl = ag_place(&g, c1, 4 * SCENE, 0, 0, SCENE);
    assert(pl.x0 >= pl.x1);
    check_copy(&g, c1, APACK_GOLDEN_CLIP1, 4 * SCENE, 0);
    /* Scale 2 widens each index to a 2 x 2 block, also when the clip edge cuts a block in half. */
    ap_pack_t two = canvas(8, 8, 2, 1, 2, 3, 2, 4);
    static const uint8_t px[6] = {1, 2, 3, 4, 5, 6};
    check_copy(&two, &two.clip[0], px, 0, 0);
    check_copy(&two, &two.clip[0], px, -SCENE / 2 + 5, 0);
    pl = ag_place(&two, &two.clip[0], -SCENE / 2 + 5, 0, 0, SCENE);
    assert(pl.ox == -1 && pl.x0 == 0 && pl.x1 == 5);
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
    n = snprintf(line, sizeof line, PREFIX AG_TIME_LINE_FMT, W, W, W, W, W, W);
    assert(n > 0 && n < LOG_LINE_MAX_BYTES);
    /* Red proof: the same fields on one line are cut. */
    n = snprintf(line, sizeof line, PREFIX "anim: pack=%d skin=%d clip=%d frames=%u dec_us=%u/%u work_us=%u/%u late=%u cuts=%u cut_ms=%u/%u seek=%u verify_ms=%u iheap=%u/%u ilarge=%u",
                 AP_VERIFYING, 1, APLAY_NOTHING, W, W, W, W, W, W, W, W, W, W, W, W, W, W);
    assert(n >= LOG_LINE_MAX_BYTES);
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
    test_place_at_every_orbit_point();
    test_sticker_place();
    test_copy_clips_to_the_scene();
    test_bring_follows_the_decode_rules();
    test_stage_map();
    test_imu_bits();
    test_pickup_glue();
    puts("aglue_test ok");
    return 0;
}
