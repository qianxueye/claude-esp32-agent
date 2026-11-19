#include "aglue.h"
#include <string.h>
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
