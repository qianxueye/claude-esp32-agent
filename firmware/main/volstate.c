#include "volstate.h"

int vs_effective(const volst_t *s)
{
    return s->ovr >= 0 ? s->ovr : s->user;
}

bool vs_load(volst_t *s, bool found, uint32_t raw)
{
    bool ok = found && raw <= 100;
    *s = (volst_t){.user = ok ? (int)raw : DEFAULT_VOLUME, .stored = ok ? (int)raw : -1, .ovr = -1, .set = ok};
    return ok;
}

static void set_user(volst_t *s, int v)
{
    s->user = v;
    s->set = true;
    s->ovr = -1;
    s->dirty = s->user != s->stored;
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

void vs_knob_arm(volst_t *s)
{
    s->knob = true;
    s->ovr = -1;
    s->base = s->user;
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

void vs_saved(volst_t *s, int v)
{
    s->stored = v;
    s->dirty = s->user != s->stored;
}

bool vs_save_due(const volst_t *s, uint32_t now, uint32_t last_write, bool quiet)
{
    return s->dirty && quiet && !s->knob && now - last_write >= VOL_SAVE_MIN_MS;
}
