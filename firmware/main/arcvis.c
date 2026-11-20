#include "arcvis.h"

_Static_assert((ARC_OVER_CELLS - 1) * ARC_SPRING_STEP_MS < VOL_ARC_LINGER_MS, "the spring must end well inside the linger");

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
    v->over = 0;
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
    bool same = v->drawn && v->drawn_kind == v->kind && v->over * v->drawn_over > 0;
    int mag = v->over < 0 ? -v->over : v->over, drawn = v->drawn_over < 0 ? -v->drawn_over : v->drawn_over;
    v->over = same ? (v->over < 0 ? -1 : 1) * (mag < drawn ? mag : drawn) : 0;
    v->armed = false;
    v->linger = true;
    v->val = val;
    v->end_ms = now;
}

void av_over(arcvis_t *v, int over)
{
    if (v->armed) {
        v->over = over;
    }
}

int av_spring(int over0, uint32_t elapsed_ms)
{
    int mag = over0 < 0 ? -over0 : over0;
    int left = mag - 1 - (int)(elapsed_ms / ARC_SPRING_STEP_MS);
    left = left < 0 ? 0 : left;
    return over0 < 0 ? -left : left;
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

void av_force_over(arcvis_t *v, int over)
{
    v->force_over = over;
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
    bool show = visible(v, now);
    bool forced = v->force >= 0;
    int val = forced ? v->force : v->val, kind = forced ? v->force_kind : v->kind;
    quad = !forced && v->quad_latch >= 0 ? v->quad_latch : quad;
    if (!show) {
        v->linger = false;
    }
    int over = forced ? v->force_over : v->armed ? v->over : av_spring(v->over, now - v->end_ms);
    over = (over < 0 && val == 0) || (over > 0 && val == (kind == AV_BRT ? BRT_STEPS : 100)) ? over : 0;
    if (v->drawn && (!show || quad != v->drawn_quad || kind != v->drawn_kind)) {
        a.flags |= AV_CLEAR_OLD;
        a.old_quad = v->drawn_quad;
        a.old_kind = v->drawn_kind;
        a.old_over = v->drawn_over;
        v->drawn = false;
        v->drawn_over = 0;
    }
    if (show) {
        a.quad = quad;
        a.kind = kind;
        a.over_from = v->drawn_over;
        a.over_to = over;
    }
    if (show && !v->drawn) {
        a.flags |= AV_DRAW_NEW;
        a.val = val;
        v->drawn = true;
    } else if (show) {
        if (val != v->drawn_val) {
            a.flags |= AV_DIFF;
            a.from = v->drawn_val;
            a.to = val;
        }
        if (over != v->drawn_over) {
            a.flags |= AV_OVER;
        }
    }
    if (show) {
        v->drawn_val = val;
        v->drawn_quad = quad;
        v->drawn_kind = kind;
        v->drawn_over = over;
    }
    return a;
}
