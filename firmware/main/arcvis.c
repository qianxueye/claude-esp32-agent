#include "arcvis.h"

void av_init(arcvis_t *v)
{
    *v = (arcvis_t){.force = -1, .quad_latch = -1};
}

void av_arm(arcvis_t *v, int val)
{
    av_arm_kind(v, AV_VOL, val, -1);
}

void av_arm_kind(arcvis_t *v, int kind, int val, int quad_latch)
{
    v->armed = true;
    v->linger = false;
    v->blink = false;
    v->kind = kind;
    v->val = val;
    v->quad_latch = quad_latch;
}

void av_set(arcvis_t *v, int val)
{
    v->val = val;
}

void av_end(arcvis_t *v, int val, uint32_t now)
{
    v->armed = false;
    v->linger = true;
    v->val = val;
    v->end_ms = now;
}

void av_blink(arcvis_t *v, uint32_t now)
{
    v->blink = true;
    v->blink_ms = now;
}

void av_force(arcvis_t *v, int val)
{
    av_force_kind(v, AV_VOL, val);
}

void av_force_kind(arcvis_t *v, int kind, int val)
{
    v->force = val;
    v->force_kind = kind;
}

static bool visible(const arcvis_t *v, uint32_t now)
{
    return v->force >= 0 || v->armed || (v->linger && now - v->end_ms < VOL_ARC_LINGER_MS);
}

bool av_visible(const arcvis_t *v, uint32_t now)
{
    return visible(v, now);
}

avact_t av_frame(arcvis_t *v, uint32_t now, int quad)
{
    avact_t a = {0};
    bool vis = visible(v, now);
    bool forced = v->force >= 0;
    if (v->blink && now - v->blink_ms >= BRT_END_BLINK_MS) {
        v->blink = false;
    }
    bool show = vis && (forced || !v->blink);
    int val = forced ? v->force : v->val, kind = forced ? v->force_kind : v->kind;
    quad = !forced && v->quad_latch >= 0 ? v->quad_latch : quad;
    if (!vis) {
        v->linger = false;
    }
    if (v->drawn && (!show || quad != v->drawn_quad || kind != v->drawn_kind)) {
        a.flags |= AV_CLEAR_OLD;
        a.old_quad = v->drawn_quad;
        a.old_kind = v->drawn_kind;
        v->drawn = false;
    }
    if (show && !v->drawn) {
        a.flags |= AV_DRAW_NEW;
        a.quad = quad;
        a.kind = kind;
        a.val = val;
        v->drawn = true;
    } else if (show && val != v->drawn_val) {
        a.flags |= AV_DIFF;
        a.quad = quad;
        a.kind = kind;
        a.from = v->drawn_val;
        a.to = val;
    }
    if (show) {
        v->drawn_val = val;
        v->drawn_quad = quad;
        v->drawn_kind = kind;
    }
    return a;
}
