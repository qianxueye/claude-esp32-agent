#include "volstate.h"

int vs_effective(const volst_t *s)
{
    return s->ovr >= 0 ? s->ovr : s->user;
}

bool vs_load(volst_t *s, int result, uint32_t raw)
{
    return vs_load_range(s, result, raw, 0, 100, DEFAULT_VOLUME);
}

bool vs_load_range(volst_t *s, int result, uint32_t raw, int lo, int hi, int dflt)
{
    bool ok = result == VS_LOAD_FOUND && raw >= (uint32_t)lo && raw <= (uint32_t)hi;
    *s = (volst_t){.user = ok ? (int)raw : dflt,
                   .stored = ok ? (int)raw : -1,
                   .ovr = -1,
                   .set = ok,
                   .ram = result == VS_LOAD_ERROR};
    return ok;
}

static void set_user(volst_t *s, int v)
{
    s->user = v;
    s->set = true;
    s->ovr = -1;
}

bool vs_host_set(volst_t *s, const uint8_t *payload, int len)
{
    if ((len != 1 && len != 2) || s->knob) {
        return false;
    }
    int v = payload[0] > 100 ? 100 : payload[0], mode = len == 2 ? payload[1] : VOL_MODE_TRANSIENT;
    if (mode == VOL_MODE_TRANSIENT) {
        s->ovr = v;
        return false;
    }
    if (mode == VOL_MODE_PERSIST || (mode == VOL_MODE_SEED && !s->set)) {
        set_user(s, v);
        return true;
    }
    return false;
}

void vs_session_reset(volst_t *s)
{
    s->ovr = -1;
}

int vs_knob_arm(volst_t *s)
{
    s->knob = true;
    s->ovr = -1;
    s->base = s->user;
    return s->base;
}

void vs_knob_set(volst_t *s, int v)
{
    s->user = v;
}

bool vs_knob_end(volst_t *s, bool cancel)
{
    s->knob = false;
    if (cancel) {
        s->user = s->base;
        return false;
    }
    set_user(s, s->user);
    return s->user != s->base;
}

bool vs_swipe_end(volst_t *s, bool cancel)
{
    return vs_knob_end(s, cancel || s->user == s->base);
}

void vs_saved(volst_t *s, int v)
{
    s->stored = v;
}

int vs_write_streak(volst_t *s, bool wrote)
{
    if (wrote != s->failing) {
        return VS_STREAK_SAME;
    }
    s->failing = !wrote;
    return wrote ? VS_STREAK_END : VS_STREAK_START;
}

bool vs_needs_save(const volst_t *s)
{
    int committed = s->knob ? s->base : s->user;
    return s->set && !s->ram && committed != s->stored;
}

bool vs_save_due(const volst_t *s, uint32_t now, uint32_t last_write, bool quiet)
{
    return vs_needs_save(s) && quiet && !s->knob && now - last_write >= VOL_SAVE_MIN_MS;
}

int vs_save_pick(bool a_due, bool b_due, int last)
{
    if (a_due && b_due) {
        return last == VS_PICK_A ? VS_PICK_B : VS_PICK_A;
    }
    return a_due ? VS_PICK_A : b_due ? VS_PICK_B : VS_PICK_NONE;
}
