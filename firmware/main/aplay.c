#include "aplay.h"
#include <string.h>

void aplay_init(aplay_t *a, const ap_pack_t *pack)
{
    memset(a, 0, sizeof(*a));
    a->pack = pack;
    a->enabled = pack != NULL;
    a->pin = -1;
    a->state = -1;
    a->base = -1;
}

void aplay_set_ready(aplay_t *a, int (*ready)(void *ctx, int clip), void *ctx)
{
    a->ready = ready;
    a->ready_ctx = ctx;
}

void aplay_enable(aplay_t *a, bool on)
{
    a->enabled = on && a->pack != NULL;
}

void aplay_pin(aplay_t *a, int clip)
{
    a->pin = a->pack && clip >= 0 && clip < a->pack->clip_count ? clip : -1;
}

void aplay_pickup(aplay_t *a)
{
    a->pickup = true;
}

static int base_clip(const aplay_t *a, int state)
{
    if (!a->pack || !a->enabled) {
        return APLAY_CLAWD;
    }
    if (a->pin >= 0) {
        return a->pin;
    }
    if (state < 0 || state >= AP_STATE_SLOTS) {
        return APLAY_CLAWD;
    }
    const uint8_t v = a->pack->state_clip[state];
    return v < a->pack->clip_count ? v : APLAY_CLAWD;
}

static uint32_t frame_ms(const aplay_t *a)
{
    ap_frame_t f;
    return ap_frame(a->pack, a->clip, a->frame, &f) ? f.ms : 1;
}

static void start(aplay_t *a, int clip, uint32_t now)
{
    a->clip = clip;
    a->frame = 0;
    a->stalled = false;
    a->due = now + frame_ms(a);
}

/* One frame forward at most; a frame already over on arrival rebases the schedule and counts as late. */
static void step(aplay_t *a, uint32_t now)
{
    const ap_clip_t *c = &a->pack->clip[a->clip];
    if (a->frame + 1 < c->frames) {
        if (a->ready && a->ready(a->ready_ctx, a->clip) <= a->frame + 1) {
            a->starve += a->stalled ? 0 : 1;
            a->stalled = true;
            return;
        }
        a->stalled = false;
        a->frame++;
    } else if (a->once) {
        a->once = false;
        a->clip = a->base;
        a->frame = 0;
    } else if (c->loop && c->frames > 1) {
        a->frame = 0;
    } else {
        a->due = now + frame_ms(a);
        return;
    }
    a->steps++;
    const uint32_t ms = frame_ms(a);
    a->due += ms;
    if ((int32_t)(now - a->due) >= 0) {
        a->due = now + ms;
        a->late++;
    }
}

aplay_out_t aplay_tick(aplay_t *a, uint32_t now_ms, int state, aplay_stage_t stage)
{
    const bool pickup = a->pickup;
    a->pickup = false;
    const int base = base_clip(a, state);
    if (base < 0 || stage == APS_OFF) {
        a->running = a->once = a->dim = false;
        return (aplay_out_t){base < 0 ? APLAY_CLAWD : APLAY_NOTHING, 0};
    }
    bool fresh = false;
    if (!a->running || base != a->base || (a->pin < 0 && state != a->state)) {
        a->cuts += a->running ? 1 : 0;
        a->running = true;
        a->once = false;
        a->base = base;
        start(a, base, now_ms);
        fresh = true;
    }
    a->state = state;
    const uint8_t pc = a->pack->pickup_clip;
    if (pickup && stage == APS_ACTIVE && a->pin < 0 && state == APLAY_STATE_IDLE && !a->once && pc < a->pack->clip_count) {
        a->once = true;
        start(a, pc, now_ms);
        fresh = true;
    }
    if (stage == APS_DIM) {
        if (a->once) {
            a->once = false;
            start(a, base, now_ms);
        }
        a->dim = true;
    } else if (a->dim) {
        a->dim = false;
        a->due = now_ms + frame_ms(a);
    } else if (!fresh && (int32_t)(now_ms - a->due) >= 0) {
        step(a, now_ms);
    }
    return (aplay_out_t){a->clip, a->frame};
}
