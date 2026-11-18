#include "keydebounce.h"

void keydeb_init(keydeb_t *k)
{
    k->down = 0;
    k->up = 0;
    k->sent = false;
}

bool keydeb_poll(keydeb_t *k, bool pressed, int polls)
{
    if (pressed) {
        k->up = 0;
        if (k->down < polls) {
            k->down++;
        }
        if (k->down >= polls && !k->sent) {
            k->sent = true;
            return true;
        }
    } else {
        k->down = 0;
        if (++k->up >= polls) {
            k->sent = false;
            k->up = polls;
        }
    }
    return false;
}

bool keydeb_poll_read(keydeb_t *k, bool read_ok, bool pressed, int polls)
{
    return read_ok && keydeb_poll(k, pressed, polls);
}
