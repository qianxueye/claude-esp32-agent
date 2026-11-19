#include "aglue.h"
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
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

bool ag_bring(const ap_pack_t *p, int clip, int frame, uint8_t *buf, ag_held_t *held)
{
    if (held->clip == clip && held->frame == frame) {
        return true;
    }
    const int from = held->clip == clip && frame == held->frame + 1 ? frame : 0;
    held->clip = held->frame = -1;
    if (!p || clip < 0 || clip >= p->clip_count || frame < 0 || frame >= p->clip[clip].frames) {
        return false;
    }
    const uint32_t area = (uint32_t)p->clip[clip].w * p->clip[clip].h;
    for (int f = from; f <= frame; f++) {
        ap_frame_t fr;
        if (!ap_frame(p, clip, f, &fr) || !ap_decode(&fr, buf, area)) {
            return false;
        }
    }
    held->clip = clip;
    held->frame = frame;
    held->frames++;
    held->seeks += from == 0 && frame > 0 ? 1 : 0;
    return true;
}

static int lo(int v, int m)
{
    return v < m ? m : v;
}

static int hi(int v, int m)
{
    return v > m ? m : v;
}

ag_place_t ag_place(const ap_pack_t *p, const ap_clip_t *c, int gx, int gy, int shift, int scene)
{
    ag_place_t pl;
    const int s = p->scale;
    pl.ox = scene / 2 + gx - p->canvas_w * s / 2 + c->x * s;
    pl.oy = scene / 2 + gy + shift - p->canvas_h * s / 2 + c->y * s;
    pl.x0 = lo(pl.ox, 0);
    pl.y0 = lo(pl.oy, 0);
    pl.x1 = hi(pl.ox + c->w * s, scene);
    pl.y1 = hi(pl.oy + c->h * s, scene);
    return pl;
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

void ag_copy(const uint8_t *buf, const ap_clip_t *c, uint8_t scale, const ag_place_t *pl, uint8_t *dst, int dst_stride)
{
    if (pl->x0 >= pl->x1 || pl->y0 >= pl->y1 || scale < 1) {
        return;
    }
    for (int y = pl->y0; y < pl->y1; y++) {
        const uint8_t *row = buf + (size_t)((y - pl->oy) / scale) * c->w;
        uint8_t *out = dst + (size_t)y * dst_stride;
        if (scale == 1) {
            memcpy(out + pl->x0, row + (pl->x0 - pl->ox), (size_t)(pl->x1 - pl->x0));
            continue;
        }
        for (int x = pl->x0; x < pl->x1; x++) {
            out[x] = row[(x - pl->ox) / scale];
        }
    }
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

ag_bubble_t ag_side_bubble(int w_units, int content_units, bool thought, bool left, int bub_y, int gx, int gy, int pop, int scene)
{
    const int h = AG_BUBBLE_H_UNITS;
    const int by = ag_bub_y_snap(bub_y);
    const int x1 = ag_bubble_edge(by, h * U / 2, ORBIT_HALF_X_PX, ORBIT_HALF_Y_PX);
    const int ws = pop >= 3 ? w_units : even_up(w_units * pop / 3), hs = pop >= 3 ? h : even_up(h * pop / 3);
    const int cx = scene / 2 + gx + ORBIT_RECENTRE_X_PX, cy = scene / 2 + gy + ORBIT_RECENTRE_Y_PX;
    const int mid_u = x1 / U - w_units + w_units / 2, yc = by / U;
    const int bx0 = left ? -(mid_u + ws / 2) : mid_u - ws / 2, by0 = yc - hs / 2;
    ag_bubble_t b;
    memset(&b, 0, sizeof b);
    b.body = rect_u(cx, cy, bx0, by0, ws, hs);
    if (pop < 3) {
        return b;
    }
    const int tx = bx0 + 1 + BUBBLE_PAD_UNITS + (w_units - 2 - 2 * BUBBLE_PAD_UNITS - content_units) / 2;
    b.text_x = cx + tx * U;
    b.text_y = cy + (by0 + 3) * U;
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
