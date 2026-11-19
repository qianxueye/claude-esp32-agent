#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "audio.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_heap_caps.h"
#include "dpower.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "core/lv_refr_private.h"
#include "acomp.h"
#include "aglue.h"
#include "arcvis.h"
#include "bright.h"
#include "brtmap.h"
#include "orient.h"
#include "proto.h"
#include "rotblit.h"
#include "scenepal.h"
#include "skin.h"
#include "transport.h"
#include "volarc.h"
#include "volknob.h"
#include "volume.h"

LV_FONT_DECLARE(clawd_font_48);

static const char *TAG = "ui";

#define U BUBBLE_UNIT_PX
#define C MASCOT_CELL_PX
#define SCENE SCENE_SIZE_PX
#define TEXT_ROWS_PX 48
#define CANVAS_OFF ((LCD_SIZE - SCENE_OUT_PX) / 2)
_Static_assert(CANVAS_OFF == 0, "the flush hook and the snapshot compose in panel coordinates");

typedef struct {
    int state, dx, dy, gx, gy, shift;
    int eye_mode, eye_dx, eye_row, eye_h;
    int claw_l, claw_r, legs_up, dim;
    int pop, bub, dot_phase, spin;
    int anim_clip, anim_frame;
} scene_t;

enum { EYES_OPEN, EYES_CLOSED, EYES_CROSS };

typedef struct {
    const char *text;
    int glyphs;
    bool thought, dots, spinner;
    uint8_t text_color, fill;
} bubble_def_t;

static const bubble_def_t BUBBLES[ST_COUNT] = {
    [ST_LISTENING] = {"在听", 2, false, true, false, PAL_INK, PAL_FILL},
    [ST_THINKING] = {"在想", 2, true, false, true, PAL_INK, PAL_FILL},
    [ST_SPEAKING] = {"在说", 2, false, true, false, PAL_INK, PAL_FILL},
    [ST_ERROR] = {"出错了", 3, false, false, false, PAL_ERROR, PAL_FILL},
    [ST_OFFLINE] = {"离线", 2, false, false, false, PAL_INK, PAL_OFFLINE_FILL},
};

#define SPINNER_N 6
#define SPINNER_PX AG_SPINNER_UNITS
static const char *const SPINNER[SPINNER_N][SPINNER_PX] = {
    {".......", ".......", ".......", "...#...", ".......", ".......", "......."},
    {"...#...", "...#...", "...#...", "###.###", "...#...", "...#...", "...#..."},
    {"#..#..#", ".#.#.#.", "..###..", "###.###", "..###..", ".#.#.#.", "#..#..#"},
    {"...#...", "..###..", ".#####.", "###.###", ".#####.", "..###..", "...#..."},
    {"..#.#..", "#.###.#", ".#####.", "###.###", ".#####.", "#.###.#", "..#.#.."},
    {"#..#..#", ".#.#.#.", "..###..", "#######", "..###..", ".#.#.#.", "#..#..#"},
};
static const int SPINNER_PINGPONG[] = {0, 1, 2, 3, 4, 5, 4, 3, 2, 1};

static const int LEG_COL[4] = {3, 5, 10, 12};

static lv_obj_t *s_canvas;
static lv_area_t s_inval[INVAL_LOG_MAX];
static int s_inval_n;
static SemaphoreHandle_t s_te_sem;
static volatile uint32_t s_te_edges;
static uint8_t *s_model;
static uint16_t s_pal[PAL_N], s_pal_be[PAL_N]; /* the swapped copy is what the flush hook writes */
static lv_draw_buf_t *s_out;
static uint8_t *s_mask[ST_COUNT];
static irect_t s_bb, s_prev_bb;
static bool s_model_dirty;
static uint32_t s_redraw_us, s_blit_us, s_redraw_n, s_blit_n, s_blit_px;
static dev_state_t s_state = ST_IDLE;
static uint32_t s_state_since, s_last_tick, s_pop_at;
static int s_pop, s_pop_target, s_bub_state;
static scene_t s_drawn;
static volknob_t s_vk;
static arcvis_t s_av;
static volarc_t s_arc, s_barc, s_over[2];
static uint16_t s_arc_col[3], s_barc_col[3];
static int s_bench, s_ring_width, s_ring_opa;
static char s_miss[150];
static uint32_t s_te_missed, s_cover_frames, s_cover_viol;
static float s_bench_angle;
static int s_shown_rot = -1;
static volatile uint32_t s_refr_count, s_refr_worst_ms, s_refr_start, s_refr_sum_ms;
static dpower_t s_dp;
static esp_lcd_panel_handle_t s_panel;
static volatile uint32_t s_wake_bits;
static volatile bool s_panel_on = true;
static int s_applied_level = DISP_ACTIVE_PERCENT, s_time_scale = 1, s_orbit_force = -1, s_fill = -1;
static dp_stage_t s_logged_stage = DP_ACTIVE;
static dp_touch_t s_touch;
static volatile uint32_t s_wakes[DP_CAUSE_N];
static const char *const STAGE_NAME[] = {"active", "dim", "off"};
static const char *const CAUSE_NAME[DP_CAUSE_N] = {"none", "boot", "touch", "button", "state", "motion", "timer", "force", "setdown"};
static aplay_t s_ap;
static bool s_ap_ready, s_skin_on = true;
static int s_anim_pin = -1, s_pal_clip = -1;
static aplay_out_t s_anim = {APLAY_NOTHING, 0};
static uint8_t *s_abuf;
static ag_held_t s_held = {-1, -1, 0, 0};
static volatile uint32_t s_dec_sum, s_dec_max, s_work_sum, s_work_max, s_cut_last, s_cut_max;
static int64_t s_work_t0, s_work_tick_us, s_refr_t0_us, s_cut_t0;
static int s_work_stage; /* 1: frame decoded, 2: presented, waiting for its flush */
static bool s_cut_pending;
static acomp_art_t s_art;
static acomp_state_t s_ac;
static bool s_art_ready, s_art_wanted_last;
static int s_tick_bands;

static uint16_t rgb565(uint32_t hex)
{
    return (uint16_t)(((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x1F));
}

static uint32_t scale_hex(uint32_t hex, int percent)
{
    uint32_t r = ((hex >> 16) & 0xFF) * percent / 100, g = ((hex >> 8) & 0xFF) * percent / 100, b = (hex & 0xFF) * percent / 100;
    return (r << 16) | (g << 8) | b;
}

static void grow_bb(int x0, int y0, int x1, int y1)
{
    s_bb.x0 = x0 < s_bb.x0 ? x0 : s_bb.x0;
    s_bb.y0 = y0 < s_bb.y0 ? y0 : s_bb.y0;
    s_bb.x1 = x1 > s_bb.x1 ? x1 : s_bb.x1;
    s_bb.y1 = y1 > s_bb.y1 ? y1 : s_bb.y1;
}

static void fill(int x, int y, int w, int h, uint8_t color)
{
    int x1 = x + w, y1 = y + h;
    x = x < 0 ? 0 : x;
    y = y < 0 ? 0 : y;
    x1 = x1 > SCENE ? SCENE : x1;
    y1 = y1 > SCENE ? SCENE : y1;
    if (x >= x1 || y >= y1) {
        return;
    }
    grow_bb(x, y, x1, y1);
    for (int j = y; j < y1; j++) {
        memset(s_model + (size_t)j * SCENE + x, color, (size_t)(x1 - x));
    }
}

static void fill_u(int ux, int uy, int uw, int uh, uint8_t color)
{
    fill(ux * U, uy * U, uw * U, uh * U, color);
}

static int pingpong(uint32_t t, uint32_t step, int count)
{
    return (int)((t / step) % (uint32_t)count);
}

static void draw_eyes(const scene_t *s, int ox, int oy, uint8_t black)
{
    const int cols[2] = {4, 11};
    for (int i = 0; i < 2; i++) {
        int x = ox + (cols[i] + s->eye_dx) * C;
        if (s->eye_mode == EYES_OPEN) {
            fill(x, oy + s->eye_row * C, C, s->eye_h * C, black);
        } else if (s->eye_mode == EYES_CLOSED) {
            fill(x, oy + 3 * C, C, C, black);
        } else {
            int top = oy + 2 * C + 2 * U;
            for (int k = 0; k < 4; k++) {
                fill(x + k * U, top + k * U, U, U, black);
                fill(x + (3 - k) * U, top + k * U, U, U, black);
            }
        }
    }
}

static void draw_mascot(const scene_t *s)
{
    int ox = SCENE / 2 - 8 * C + s->gx + s->dx, oy = SCENE / 2 - 5 * C + s->gy + s->shift + s->dy;
    const uint8_t col = PAL_MASCOT;
    s_pal[PAL_MASCOT] = rgb565(scale_hex(MASCOT_COLOR, s->dim));
    fill(ox + 2 * C, oy, 12 * C, 8 * C, col);
    fill(ox, oy + (4 + s->claw_l) * C, 2 * C, 2 * C, col);
    fill(ox + 14 * C, oy + (4 + s->claw_r) * C, 2 * C, 2 * C, col);
    for (int i = 0; i < 4; i++) {
        fill(ox + LEG_COL[i] * C, oy + 8 * C, C, ((s->legs_up >> i) & 1) ? C : 2 * C, col);
    }
    draw_eyes(s, ox, oy, PAL_BG);
}

static void draw_sprite(int x, int y, const char *const rows[SPINNER_PX], uint8_t color)
{
    for (int r = 0; r < SPINNER_PX; r++) {
        for (int c = 0; c < SPINNER_PX; c++) {
            if (rows[r][c] == '#') {
                fill(x + c * U, y + r * U, U, U, color);
            }
        }
    }
}

static ag_content_t bubble_content(const bubble_def_t *b)
{
    return (ag_content_t){b->glyphs, b->spinner, b->dots};
}

static void draw_bubble_frame(int x0, int y0, int w, int h, uint8_t ink, uint8_t fill_c)
{
    fill_u(x0 + 1, y0, w - 2, h, ink);
    fill_u(x0, y0 + 1, w, h - 2, ink);
    fill_u(x0 + 2, y0 + 1, w - 4, h - 2, fill_c);
    fill_u(x0 + 1, y0 + 2, w - 2, h - 4, fill_c);
}

static void draw_mask(const bubble_def_t *b, int st, int ox, int oy)
{
    const uint8_t *mask = s_mask[st];
    const int mw = 12 * b->glyphs * U;
    for (int j = 0; j < TEXT_ROWS_PX; j++) {
        for (int i = 0; i < mw; i++) {
            if (mask[j * mw + i]) {
                s_model[(size_t)(oy + j) * SCENE + ox + i] = b->text_color;
            }
        }
    }
    grow_bb(ox, oy, ox + mw, oy + TEXT_ROWS_PX);
}

/* The sticker skin's bubble beside the art, every cell from ag_side_bubble placed to the pixel. */
static void draw_side_bubble(const scene_t *s, const ap_clip_t *clip)
{
    const bubble_def_t *b = &BUBBLES[s->bub];
    const ag_bubble_t g = ag_side_bubble(bubble_content(b), b->thought, clip->bubble_left, clip->bubble_y, s->gx, s->gy, s->pop, s->dot_phase, SCENE);
    const irect_t box = ag_bubble_bbox(&g);
    grow_bb(box.x0, box.y0, box.x1, box.y1);
    for (int i = 0; i < g.n_frame; i++) {
        fill(g.frame[i].r.x, g.frame[i].r.y, g.frame[i].r.w, g.frame[i].r.h, g.frame[i].ink ? PAL_INK : b->fill);
    }
    for (int i = 0; i < g.n_cells; i++) {
        fill(g.cells[i].r.x, g.cells[i].r.y, g.cells[i].r.w, g.cells[i].r.h, g.cells[i].ink ? PAL_INK : b->fill);
    }
    if (g.spinner) {
        draw_sprite(g.spin.x, g.spin.y, SPINNER[s->spin], PAL_INK);
    }
    for (int i = 0; i < g.n_dots; i++) {
        fill(g.dots[i].x, g.dots[i].y, g.dots[i].w, g.dots[i].h, PAL_INK);
    }
    if (g.mask.w > 0) {
        draw_mask(b, s->bub, g.mask.x, g.mask.y);
    }
}

static void draw_top_bubble(const scene_t *s)
{
    const bubble_def_t *b = &BUBBLES[s->bub];
    const int content = ag_content_units(bubble_content(b)), w = ag_bubble_w_units(content);
    const ag_top_bubble_t t = ag_top_bubble(w, b->thought, s->pop, s->gx, s->gy, SCENE);
    const int cx = t.cx_u, bottom_u = t.bottom_u, x0 = t.body.x / U, y0 = t.body.y / U, ws = t.body.w / U, hs = t.body.h / U;
    const uint8_t ink = PAL_INK, fillc = b->fill;
    draw_bubble_frame(x0, y0, ws, hs, ink, fillc);
    if (s->pop < 3) {
        return;
    }
    if (b->thought) {
        fill_u(cx - 2, bottom_u + 1, 4, 4, fillc);
        fill_u(cx - 1, bottom_u + 6, 2, 2, fillc);
    } else {
        fill_u(cx - 2, bottom_u - 1, 4, 1, fillc);
        fill_u(cx - 2, bottom_u, 4, 1, ink);
        fill_u(cx - 1, bottom_u, 2, 1, fillc);
        fill_u(cx - 1, bottom_u + 1, 2, 1, ink);
    }
    const int left = x0 + 1 + BUBBLE_PAD_UNITS + (w - 2 - 2 * BUBBLE_PAD_UNITS - content) / 2;
    const int text_y = y0 + 3;
    int tx = left;
    if (b->spinner) {
        draw_sprite(left * U, (text_y + 2) * U, SPINNER[s->spin], ink);
        tx += SPINNER_PX + 2;
    }
    if (b->dots) {
        const int dx = tx + 12 * b->glyphs + 2;
        for (int i = 0; i < 3; i++) {
            const int size = i == s->dot_phase ? 2 : 1;
            fill_u(dx + i * 3, text_y + 11 - size, size, size, ink);
        }
    }
    draw_mask(b, s->bub, tx * U, text_y * U);
}

static void draw_bubble(const scene_t *s)
{
    if (s->pop == 0 || !BUBBLES[s->bub].text) {
        return;
    }
    if (s_art_ready) {
        draw_side_bubble(s, &s_ap.pack->clip[s->anim_clip]);
    } else {
        draw_top_bubble(s);
    }
}

static void render_masks(lv_obj_t *scr)
{
    for (int st = 0; st < ST_COUNT; st++) {
        const bubble_def_t *b = &BUBBLES[st];
        if (!b->text) {
            continue;
        }
        const int w = 12 * b->glyphs * U;
        lv_draw_buf_t *tmp = lv_draw_buf_create(w, TEXT_ROWS_PX, LV_COLOR_FORMAT_RGB565, 0);
        ESP_ERROR_CHECK(tmp ? ESP_OK : ESP_ERR_NO_MEM);
        lv_draw_buf_clear(tmp, NULL);
        lv_obj_t *cv = lv_canvas_create(scr);
        lv_canvas_set_draw_buf(cv, tmp);
        lv_layer_t layer;
        lv_canvas_init_layer(cv, &layer);
        lv_draw_label_dsc_t dsc;
        lv_draw_label_dsc_init(&dsc);
        dsc.font = &clawd_font_48;
        dsc.color = lv_color_white();
        dsc.text = b->text;
        dsc.text_static = true;
        lv_area_t a = {0, 0, w - 1, TEXT_ROWS_PX - 1};
        lv_draw_label(&layer, &dsc, &a);
        lv_canvas_finish_layer(cv, &layer);
        s_mask[st] = heap_caps_malloc((size_t)w * TEXT_ROWS_PX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_ERROR_CHECK(s_mask[st] ? ESP_OK : ESP_ERR_NO_MEM);
        for (int j = 0; j < TEXT_ROWS_PX; j++) {
            const uint16_t *row = (const uint16_t *)((const uint8_t *)tmp->data + (size_t)j * tmp->header.stride);
            for (int i = 0; i < w; i++) {
                s_mask[st][j * w + i] = row[i] != 0;
            }
        }
        lv_obj_delete(cv);
        lv_draw_buf_destroy(tmp);
    }
}

static void anim_reset_held(void)
{
    s_held.clip = s_held.frame = -1;
    s_pal_clip = -1;
}

/* Decodes the frame if the buffer holds another, loads the clip's palette on a change, and describes the art for present(). */
static void draw_anim(const scene_t *s)
{
    const ap_pack_t *p = s_ap.pack;
    const ap_clip_t *c = &p->clip[s->anim_clip];
    if (s_held.clip != s->anim_clip || s_held.frame != s->anim_frame) {
        const int64_t t0 = esp_timer_get_time();
        if (!ag_bring(p, s->anim_clip, s->anim_frame, s_abuf, &s_held)) {
            skin_fail(AP_DECODE_ERROR);
            aplay_init(&s_ap, NULL);
            anim_reset_held();
            return;
        }
        const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        s_dec_sum += us;
        s_dec_max = us > s_dec_max ? us : s_dec_max;
        s_work_t0 = t0;
        s_work_stage = 1;
    }
    if (s_pal_clip != s->anim_clip) {
        for (int i = 0; i < c->colors; i++) {
            s_pal[PAL_PACK_FIRST + i] = (uint16_t)(c->palette[2 * i] | (c->palette[2 * i + 1] << 8));
            s_pal_be[PAL_PACK_FIRST + i] = __builtin_bswap16(s_pal[PAL_PACK_FIRST + i]);
        }
        s_pal_clip = s->anim_clip;
    }
    const ag_place_t pl = ag_sticker_place(p, c, s->gx, s->gy, SCENE);
    s_art = (acomp_art_t){s_abuf, c->w, c->h, c->w, p->scale, pl.ox, pl.oy, 0, SCENE, SCENE_OUT_PX};
    s_art_ready = true;
}

/* Whether the flush hook and the snapshot draw the art now. */
static bool art_wanted(void)
{
    return s_art_ready && s_fill < 0 && s_panel_on;
}

static void compute_scene(scene_t *s, uint32_t now)
{
    memset(s, 0, sizeof(*s));
    uint32_t t = now - s_state_since;
    s->state = s_state;
    s->eye_row = 2;
    s->eye_h = 2;
    s->dim = 100;
    s->bub = s_bub_state;
    s->pop = s_pop;
    s->shift = (BUBBLE_SHIFT_UNITS * s_pop / 3) * U;
    if (s_pop == 3 && BUBBLES[s_bub_state].dots) {
        s->dot_phase = (int)((t / BUBBLE_DOT_PULSE_MS) % 3);
    }
    if (s_pop == 3 && BUBBLES[s_bub_state].spinner) {
        s->spin = SPINNER_PINGPONG[pingpong(t, SPINNER_STEP_MS, 10)];
    }
    dp_orbit(s_orbit_force >= 0 ? s_orbit_force : (int)((now / ORBIT_DWELL_MS) % (ORBIT_COLS * ORBIT_ROWS)), &s->gx, &s->gy);
    s->anim_clip = s_anim.clip;
    s->anim_frame = s_anim.frame;
    if (s_anim.clip != APLAY_CLAWD) {
        s->shift = 0;
        return;
    }
    switch (s_state) {
    case ST_IDLE: {
        s->dy += (t % IDLE_BOB_PERIOD_MS) >= IDLE_BOB_PERIOD_MS / 2 ? U : 0;
        if (t % IDLE_BLINK_PERIOD_MS >= IDLE_BLINK_PERIOD_MS - IDLE_BLINK_MS) {
            s->eye_mode = EYES_CLOSED;
        }
        uint32_t gp = t % IDLE_GLANCE_PERIOD_MS;
        if (gp >= IDLE_GLANCE_PERIOD_MS - IDLE_GLANCE_MS) {
            s->eye_dx = ((t / IDLE_GLANCE_PERIOD_MS) & 1) ? 1 : -1;
        }
        const uint32_t total = 2 * SCUTTLE_DISTANCE_UNITS * SCUTTLE_STEP_MS + SCUTTLE_HOLD_MS;
        uint32_t sp = t % IDLE_SCUTTLE_PERIOD_MS;
        if (sp >= IDLE_SCUTTLE_PERIOD_MS - total) {
            uint32_t k = sp - (IDLE_SCUTTLE_PERIOD_MS - total);
            int out = SCUTTLE_DISTANCE_UNITS * SCUTTLE_STEP_MS, steps;
            if (k < (uint32_t)out) {
                steps = (int)(k / SCUTTLE_STEP_MS) + 1;
            } else if (k < (uint32_t)out + SCUTTLE_HOLD_MS) {
                steps = SCUTTLE_DISTANCE_UNITS;
            } else {
                steps = SCUTTLE_DISTANCE_UNITS - (int)((k - out - SCUTTLE_HOLD_MS) / SCUTTLE_STEP_MS) - 1;
            }
            bool moving = k < (uint32_t)out || k >= (uint32_t)out + SCUTTLE_HOLD_MS;
            int dir = ((t / IDLE_SCUTTLE_PERIOD_MS) & 1) ? -1 : 1;
            s->dx += dir * steps * U;
            if (moving) {
                s->legs_up = (steps & 1) ? 0x6 : 0x9;
            }
        }
        break;
    }
    case ST_LISTENING:
        s->claw_l = s->claw_r = -1;
        s->eye_row = 1;
        s->eye_h = 3;
        break;
    case ST_THINKING:
        s->eye_row = 1;
        s->legs_up = ((t / THINK_LEG_STEP_MS) & 1) ? 0x6 : 0x9;
        break;
    case ST_SPEAKING: {
        int lvl = audio_spk_level();
        s->dy -= (lvl * SPEAK_BOUNCE_MAX_UNITS / 1000) * U;
        if (lvl >= SPEAK_CLAW_LEVEL_MIN) {
            bool left = (t / SPEAK_CLAW_STEP_MS) & 1;
            s->claw_l = left ? -1 : 0;
            s->claw_r = left ? 0 : -1;
        }
        break;
    }
    case ST_ERROR:
        s->claw_l = s->claw_r = 1;
        if (t < ERROR_CROSS_MS) {
            s->eye_mode = EYES_CROSS;
        }
        if (t < ERROR_SHAKE_MS) {
            s->dx += ((t / ERROR_SHAKE_STEP_MS) & 1) ? U : -U;
        }
        break;
    case ST_OFFLINE:
        s->dim = MASCOT_DIM_PERCENT;
        s->eye_mode = EYES_CLOSED;
        s->dy += (t % (2 * IDLE_BOB_PERIOD_MS)) >= IDLE_BOB_PERIOD_MS ? U : 0;
        break;
    default:
        break;
    }
}

static void redraw(const scene_t *s)
{
    int64_t t0 = esp_timer_get_time();
    if (s_prev_bb.x1 > s_prev_bb.x0) {
        fill(s_prev_bb.x0, s_prev_bb.y0, s_prev_bb.x1 - s_prev_bb.x0, s_prev_bb.y1 - s_prev_bb.y0, PAL_BG);
    }
    s_bb = (irect_t){SCENE, SCENE, 0, 0};
    s_art_ready = false;
    if (s->anim_clip == APLAY_CLAWD) {
        draw_mascot(s);
    } else if (s->anim_clip >= 0) {
        draw_anim(s);
    }
    draw_bubble(s);
    s_prev_bb = s_bb;
    s_model_dirty = true;
    s_redraw_us += (uint32_t)(esp_timer_get_time() - t0);
    s_redraw_n++;
}

typedef struct {
    bool patch;               /* a flush-chunk patch, else a scene blit */
    int tenths, y0, y1;       /* blit rows, or patch rows */
    uint16_t *dst;            /* patch: pixel (x0, y0) of the chunk */
    int stride, x0, x1;       /* patch: chunk width and columns */
} blit_job_t;

static blit_job_t s_job;
static TaskHandle_t s_blit_task;
static SemaphoreHandle_t s_blit_done;
static xspan_t s_span_new[SCENE_OUT_PX], s_span_prev[SCENE_OUT_PX], s_span_clear[SCENE_OUT_PX];

static void run_patch(const blit_job_t *j)
{
    acomp_rows(&s_art, s_ac.art, s_pal_be, j->dst, j->stride, j->x0, j->y0, j->x1, j->y1);
}

static void run_blit(int tenths, int y0, int y1)
{
    rotblit(s_model, SCENE, s_pal, (uint16_t *)s_out->data, s_out->header.stride / 2, SCENE_OUT_PX, tenths, s_span_new,
            s_span_clear, y0, y1);
}

static void blit_worker(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_job.patch) {
            run_patch(&s_job);
        } else {
            run_blit(s_job.tenths, s_job.y0, s_job.y1);
        }
        xSemaphoreGive(s_blit_done);
    }
}

static bool span_empty(xspan_t s)
{
    return s.x0 >= s.x1;
}

static void present(int tenths)
{
    int64_t t0 = esp_timer_get_time();
    irect_t margin = {s_bb.x0 - 1, s_bb.y0 - 1, s_bb.x1 + 1, s_bb.y1 + 1};
    margin.x0 = margin.x0 < 0 ? 0 : margin.x0;
    margin.y0 = margin.y0 < 0 ? 0 : margin.y0;
    margin.x1 = margin.x1 > SCENE ? SCENE : margin.x1;
    margin.y1 = margin.y1 > SCENE ? SCENE : margin.y1;
    rotblit_spans(&margin, SCENE, SCENE_OUT_PX, tenths, s_span_new);
    int ymin = SCENE_OUT_PX, ymax = 0;
    for (int y = 0; y < SCENE_OUT_PX; y++) {
        xspan_t n = s_span_new[y], p = s_span_prev[y];
        xspan_t u = span_empty(n) ? p : span_empty(p) ? n : (xspan_t){n.x0 < p.x0 ? n.x0 : p.x0, n.x1 > p.x1 ? n.x1 : p.x1};
        s_span_clear[y] = u;
        if (!span_empty(u)) {
            ymin = y < ymin ? y : ymin;
            ymax = y + 1;
        }
    }
    if (ymin < ymax) {
        int mid = (ymin + ymax) / 2;
        s_job = (blit_job_t){.tenths = tenths, .y0 = mid, .y1 = ymax};
        xTaskNotifyGive(s_blit_task);
        run_blit(tenths, ymin, mid);
        xSemaphoreTake(s_blit_done, portMAX_DELAY);
    }
    uint32_t px = 0;
    s_inval_n = 0;
    s_art.tenths = tenths;
    s_art_wanted_last = art_wanted();
    s_tick_bands = acomp_step(&s_ac, s_art_wanted_last, &s_art, s_span_clear);
    lv_display_t *disp = lv_obj_get_display(s_canvas);
    const int shrink = s_bench == UI_BENCH_SHRINK ? 8 : 0;
    const bool overflow = s_tick_bands > ACOMP_MAX_BANDS;
    if (overflow) {
        lv_area_t all = {CANVAS_OFF, CANVAS_OFF, CANVAS_OFF + SCENE_OUT_PX - 1, CANVAS_OFF + SCENE_OUT_PX - 1};
        lv_inv_area(disp, &all);
        px = SCENE_OUT_PX * SCENE_OUT_PX;
    }
    for (int i = 0; i < s_tick_bands && !overflow; i++) {
        const irect_t b = s_ac.bands[i];
        lv_area_t inv = {b.x0 + shrink + CANVAS_OFF, b.y0 + CANVAS_OFF, b.x1 - 1 - shrink + CANVAS_OFF, b.y1 - 1 + CANVAS_OFF};
        lv_inv_area(disp, &inv);
        px += (uint32_t)((b.x1 - b.x0) * (b.y1 - b.y0));
    }
    const int off = CANVAS_OFF;
    s_cover_frames++;
    for (int y = ymin; y < ymax; y++) {
        xspan_t sp = s_span_clear[y];
        if (span_empty(sp)) {
            continue;
        }
        bool covered = false;
        for (int i = 0; i < s_inval_n && !covered; i++) {
            covered = s_inval[i].y1 <= y + off && s_inval[i].y2 >= y + off && s_inval[i].x1 <= sp.x0 + off &&
                      s_inval[i].x2 >= sp.x1 - 1 + off;
        }
        if (!covered) {
            int len = snprintf(s_miss, sizeof(s_miss), "r%d s%d-%d:", y, sp.x0, sp.x1);
            for (int i = 0; i < s_inval_n && len < (int)sizeof(s_miss) - 24; i++) {
                {
                    len += snprintf(s_miss + len, sizeof(s_miss) - len, "%d,%d,%d,%d;", (int)s_inval[i].x1, (int)s_inval[i].y1,
                                    (int)s_inval[i].x2, (int)s_inval[i].y2);
                }
            }
            s_cover_viol++;
            break;
        }
    }
    memcpy(s_span_prev, s_span_new, sizeof(s_span_new));
    s_blit_us += (uint32_t)(esp_timer_get_time() - t0);
    s_blit_n++;
    s_blit_px += px;
}

static bool bench_spins(void)
{
    return s_bench == UI_BENCH_SPIN || s_bench == UI_BENCH_SHRINK;
}

/* Composes the art into a packed, byte-swapped flush chunk, the rows split over both cores unless the bench forces one. */
static void patch_chunk(uint16_t *chunk, int x0, int y0, int x1, int y1)
{
    int ya, mid, yb;
    if (!acomp_split(s_ac.art, y0, y1, &ya, &mid, &yb)) {
        return;
    }
    const int w = x1 - x0;
    uint16_t *top = chunk + (size_t)(ya - y0) * w;
    if (s_bench == UI_BENCH_SERIAL || yb - ya < 2) {
        run_patch(&(blit_job_t){.y0 = ya, .y1 = yb, .dst = top, .stride = w, .x0 = x0, .x1 = x1});
        return;
    }
    s_job = (blit_job_t){.patch = true, .y0 = mid, .y1 = yb, .dst = top + (size_t)(mid - ya) * w, .stride = w, .x0 = x0, .x1 = x1};
    xTaskNotifyGive(s_blit_task);
    run_patch(&(blit_job_t){.y0 = ya, .y1 = mid, .dst = top, .stride = w, .x0 = x0, .x1 = x1});
    xSemaphoreTake(s_blit_done, portMAX_DELAY);
}

static esp_err_t flush_hook(lv_display_t *disp, esp_lcd_panel_handle_t panel, int x0, int y0, int x1, int y1, const void *map, void *ctx)
{
    if (art_wanted()) {
        patch_chunk((uint16_t *)map, x0 - CANVAS_OFF, y0 - CANVAS_OFF, x1 - CANVAS_OFF, y1 - CANVAS_OFF);
    }
    return esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, map);
}

static void apply_rotation(float deg)
{
    int tenths = ((int)lroundf(deg * 10.0f)) % 3600;
    if (tenths == s_shown_rot && !s_model_dirty) {
        return;
    }
    s_shown_rot = tenths;
    s_model_dirty = false;
    present(tenths);
}

static uint32_t dp_clock(void)
{
    return now_ms() * (uint32_t)s_time_scale;
}

static void ui_set_state_locked(dev_state_t s)
{
    if (s != s_state) {
        s_cut_t0 = esp_timer_get_time();
    }
    s_state = s;
    s_state_since = lv_tick_get();
    dp_state(&s_dp, dp_clock(), (int)s);
    if (BUBBLES[s].text) {
        s_bub_state = s;
        s_pop_target = 3;
    } else {
        s_pop_target = 0;
    }
}

/* (Re)starts the power timeline at the current brightness. */
static void dp_start(void)
{
    dp_init(&s_dp, dp_clock());
    dp_set_active(&s_dp, bright_effective());
}

static void knob_events(int ev, uint32_t now)
{
    if (ev & VK_EV_ARM) {
        vk_rebase(&s_vk, volume_knob_arm());
        av_arm(&s_av, s_vk.v);
    }
    if (ev & VK_EV_VOL) {
        volume_knob_set(s_vk.v);
        av_set(&s_av, s_vk.v);
    }
    if (ev & VK_EV_BRT_ARM) {
        vk_swipe_rebase(&s_vk, brt_index(bright_swipe_arm(s_vk.cls == VK_EDGE)));
        dp_set_active(&s_dp, bright_effective());
        av_arm_kind(&s_av, AV_BRT, s_vk.bi, s_vk.quad);
    }
    if (ev & VK_EV_BRT) {
        bright_swipe_set(brt_level(s_vk.bi));
        dp_set_active(&s_dp, bright_effective());
        av_set(&s_av, s_vk.bi);
    }
    if (ev & VK_EV_BRT_STOP) {
        bright_count_stop();
        if (BRT_BOUNCE_STEPS > 0) {
            dp_bump(&s_dp, dp_clock(), brt_level(s_vk.bi == BRT_STEPS ? s_vk.bi - BRT_BOUNCE_STEPS : s_vk.bi + BRT_BOUNCE_STEPS));
        }
    }
    if (ev & VK_EV_OVER) {
        av_over(&s_av, s_vk.over);
        int cells = s_vk.over < 0 ? -s_vk.over : s_vk.over;
        if (s_vk.state == VK_BRIGHT) {
            bright_count_over(cells);
        } else if (s_vk.state == VK_ARMED) {
            volume_count_over(cells);
        }
    }
    if (ev & (VK_EV_ARM | VK_EV_VOL | VK_EV_BRT_ARM | VK_EV_BRT | VK_EV_BRT_STOP | VK_EV_OVER)) {
        dp_activity(&s_dp, dp_clock(), DP_CAUSE_TOUCH);
    }
    if (ev & (VK_EV_COMMIT | VK_EV_CANCEL)) {
        volume_knob_end((ev & VK_EV_CANCEL) != 0);
        av_end(&s_av, s_vk.v, now);
    }
    if (ev & (VK_EV_BRT_COMMIT | VK_EV_BRT_CANCEL)) {
        bright_swipe_end((ev & VK_EV_BRT_CANCEL) != 0);
        dp_set_active(&s_dp, bright_effective());
        av_end(&s_av, s_vk.bi, now);
    }
}

static void log_knob_end(uint32_t now)
{
    ESP_LOGI(TAG, "knob: end cls=%s armed=%d why=%s v0=%d v=%d travel=%.0f disp=%.0f x0=%d y0=%d r=%.0f dur=%u", vk_cls_name(s_vk.cls), s_vk.armed_ever,
             vk_why_name(s_vk.why), s_vk.v0, s_vk.v, s_vk.travel, s_vk.max_disp, s_vk.x0, s_vk.y0, s_vk.r0, (unsigned)(now - s_vk.t0));
}

/* One line per touch that left the slop; b0 and b are percent, -1 when the swipe never armed. */
static void log_swipe_end(uint32_t now)
{
    if (!s_vk.moved) {
        return;
    }
    int b0 = s_vk.bright_ever ? brt_level(s_vk.bi0) : -1, b = s_vk.bright_ever ? brt_level(s_vk.bi) : -1;
    char line[LOG_LINE_MAX_BYTES];
    vk_swipe_line(line, sizeof(line), &s_vk, b0, b, now);
    ESP_LOGI(TAG, "%s", line);
}

static void touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    uint32_t now = lv_tick_get();
    lv_point_t p = {0, 0};
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_indev_get_point(lv_indev_active(), &p);
    }
    switch (code) {
    case LV_EVENT_PRESSED: {
        if (s_vk.state == VK_ARMED || s_vk.state == VK_BRIGHT) {
            knob_events(vk_lost(&s_vk), now);
        }
        dp_touch_press(&s_touch, s_logged_stage);
        dp_activity(&s_dp, dp_clock(), DP_CAUSE_TOUCH);
        int user;
        bool set;
        volume_hello_fields(&user, &set);
        vk_press(&s_vk, p.x, p.y, now, !dp_touch_sends(&s_touch), (uint8_t)user);
        vk_swipe_frame(&s_vk, orient_target());
        if (s_vk.cls == VK_EDGE) {
            volume_count_edge();
        }
        break;
    }
    case LV_EVENT_PRESSING: {
        bool slid = s_vk.slid;
        knob_events(vk_move(&s_vk, p.x, p.y, now), now);
        if (!slid && s_vk.slid) {
            bright_count_slide();
        }
        break;
    }
    case LV_EVENT_RELEASED:
        knob_events(vk_release(&s_vk, now), now);
        log_knob_end(now);
        log_swipe_end(now);
        break;
    case LV_EVENT_PRESS_LOST:
        knob_events(vk_lost(&s_vk), now);
        log_knob_end(now);
        log_swipe_end(now);
        break;
    default:
        break;
    }
}

static void refr_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        xSemaphoreTake(s_te_sem, 0);
        if (s_panel_on && xSemaphoreTake(s_te_sem, pdMS_TO_TICKS(DISPLAY_TE_WAIT_MS)) != pdTRUE) {
            s_te_missed++;
        }
        s_refr_start = lv_tick_get();
        s_refr_t0_us = esp_timer_get_time();
    } else {
        uint32_t d = lv_tick_elaps(s_refr_start);
        s_refr_count++;
        s_refr_sum_ms += d;
        if (d > s_refr_worst_ms) {
            s_refr_worst_ms = d;
        }
        const int64_t t = esp_timer_get_time();
        if (s_work_stage == 2) {
            const uint32_t us = (uint32_t)(s_work_tick_us + (t - s_refr_t0_us));
            s_work_sum += us;
            s_work_max = us > s_work_max ? us : s_work_max;
            s_work_stage = 0;
        }
        if (s_cut_pending) {
            s_cut_last = (uint32_t)((t - s_cut_t0) / 1000);
            s_cut_max = s_cut_last > s_cut_max ? s_cut_last : s_cut_max;
            s_cut_pending = false;
            s_cut_t0 = 0;
        }
    }
}

static void draw_ring(int width, int opa)
{
    const int c = LCD_SIZE / 2, r_out = LISTEN_RING_DIAMETER / 2, r_clear = r_out - LISTEN_RING_MAX_WIDTH_PX - 1;
    const float r_in = (float)(r_out - width);
    uint16_t *base = (uint16_t *)s_out->data;
    const size_t stride = s_out->header.stride / 2;
    for (int y = c - r_out - 1; y <= c + r_out; y++) {
        for (int x = c - r_out - 1; x <= c + r_out; x++) {
            float dx = x + 0.5f - c, dy = y + 0.5f - c, d2 = dx * dx + dy * dy;
            if (d2 < (float)(r_clear * r_clear) || d2 > (float)((r_out + 1) * (r_out + 1))) {
                continue;
            }
            float d = sqrtf(d2);
            float cov = fminf(fmaxf((float)r_out - d + 0.5f, 0.0f), 1.0f) - fminf(fmaxf(r_in - d + 0.5f, 0.0f), 1.0f);
            int scale = width > 0 && cov > 0 ? (int)(cov * (float)opa * 100.0f / 255.0f) : 0;
            base[(size_t)y * stride + x] = rgb565(scale_hex(MASCOT_COLOR, scale));
        }
    }
}

/* Invalidates the ring band as strips so a ring change repaints about ten thousand pixels, not the screen. */
static void invalidate_ring(void)
{
    const int r_out = LISTEN_RING_DIAMETER / 2 + 1, r_in = r_out - LISTEN_RING_MAX_WIDTH_PX - 2, c = LCD_SIZE / 2;
    for (int y0 = c - r_out; y0 < c + r_out; y0 += DIRTY_BAND_ROWS) {
        int y1 = y0 + DIRTY_BAND_ROWS > c + r_out ? c + r_out : y0 + DIRTY_BAND_ROWS;
        int da = abs(y0 - c), db = abs(y1 - 1 - c);
        int dn = (y0 <= c && y1 - 1 >= c) ? 0 : (da < db ? da : db), df = da > db ? da : db;
        int xo = (int)ceilf(sqrtf((float)(r_out * r_out - dn * dn))) + 1;
        int xi = df >= r_in ? 0 : (int)floorf(sqrtf((float)(r_in * r_in - df * df))) - 1;
        lv_display_t *disp = lv_obj_get_display(s_canvas);
        if (xi <= 0) {
            lv_area_t all = {c - xo, y0, c + xo, y1 - 1};
            lv_inv_area(disp, &all);
        } else {
            lv_area_t l = {c - xo, y0, c - xi, y1 - 1}, r = {c + xi, y0, c + xo, y1 - 1};
            lv_inv_area(disp, &l);
            lv_inv_area(disp, &r);
        }
    }
}

_Static_assert(SCENE_OUT_PX == LCD_SIZE, "the arc and its clear loop index s_out in panel coordinates");

static const volarc_t *arc_table(int kind)
{
    return kind == AV_BRT ? &s_barc : &s_arc;
}

static void arc_put(int kind, uint32_t e, int quad, int v)
{
    int x, y;
    volarc_xy(e, quad, &x, &y);
    int t = volarc_thr(e);
    ((uint16_t *)s_out->data)[(size_t)y * (s_out->header.stride / 2) + x] = kind == AV_BRT ? s_barc_col[volarc_seg_class(t, v)] : s_arc_col[volarc_class(t, v)];
}

static const volarc_t *over_table(int kind)
{
    return &s_over[kind == AV_BRT ? 1 : 0];
}

static int arc_chunk(int kind)
{
    return kind == AV_BRT ? BRT_ARC_CHUNK_CELLS : VOL_ARC_CHUNK_THR;
}

static int arc_inv_rects(const volarc_rect_t *r, int n)
{
    lv_display_t *disp = lv_obj_get_display(s_canvas);
    for (int i = 0; i < n; i++) {
        lv_area_t a = {r[i].x1 + CANVAS_OFF, r[i].y1 + CANVAS_OFF, r[i].x2 + CANVAS_OFF, r[i].y2 + CANVAS_OFF};
        lv_inv_area(disp, &a);
    }
    return n;
}

static int arc_invalidate(int kind, int quad, int lo, int hi)
{
    volarc_rect_t r[VOLARC_MAX_AREAS];
    return arc_inv_rects(r, volarc_areas_chunk(arc_table(kind), quad, lo, hi, arc_chunk(kind), r, VOLARC_MAX_AREAS));
}

/* Whole-meter frame: the lit stretch shares the rectangle of the end chunk it touches. */
static int arc_invalidate_whole(int kind, int quad, int stretch)
{
    volarc_rect_t r[VOLARC_MAX_AREAS];
    return arc_inv_rects(r, volarc_whole_areas(arc_table(kind), over_table(kind), quad, arc_chunk(kind), stretch, r, VOLARC_MAX_AREAS));
}

static int arc_paint(int kind, int quad, int v, int lo, int hi)
{
    const volarc_t *t = arc_table(kind);
    for (int i = t->cnt[lo]; i < t->cnt[hi]; i++) {
        arc_put(kind, t->e[i], quad, v);
    }
    return arc_invalidate(kind, quad, lo, hi);
}

/* Paints the end-cue cells with threshold in (lo, hi] lit or black for stretch over; blank blacks them all. */
static void over_put(int kind, int quad, int lo, int hi, int over, bool blank)
{
    const volarc_t *t = over_table(kind);
    uint16_t col = kind == AV_BRT ? s_barc_col[volarc_over_class(1, over)] : s_arc_col[volarc_over_class(0, over)];
    for (int i = t->cnt[lo]; i < t->cnt[hi]; i++) {
        int x, y;
        volarc_xy(t->e[i], quad, &x, &y);
        bool lit = !blank && volarc_over_lit(volarc_thr(t->e[i]), over);
        ((uint16_t *)s_out->data)[(size_t)y * (s_out->header.stride / 2) + x] = lit ? col : 0;
    }
}

/* Returns the number of areas invalidated; the ring waits for a frame with more than VOLARC_RING_MAX_AREAS. */
static int arc_update(uint32_t now)
{
    avact_t a = av_frame(&s_av, now, ((int)lroundf(orient_target() / 90.0f)) & 3);
    int areas = 0;
    if ((a.flags & AV_CLEAR_OLD) && arc_table(a.old_kind)->n > 0) {
        const volarc_t *t = arc_table(a.old_kind);
        for (int i = 0; i < t->n; i++) {
            int x, y;
            volarc_xy(t->e[i], a.old_quad, &x, &y);
            ((uint16_t *)s_out->data)[(size_t)y * (s_out->header.stride / 2) + x] = 0;
        }
        over_put(a.old_kind, a.old_quad, 0, 2 * ARC_OVER_CELLS, a.old_over, true);
        areas += arc_invalidate_whole(a.old_kind, a.old_quad, a.old_over);
    }
    if ((a.flags & AV_DRAW_NEW) && arc_table(a.kind)->n > 0) {
        const volarc_t *t = arc_table(a.kind);
        for (int i = 0; i < t->n; i++) {
            arc_put(a.kind, t->e[i], a.quad, a.val);
        }
        over_put(a.kind, a.quad, 0, 2 * ARC_OVER_CELLS, a.over_to, false);
        areas += arc_invalidate_whole(a.kind, a.quad, a.over_to);
    }
    if ((a.flags & AV_DIFF) && arc_table(a.kind)->n > 0) {
        int lo, hi;
        if (a.kind == AV_BRT) {
            volarc_seg_change_range(a.from, a.to, &lo, &hi);
        } else {
            volarc_change_range(a.from, a.to, &lo, &hi);
        }
        areas += arc_paint(a.kind, a.quad, a.to, lo, hi);
    }
    if ((a.flags & AV_OVER) && arc_table(a.kind)->n > 0) {
        int lo[2], hi[2], k = volarc_over_ranges(a.over_from, a.over_to, lo, hi);
        for (int i = 0; i < k; i++) {
            over_put(a.kind, a.quad, lo[i], hi[i], a.over_to, false);
        }
        volarc_rect_t r[2];
        areas += arc_inv_rects(r, volarc_over_areas(over_table(a.kind), a.quad, a.over_from, a.over_to, r));
    }
    return areas;
}

static void update_ring(void)
{
    int w = 0, opa = 0;
    if (s_state == ST_LISTENING && (ANIM_RING || s_anim.clip == APLAY_CLAWD)) {
        int lvl = audio_mic_level() * LISTEN_RING_STEPS / 1001 * 1000 / (LISTEN_RING_STEPS - 1);
        opa = LISTEN_RING_MIN_OPA + lvl * (255 - LISTEN_RING_MIN_OPA) / 1000;
        w = LISTEN_RING_MIN_WIDTH_PX + lvl * (LISTEN_RING_MAX_WIDTH_PX - LISTEN_RING_MIN_WIDTH_PX) / 1000;
    }
    if (w != s_ring_width || opa != s_ring_opa) {
        s_ring_width = w;
        s_ring_opa = opa;
        draw_ring(w, opa);
        invalidate_ring();
    }
}

static void power_tick(void)
{
    uint32_t t = dp_clock();
    uint32_t bits = __atomic_exchange_n(&s_wake_bits, 0, __ATOMIC_RELAXED);
    for (int c = 1; c < DP_CAUSE_N; c++) {
        if (c != DP_CAUSE_SETDOWN && (bits & (1u << c))) {
            dp_activity(&s_dp, t, (dp_cause_t)c);
        }
    }
    if (bits & (1u << DP_CAUSE_SETDOWN)) {
        dp_setdown(&s_dp, t);
    }
    if (bits & AG_PICKUP_BIT) {
        aplay_pickup(&s_ap);
    }
    dp_out_t o = dp_update(&s_dp, t);
    if (o.panel_on && !s_panel_on) {
        bsp_display_brightness_set(o.level_pct);
        s_applied_level = o.level_pct;
        esp_lcd_panel_disp_on_off(s_panel, true);
        s_panel_on = true;
        ESP_LOGI(TAG, "disp: panel=on level=%d", o.level_pct);
    } else {
        if (s_panel_on && o.level_pct != s_applied_level) {
            bsp_display_brightness_set(o.level_pct);
            s_applied_level = o.level_pct;
        }
        if (!o.panel_on && s_panel_on) {
            esp_lcd_panel_disp_on_off(s_panel, false);
            s_panel_on = false;
            ESP_LOGI(TAG, "disp: panel=off level=%d", o.level_pct);
        }
    }
    if (o.stage != s_logged_stage) {
        if (o.stage < s_logged_stage) {
            s_wakes[o.cause]++;
        }
        s_logged_stage = o.stage;
        ESP_LOGI(TAG, "disp: stage=%s cause=%s level=%d hold=%u", STAGE_NAME[o.stage], CAUSE_NAME[o.cause], o.level_pct, (unsigned)o.hold_ms);
    }
}

/* Hands the player the pack once verified, then picks this tick's clip and frame; nothing is drawn while the check runs. */
static void anim_tick(uint32_t now)
{
    if (skin_status() == AP_VERIFYING) {
        s_anim = (aplay_out_t){APLAY_NOTHING, 0};
        return;
    }
    if (!s_ap_ready) {
        s_ap_ready = true;
        const ap_pack_t *p = skin_pack();
        if (p) {
            s_abuf = heap_caps_malloc(p->max_area, MALLOC_CAP_SPIRAM);
            if (!s_abuf) {
                ESP_LOGW(TAG, "anim: no frame buffer of %u bytes", (unsigned)p->max_area);
                p = NULL;
            }
        }
        aplay_init(&s_ap, p);
        aplay_enable(&s_ap, s_skin_on);
        aplay_pin(&s_ap, s_anim_pin);
    }
    const uint32_t cuts = s_ap.cuts;
    s_anim = aplay_tick(&s_ap, now, (int)s_state, ag_stage(s_panel_on, s_logged_stage, ANIM_DIM_PLAYS));
    s_cut_pending = s_cut_pending || (s_ap.cuts != cuts && s_cut_t0 != 0);
    if (!s_cut_pending) {
        s_cut_t0 = 0;
    }
    if (s_anim.clip < 0) {
        anim_reset_held();
    }
}

static void tick_cb(lv_timer_t *t)
{
    uint32_t now = lv_tick_get();
    uint32_t dt = now - s_last_tick;
    s_last_tick = now;
    knob_events(vk_tick(&s_vk, now), now);
    if (s_pop != s_pop_target && now - s_pop_at >= BUBBLE_POP_STEP_MS) {
        s_pop += s_pop < s_pop_target ? 1 : -1;
        s_pop_at = now;
    }
    power_tick();
    anim_tick(now);
    scene_t sc;
    compute_scene(&sc, now);
    if (memcmp(&sc, &s_drawn, sizeof(sc)) != 0 || bench_spins() || s_bench == UI_BENCH_REDRAW) {
        s_drawn = sc;
        redraw(&sc);
    }
    s_tick_bands = 0;
    if (art_wanted() != s_art_wanted_last) {
        s_model_dirty = true;
    }
    if (s_bench == UI_BENCH_CANVAS) {
        lv_obj_invalidate(s_canvas);
    } else if (s_bench == UI_BENCH_SCREEN) {
        lv_obj_invalidate(lv_screen_active());
    }
    if (bench_spins()) {
        s_bench_angle = fmodf(s_bench_angle + 0.18f * (float)dt, 360.0f);
        apply_rotation(s_bench_angle);
    } else {
        apply_rotation(orient_step(dt, s_panel_on));
    }
    if (volarc_ring_ok(arc_update(now), s_tick_bands)) {
        update_ring();
    }
    if (s_work_stage == 1) {
        s_work_tick_us = esp_timer_get_time() - s_work_t0;
        s_work_stage = 2;
    }
    static uint32_t last_log;
    if (now - last_log >= UI_FPS_LOG_PERIOD_MS) {
        uint32_t n = s_refr_count, worst = s_refr_worst_ms, sum = s_refr_sum_ms;
        s_refr_count = s_refr_worst_ms = s_refr_sum_ms = 0;
        if (last_log) {
            ESP_LOGI(TAG, "fps=%u refr=%u/%u ang=%d b=%d blit=%u px=%u cover=%u/%u te=%u edges=%u stk=%u [%s]",
                     (unsigned)(n * 1000 / (now - last_log)), (unsigned)(n ? sum / n : 0), (unsigned)worst, s_shown_rot, s_bench,
                     (unsigned)(s_blit_n ? s_blit_us / s_blit_n : 0),
                     (unsigned)(s_blit_n ? s_blit_px / s_blit_n : 0), (unsigned)s_cover_frames, (unsigned)s_cover_viol, (unsigned)s_te_missed, (unsigned)s_te_edges,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL), s_miss);
            s_te_edges = 0;
            s_miss[0] = 0;
            s_redraw_us = s_blit_us = s_redraw_n = s_blit_n = s_blit_px = 0;
        }
        last_log = now;
    }
}

static void IRAM_ATTR te_isr(void *arg)
{
    s_te_edges++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_te_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

static void te_init(void)
{
    s_te_sem = xSemaphoreCreateBinary();
    gpio_config_t io = {.pin_bit_mask = 1ULL << DISPLAY_TE_GPIO, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_NEGEDGE};
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(DISPLAY_TE_GPIO, te_isr, NULL);
}

static void round_area_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
    if (s_inval_n < INVAL_LOG_MAX) {
        s_inval[s_inval_n++] = *a;
    }
}

static lv_display_t *display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {.swap_xy = 0, .mirror_x = 1, .mirror_y = 1},
    };
    ESP_ERROR_CHECK(esp_lv_adapter_init(&cfg.lv_adapter_cfg));
    const bsp_display_config_t dc = {.max_transfer_sz = LCD_SIZE * LCD_SIZE * 2};
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(bsp_display_new(&dc, &panel, &io));
    s_panel = panel;
    esp_lv_adapter_display_config_t dcfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = cfg.rotation,
            .hor_res = LCD_SIZE,
            .ver_res = LCD_SIZE,
            .buffer_height = DISPLAY_BUFFER_ROWS,
            .use_psram = DISPLAY_BUFFER_IN_PSRAM,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&dcfg);
    ESP_ERROR_CHECK(disp ? ESP_OK : ESP_FAIL);
    const esp_lv_adapter_draw_bitmap_callbacks_t hook = {.custom_draw_bitmap = flush_hook};
    ESP_ERROR_CHECK(esp_lv_adapter_set_draw_bitmap_callbacks(disp, &hook, NULL));
    te_init();
    lv_display_add_event_cb(disp, round_area_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    esp_lcd_touch_handle_t tp;
    ESP_ERROR_CHECK(bsp_touch_new(&cfg, &tp));
    const esp_lv_adapter_touch_config_t tc = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    ESP_ERROR_CHECK(esp_lv_adapter_register_touch(&tc) ? ESP_OK : ESP_FAIL);
    s_applied_level = bright_effective();
    ESP_ERROR_CHECK(bsp_display_brightness_set(s_applied_level));
    ESP_ERROR_CHECK(esp_lv_adapter_start());
    return disp;
}

void ui_start(void)
{
    lv_display_t *disp = display_start();
    bsp_display_lock(-1);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    s_model = heap_caps_calloc(SCENE * SCENE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_out = lv_draw_buf_create(SCENE_OUT_PX, SCENE_OUT_PX, LV_COLOR_FORMAT_RGB565, 0);
    ESP_ERROR_CHECK(s_model && s_out ? ESP_OK : ESP_ERR_NO_MEM);
    lv_draw_buf_clear(s_out, NULL);
    s_pal[PAL_FILL] = rgb565(BUBBLE_FILL_COLOR);
    s_pal[PAL_INK] = rgb565(BUBBLE_INK_COLOR);
    s_pal[PAL_ERROR] = rgb565(BUBBLE_ERROR_COLOR);
    s_pal[PAL_OFFLINE_FILL] = rgb565(BUBBLE_OFFLINE_FILL_COLOR);
    s_arc_col[VOLARC_FILL] = rgb565(BUBBLE_FILL_COLOR);
    s_arc_col[VOLARC_TRACK] = rgb565(BUBBLE_OFFLINE_FILL_COLOR);
    s_arc_col[VOLARC_MUTE] = rgb565(BUBBLE_ERROR_COLOR);
    s_barc_col[VOLARC_FILL] = rgb565(BRT_ARC_FILL_COLOR);
    s_barc_col[VOLARC_TRACK] = rgb565(BRT_ARC_TRACK_COLOR);
    av_init(&s_av);
    int64_t tab_t0 = esp_timer_get_time();
    uint32_t *arc_store = heap_caps_malloc(VOLARC_CAP * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!arc_store || volarc_build(&s_arc, arc_store, VOLARC_CAP) < 0) {
        s_arc.n = 0;
        ESP_LOGW(TAG, "volume arc table unavailable");
    }
    uint32_t *barc_store = heap_caps_malloc(VOLARC_CAP * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!barc_store || volarc_build_seg(&s_barc, barc_store, VOLARC_CAP, BRT_ARC_START_DEG, BRT_ARC_SWEEP_DEG, BRT_STEPS + 1) < 0) {
        s_barc.n = 0;
        ESP_LOGW(TAG, "brightness arc table unavailable");
    }
    for (int k = 0; k < 2; k++) {
        uint32_t *store = heap_caps_malloc(VOLARC_OVER_CAP * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        bool brt = k == AV_BRT;
        if (!store || volarc_build_over(&s_over[k], store, VOLARC_OVER_CAP, brt ? BRT_ARC_START_DEG : VOL_ARC_START_DEG, brt ? BRT_ARC_SWEEP_DEG : VOL_SWEEP_DEG,
                                        brt ? BRT_STEPS + 1 : 100) < 0) {
            memset(&s_over[k], 0, sizeof(s_over[k]));
            ESP_LOGW(TAG, "end-cue table %d unavailable", k);
        }
    }
    ESP_LOGI(TAG, "arc tables built in %d ms", (int)((esp_timer_get_time() - tab_t0) / 1000));
    render_masks(scr);
    s_blit_done = xSemaphoreCreateBinary();
    xTaskCreate(blit_worker, "blit", STACK_BLIT, NULL, PRIO_BLIT, &s_blit_task);
    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_draw_buf(s_canvas, s_out);
    lv_obj_set_pos(s_canvas, (LCD_SIZE - SCENE_OUT_PX) / 2, (LCD_SIZE - SCENE_OUT_PX) / 2);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_ALL, NULL);
    lv_display_add_event_cb(disp, refr_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, refr_cb, LV_EVENT_REFR_READY, NULL);
    s_last_tick = lv_tick_get();
    dp_start();
    ui_set_state_locked(ST_IDLE);
    anim_tick(s_last_tick);
    scene_t sc;
    compute_scene(&sc, s_last_tick);
    s_drawn = sc;
    redraw(&sc);
    lv_timer_create(tick_cb, UI_LEVEL_TIMER_MS, NULL);
    bsp_display_unlock();
    ESP_LOGI(TAG, "ui up");
}

void ui_set_state(dev_state_t s)
{
    bsp_display_lock(-1);
    ui_set_state_locked(s);
    bsp_display_unlock();
}

void ui_set_angle_override(bool on, float deg)
{
    orient_override(on, deg);
}

void ui_set_arc(int level)
{
    bsp_display_lock(-1);
    av_force(&s_av, level > 100 ? 100 : level);
    bsp_display_unlock();
}

void ui_set_brt_arc(int index)
{
    bsp_display_lock(-1);
    av_force_kind(&s_av, AV_BRT, index > BRT_STEPS ? BRT_STEPS : index);
    bsp_display_unlock();
}

void ui_set_arc_over(int cells)
{
    bsp_display_lock(-1);
    av_force_over(&s_av, cells > ARC_OVER_CELLS ? ARC_OVER_CELLS : cells < -ARC_OVER_CELLS ? -ARC_OVER_CELLS : cells);
    bsp_display_unlock();
}

void ui_set_bench(int mode)
{
    bsp_display_lock(-1);
    s_bench = mode;
    bsp_display_unlock();
}

lv_draw_buf_t *ui_snapshot_take(void)
{
    bsp_display_lock(-1);
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (buf && art_wanted()) {
        const int w = buf->header.w < ACOMP_ROWS ? buf->header.w : ACOMP_ROWS, h = buf->header.h < ACOMP_ROWS ? buf->header.h : ACOMP_ROWS;
        acomp_rows(&s_art, s_ac.art, s_pal, (uint16_t *)buf->data, buf->header.stride / 2, 0, 0, w, h);
    }
    bsp_display_unlock();
    return buf;
}

void ui_snapshot_free(lv_draw_buf_t *buf)
{
    bsp_display_lock(-1);
    lv_draw_buf_destroy(buf);
    bsp_display_unlock();
}

void ui_wake(dp_cause_t cause)
{
    __atomic_fetch_or(&s_wake_bits, 1u << cause, __ATOMIC_RELAXED);
}

void ui_wake_bits(uint32_t bits)
{
    __atomic_fetch_or(&s_wake_bits, bits, __ATOMIC_RELAXED);
}

void ui_set_disp_force(int stage)
{
    bsp_display_lock(-1);
    dp_force(&s_dp, stage > DP_OFF ? DP_OFF : stage);
    bsp_display_unlock();
}

void ui_set_disp_scale(int n)
{
    bsp_display_lock(-1);
    s_time_scale = n < 1 ? 1 : n;
    dp_start();
    dp_state(&s_dp, dp_clock(), (int)s_state);
    bsp_display_unlock();
}

void ui_set_orbit(int index)
{
    bsp_display_lock(-1);
    s_orbit_force = index;
    bsp_display_unlock();
}

void ui_set_fill(int grey)
{
    bsp_display_lock(-1);
    s_fill = grey > 255 ? 255 : grey;
    lv_obj_t *scr = lv_screen_active();
    if (s_fill >= 0) {
        lv_obj_set_style_bg_color(scr, lv_color_make(s_fill, s_fill, s_fill), 0);
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();
}

void ui_set_brightness_pin(int pct)
{
    bsp_display_lock(-1);
    bright_test_pin(pct);
    dp_set_active(&s_dp, bright_effective());
    bsp_display_unlock();
}

void ui_set_brightness(int pct)
{
    bsp_display_lock(-1);
    bright_set(pct);
    dp_set_active(&s_dp, bright_effective());
    bsp_display_unlock();
}

int ui_panel_level(void)
{
    return __atomic_load_n(&s_applied_level, __ATOMIC_RELAXED);
}

void ui_wake_counts(uint32_t out[DP_CAUSE_N])
{
    for (int i = 0; i < DP_CAUSE_N; i++) {
        out[i] = s_wakes[i];
    }
}

void ui_set_skin(bool on)
{
    bsp_display_lock(-1);
    s_skin_on = on;
    if (s_ap_ready) {
        aplay_enable(&s_ap, on);
    }
    bsp_display_unlock();
}

void ui_set_anim_pin(int clip)
{
    bsp_display_lock(-1);
    s_anim_pin = clip;
    if (s_ap_ready) {
        aplay_pin(&s_ap, clip);
    }
    bsp_display_unlock();
}

void ui_anim_stats(ui_anim_stats_t *out)
{
    *out = (ui_anim_stats_t){
        .pack = (int)skin_status(),
        .skin = s_skin_on,
        .clip = s_anim.clip,
        .frames = s_held.frames,
        .dec_sum = s_dec_sum,
        .dec_max = s_dec_max,
        .work_sum = s_work_sum,
        .work_max = s_work_max,
        .late = s_ap.late,
        .cuts = s_ap.cuts,
        .cut_last = s_cut_last,
        .cut_max = s_cut_max,
        .seeks = s_held.seeks,
        .verify_ms = skin_verify_ms(),
    };
}
