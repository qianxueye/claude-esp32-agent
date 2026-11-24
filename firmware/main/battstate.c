#include "battstate.h"
#include "app_config.h"

enum { CHG_DONE = 4, CHG_CV = 3, DIR_CHARGE = 1 };

bool batt_present(uint8_t reg00)
{
    return (reg00 >> 3) & 1;
}

bool batt_vbus(uint8_t reg00)
{
    return (reg00 >> 5) & 1;
}

int batt_dir(uint8_t reg01)
{
    return (reg01 >> 5) & 3;
}

int batt_chg(uint8_t reg01)
{
    return reg01 & 7;
}

void batt_init(batt_state_t *s)
{
    *s = (batt_state_t){.look = BATT_HIDDEN, .logged_look = BATT_HIDDEN};
}

const char *batt_look_name(batt_look_t look)
{
    static const char *const N[BATT_LOOK_N] = {"hidden", "normal", "low", "charging", "done"};
    return look < BATT_LOOK_N ? N[look] : "?";
}

static int bars_of(int pct)
{
    int b = (pct + 24) / 25;
    return b < 1 ? 1 : b > BATT_BARS_MAX ? BATT_BARS_MAX : b;
}

batt_out_t batt_step(batt_state_t *s, const batt_in_t *in)
{
    batt_out_t o = {.look = s->look, .bars = s->bars};
    if (!in->ok) {
        o.log_fail = !s->failing;
        s->failing = true;
        if (!s->seen || in->now_ms - s->last_ok_ms >= BATT_STALE_MS) {
            s->look = o.look = BATT_HIDDEN;
            s->bars = o.bars = 0;
        }
        return o;
    }
    s->failing = false;
    const bool vbus = batt_vbus(in->reg00), present = batt_present(in->reg00);
    const int dir = batt_dir(in->reg01), chg = batt_chg(in->reg01), pct = in->pct;
    const bool known = pct < BATT_UNKNOWN_PCT;
    const bool done = vbus && chg == CHG_DONE;
    const bool charging = !done && vbus && (dir == DIR_CHARGE || chg <= CHG_CV);
    o.wake = s->vbus_seen && !s->vbus && vbus;
    s->vbus_seen = true;
    s->vbus = vbus;
    s->seen = true;
    s->last_ok_ms = in->now_ms;
    if (known) {
        if (charging || done || pct >= BATT_LOW_EXIT_PCT) {
            s->low = false;
        } else if (pct <= BATT_LOW_ENTER_PCT) {
            s->low = true;
        }
    }
    if (!present) {
        o.look = BATT_HIDDEN;
        o.bars = 0;
    } else if (done) {
        o.look = BATT_DONE;
        o.bars = known ? BATT_BARS_MAX : 0;
    } else if (charging) {
        o.look = BATT_CHARGING;
        o.bars = known ? bars_of(pct) : 0;
    } else if (s->low && known) {
        o.look = BATT_LOW;
        o.bars = 1;
    } else {
        o.look = BATT_NORMAL;
        o.bars = known ? bars_of(pct) : 0;
    }
    s->look = o.look;
    s->bars = o.bars;
    const int key = present | (vbus << 1) | (dir << 2) | (chg << 4) | (o.bars << 7);
    o.log = !s->logged || key != s->logged_key || o.look != s->logged_look || in->now_ms - s->last_log_ms >= BATT_LOG_EVERY_MS;
    if (o.log) {
        s->logged = true;
        s->logged_key = key;
        s->logged_look = o.look;
        s->last_log_ms = in->now_ms;
    }
    return o;
}

static void put(batt_rect_t *r, int *n, int x, int y, int w, int h, uint8_t color)
{
    r[(*n)++] = (batt_rect_t){x, y, w, h, color};
}

int batt_layout(batt_look_t look, int bars, batt_rect_t out[BATT_RECTS_MAX], int *w, int *h)
{
    int n = 0;
    if (look == BATT_HIDDEN) {
        *w = *h = 0;
        return 0;
    }
    const bool bolt = look == BATT_CHARGING || look == BATT_DONE;
    const int ox = bolt ? 28 : 0, oy = bolt ? 4 : 0;
    *w = bolt ? BATT_BOLT_W : BATT_PLAIN_W;
    *h = bolt ? BATT_BOLT_H : BATT_PLAIN_H;
    if (bolt) {
        put(out, &n, 12, 0, 8, 4, BATT_C_CREAM);
        put(out, &n, 8, 4, 8, 4, BATT_C_CREAM);
        put(out, &n, 4, 8, 8, 4, BATT_C_CREAM);
        put(out, &n, 0, 12, 20, 4, BATT_C_CREAM);
        put(out, &n, 8, 16, 8, 4, BATT_C_CREAM);
        put(out, &n, 4, 20, 8, 4, BATT_C_CREAM);
        put(out, &n, 0, 24, 8, 4, BATT_C_CREAM);
    }
    put(out, &n, ox, oy, 32, 4, BATT_C_GREY);
    put(out, &n, ox, oy + 4, 4, 16, BATT_C_GREY);
    put(out, &n, ox + 28, oy + 4, 4, 16, BATT_C_GREY);
    put(out, &n, ox, oy + 20, 32, 4, BATT_C_GREY);
    put(out, &n, ox + 32, oy + 8, 4, 8, BATT_C_GREY);
    const uint8_t bar_color = look == BATT_LOW ? BATT_C_RED : bolt ? BATT_C_CREAM : BATT_C_GREY;
    for (int i = 0; i < bars && i < BATT_BARS_MAX; i++) {
        put(out, &n, ox + 8 + 4 * i, oy + 8, 4, 8, bar_color);
    }
    return n;
}
