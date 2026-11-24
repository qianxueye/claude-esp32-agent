#include "aglue.h"
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "battstate.h"
#include "motion.h"

uint32_t ag_imu_bits(int motion_ev, bool lifted)
{
    const bool pickup = (motion_ev & MOTION_EV_PICKUP) != 0;
    uint32_t bits = lifted || pickup ? 1u << DP_CAUSE_MOTION : 0;
    bits |= pickup ? AG_PICKUP_BIT : 0;
    bits |= (motion_ev & MOTION_EV_SETDOWN) ? 1u << DP_CAUSE_SETDOWN : 0;
    return bits;
}

aplay_stage_t ag_stage(bool panel_on, dp_stage_t stage, bool dim_plays)
{
    if (!panel_on) {
        return APS_OFF;
    }
    return stage == DP_ACTIVE || dim_plays ? APS_ACTIVE : APS_DIM;
}

ag_bring_t ag_bring(const ap_pack_t *p, int clip, int frame, int ready, uint8_t *buf, ag_held_t *held)
{
    if (held->clip == clip && held->frame == frame) {
        return AG_OK;
    }
    if (p && clip >= 0 && clip < p->clip_count && frame >= 0 && frame < p->clip[clip].frames && frame >= ready) {
        return AG_NOT_READY;
    }
    const int from = held->clip == clip && frame == held->frame + 1 ? frame : 0;
    held->clip = held->frame = -1;
    if (!p || clip < 0 || clip >= p->clip_count || frame < 0 || frame >= p->clip[clip].frames) {
        return AG_FAIL;
    }
    const uint32_t area = (uint32_t)p->clip[clip].w * p->clip[clip].h;
    for (int f = from; f <= frame; f++) {
        ap_frame_t fr;
        if (!ap_frame(p, clip, f, &fr) || !ap_decode(&fr, buf, area)) {
            return AG_FAIL;
        }
    }
    held->clip = clip;
    held->frame = frame;
    held->frames++;
    held->seeks += from == 0 && frame > 0 ? 1 : 0;
    return AG_OK;
}

ag_place_t ag_sticker_place(const ap_pack_t *p, const ap_clip_t *c, int gx, int gy, int scene)
{
    ag_place_t pl;
    const int s = p->scale;
    pl.ox = scene / 2 + gx + ORBIT_RECENTRE_X_PX - p->canvas_w * s / 2 + c->x * s;
    pl.oy = scene / 2 + gy + ORBIT_RECENTRE_Y_PX - p->canvas_h * s / 2 + c->y * s;
    pl.x0 = pl.ox;
    pl.y0 = pl.oy;
    pl.x1 = pl.ox + c->w * s;
    pl.y1 = pl.oy + c->h * s;
    return pl;
}

#define U BUBBLE_UNIT_PX

static int even_up(int v)
{
    return (v + 1) & ~1;
}

int ag_bubble_w_units(int content_units)
{
    return even_up(content_units + 2 * BUBBLE_PAD_UNITS + 2);
}

int ag_bub_y_snap(int y)
{
    const int v = y / U * U;
    return v < -AG_BUBBLE_Y_MAX_PX ? -AG_BUBBLE_Y_MAX_PX : v > AG_BUBBLE_Y_MAX_PX ? AG_BUBBLE_Y_MAX_PX : v;
}

int ag_bubble_edge(int bub_y, int half_h_px, int orbit_hx_px, int orbit_hy_px)
{
    const int r = SCENE_RING_CLEAR_PX - AG_BUBBLE_FIT_MARGIN_PX;
    const int dy = abs(bub_y) + half_h_px + 1 + orbit_hy_px;
    int x1 = r / U * U;
    while (x1 > 0 && (x1 + 1 + orbit_hx_px) * (x1 + 1 + orbit_hx_px) + dy * dy >= r * r) {
        x1 -= U;
    }
    return x1;
}

static ag_rect_t rect_u(int cx, int cy, int ux, int uy, int uw, int uh)
{
    return (ag_rect_t){cx + ux * U, cy + uy * U, uw * U, uh * U};
}

/* Cells at depth da..db beyond the inner edge column ic, rows ya..yb, all in rel units. */
static ag_cell_t cell(int cx, int cy, int ic, int dir, int da, int db, int ya, int yb, bool ink)
{
    const int ca = ic + dir * (1 + da), cb = ic + dir * (1 + db);
    const int lo = ca < cb ? ca : cb;
    return (ag_cell_t){rect_u(cx, cy, lo, ya, abs(cb - ca) + 1, yb - ya + 1), ink};
}

int ag_content_units(ag_content_t c)
{
    return (c.spinner ? AG_SPINNER_UNITS + 2 : 0) + AG_GLYPH_UNITS * c.glyphs + (c.dots ? 10 : 0);
}

ag_bubble_t ag_side_bubble(ag_content_t content, bool thought, bool left, int bub_y, int gx, int gy, int pop, int dot_phase, int scene)
{
    const int h = AG_BUBBLE_H_UNITS, content_units = ag_content_units(content);
    const int w_units = ag_bubble_w_units(content_units);
    const int by = ag_bub_y_snap(bub_y);
    const int x1 = ag_bubble_edge(by, h * U / 2, ORBIT_HALF_X_PX, ORBIT_HALF_Y_PX);
    const int ws = pop >= 3 ? w_units : even_up(w_units * pop / 3), hs = pop >= 3 ? h : even_up(h * pop / 3);
    const int cx = scene / 2 + gx + ORBIT_RECENTRE_X_PX, cy = scene / 2 + gy + ORBIT_RECENTRE_Y_PX;
    const int mid_u = x1 / U - w_units + w_units / 2, yc = by / U;
    const int bx0 = left ? -(mid_u + ws / 2) : mid_u - ws / 2, by0 = yc - hs / 2;
    ag_bubble_t b;
    memset(&b, 0, sizeof b);
    b.body = rect_u(cx, cy, bx0, by0, ws, hs);
    if (pop < 1) {
        return b;
    }
    b.n_frame = 4;
    b.frame[0] = (ag_cell_t){rect_u(cx, cy, bx0 + 1, by0, ws - 2, hs), true};
    b.frame[1] = (ag_cell_t){rect_u(cx, cy, bx0, by0 + 1, ws, hs - 2), true};
    b.frame[2] = (ag_cell_t){rect_u(cx, cy, bx0 + 2, by0 + 1, ws - 4, hs - 2), false};
    b.frame[3] = (ag_cell_t){rect_u(cx, cy, bx0 + 1, by0 + 2, ws - 2, hs - 4), false};
    if (pop < 3) {
        return b;
    }
    const int tx = bx0 + 1 + BUBBLE_PAD_UNITS + (w_units - 2 - 2 * BUBBLE_PAD_UNITS - content_units) / 2, ty = by0 + 3;
    const int mx = tx + (content.spinner ? AG_SPINNER_UNITS + 2 : 0);
    if (content.spinner) {
        b.spinner = true;
        b.spin = rect_u(cx, cy, tx, ty + 2, AG_SPINNER_UNITS, AG_SPINNER_UNITS);
    }
    if (content.dots) {
        b.n_dots = 3;
        for (int i = 0; i < 3; i++) {
            const int size = i == dot_phase ? 2 : 1;
            b.dots[i] = rect_u(cx, cy, mx + AG_GLYPH_UNITS * content.glyphs + 2 + i * 3, ty + 11 - size, size, size);
        }
    }
    b.mask = rect_u(cx, cy, mx, ty, AG_GLYPH_UNITS * content.glyphs, AG_TEXT_ROWS_PX / U);
    const int ic = left ? bx0 + ws - 1 : bx0, dir = left ? 1 : -1;
    if (thought) {
        b.cells[0] = cell(cx, cy, ic, dir, 1, 4, yc - 2, yc + 1, false);
        b.cells[1] = cell(cx, cy, ic, dir, 6, 7, yc - 1, yc, false);
        b.n_cells = 2;
    } else {
        b.cells[0] = cell(cx, cy, ic, dir, -1, -1, yc - 2, yc + 1, false);
        b.cells[1] = cell(cx, cy, ic, dir, 0, 0, yc - 2, yc + 1, true);
        b.cells[2] = cell(cx, cy, ic, dir, 0, 0, yc - 1, yc, false);
        b.cells[3] = cell(cx, cy, ic, dir, 1, 1, yc - 1, yc, true);
        b.n_cells = 4;
    }
    return b;
}

irect_t ag_bubble_bbox(const ag_bubble_t *b)
{
    if (b->n_frame == 0) {
        return (irect_t){0, 0, 0, 0};
    }
    irect_t r = {b->body.x, b->body.y, b->body.x + b->body.w, b->body.y + b->body.h};
    for (int i = 0; i < b->n_cells; i++) {
        const ag_rect_t c = b->cells[i].r;
        r.x0 = c.x < r.x0 ? c.x : r.x0;
        r.y0 = c.y < r.y0 ? c.y : r.y0;
        r.x1 = c.x + c.w > r.x1 ? c.x + c.w : r.x1;
        r.y1 = c.y + c.h > r.y1 ? c.y + c.h : r.y1;
    }
    return r;
}

ag_top_bubble_t ag_top_bubble(int w_units, bool thought, int pop, int gx, int gy, int scene)
{
    const int h = AG_BUBBLE_H_UNITS;
    const int ws = pop == 3 ? w_units : even_up(w_units * pop / 3), hs = pop == 3 ? h : even_up(h * pop / 3);
    const int below = thought ? 8 : 2;
    const int anchor_top = scene / 2 - 5 * MASCOT_CELL_PX + BUBBLE_SHIFT_UNITS * U + gy;
    ag_top_bubble_t t;
    t.bottom_u = (anchor_top - BUBBLE_GAP_UNITS * U) / U - below;
    t.cx_u = (scene / 2 + gx) / U;
    t.body = (ag_rect_t){(t.cx_u - ws / 2) * U, (t.bottom_u - hs) * U, ws * U, hs * U};
    return t;
}

ag_rect_t ag_batt_below(int w, int h, int gx, int gy, int scene)
{
    return (ag_rect_t){scene / 2 + BATT_CENTER_DX_PX + gx - w / 2, scene / 2 + 5 * MASCOT_CELL_PX + BATT_GAP_UNITS * U + gy - (h - BATT_PLAIN_H), w, h};
}

ag_rect_t ag_batt_beside_top(const ag_top_bubble_t *t, int w, int h, int scene)
{
    const int x = t->body.x + t->body.w + BATT_BESIDE_GAP_UNITS * U, x_max = scene / 2 + BATT_BESIDE_MAX_DX_PX - w;
    return (ag_rect_t){x < x_max ? x : x_max, t->body.y + BUBBLE_HEIGHT_UNITS / 2 * U - h / 2, w, h};
}

ag_rect_t ag_batt_beside_side(const ag_bubble_t *g, bool left, int w, int h)
{
    const irect_t box = ag_bubble_bbox(g);
    const int gap = BATT_BESIDE_GAP_UNITS * U;
    return (ag_rect_t){left ? box.x1 + gap : box.x0 - gap - w, g->body.y + g->body.h / 2 - h / 2, w, h};
}

ag_rect_t ag_batt_plate(ag_rect_t icon)
{
    const int m = BATT_PLATE_UNITS * U;
    return (ag_rect_t){icon.x - m, icon.y - m, icon.w + 2 * m, icon.h + 2 * m};
}

void ag_acc_add(ag_acc_t *a, uint32_t us)
{
    a->n++;
    a->sum += us;
    a->max = us > a->max ? us : a->max;
}

uint32_t ag_window_mean(uint32_t prev_sum, uint32_t prev_n, uint32_t sum, uint32_t n)
{
    const uint32_t dn = n - prev_n;
    return dn ? (sum - prev_sum) / dn : 0;
}

int ag_work_refresh(ag_work_t *w, int stage, uint32_t tick_us, uint32_t refr_us)
{
    if (!w->armed) {
        w->armed = true;
        w->boot_us = (stage == 2 ? tick_us : 0) + refr_us;
        return 0;
    }
    if (stage == 2) {
        ag_acc_add(&w->work, tick_us + refr_us);
        return 0;
    }
    return stage;
}

uint32_t ag_work_boot_ms(const ag_work_t *w)
{
    return (w->boot_us + 500) / 1000;
}
