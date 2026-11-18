#include "arcvis.h"

void av_init(arcvis_t *v)
{
    *v = (arcvis_t){.force = -1};
}

void av_arm(arcvis_t *v, int val)
{
    v->armed = true;
    v->linger = false;
    v->val = val;
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

void av_force(arcvis_t *v, int val)
{
    v->force = val;
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
    int val = v->force >= 0 ? v->force : v->val;
    if (!vis) {
        v->linger = false;
    }
    if (v->drawn && (!vis || quad != v->drawn_quad)) {
        a.flags |= AV_CLEAR_OLD;
        a.old_quad = v->drawn_quad;
        v->drawn = false;
    }
    if (vis && !v->drawn) {
        a.flags |= AV_DRAW_NEW;
        a.quad = quad;
        a.val = val;
        v->drawn = true;
    } else if (vis && val != v->drawn_val) {
        a.flags |= AV_DIFF;
        a.quad = quad;
        a.from = v->drawn_val;
        a.to = val;
    }
    if (vis) {
        v->drawn_val = val;
        v->drawn_quad = quad;
    }
    return a;
}
