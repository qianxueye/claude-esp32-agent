#include "volknob.h"
#include <math.h>

#define PI_D 3.14159265358979323846
#define CENTRE_PX ((LCD_SIZE - 1) / 2.0)

static double angle_of(int x, int y)
{
    return atan2(y - CENTRE_PX, x - CENTRE_PX) * 180.0 / PI_D;
}

static double radius_of(int x, int y)
{
    return hypot(x - CENTRE_PX, y - CENTRE_PX);
}

static double wrap_delta(double d)
{
    return fmod(fmod(d + 540.0, 360.0) + 360.0, 360.0) - 180.0;
}

void vk_init(volknob_t *k)
{
    *k = (volknob_t){.state = VK_IDLE};
}

int vk_press(volknob_t *k, int x, int y, uint32_t t_ms, bool dark, uint8_t user_vol)
{
    vk_init(k);
    k->x0 = k->lx = x;
    k->y0 = k->ly = y;
    k->t0 = k->last_t = t_ms;
    k->r0 = k->rmin = k->rmax = radius_of(x, y);
    k->theta_last = angle_of(x, y);
    k->v = k->v0 = user_vol;
    k->state = dark ? VK_DEAD : k->r0 >= VOL_R_MIN_PX ? VK_EDGE : VK_CENTRE;
    k->cls = k->state;
    k->why = dark ? VK_WHY_DARK : VK_WHY_NONE;
    return 0;
}

static int clamp_v(int v)
{
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static void kill(volknob_t *k, int why)
{
    k->state = VK_DEAD;
    k->why = why;
}

int vk_move(volknob_t *k, int x, int y, uint32_t t_ms)
{
    if (k->state == VK_IDLE || (x == k->lx && y == k->ly)) {
        return 0;
    }
    k->lx = x;
    k->ly = y;
    double disp = hypot(x - k->x0, y - k->y0), r = radius_of(x, y);
    k->max_disp = disp > k->max_disp ? disp : k->max_disp;
    k->rmin = r < k->rmin ? r : k->rmin;
    k->rmax = r > k->rmax ? r : k->rmax;
    if (disp > TOUCH_SLOP_PX) {
        k->moved = true;
    }
    if (k->state == VK_CENTRE && k->moved) {
        kill(k, VK_WHY_MOVED);
    }
    if (k->state != VK_EDGE && k->state != VK_ARMED) {
        return 0;
    }
    if (k->state == VK_ARMED && r < VOL_R_TRACK_PX) {
        k->reseed = true;
        return 0;
    }
    if (k->state == VK_EDGE && (k->noarm || r < VOL_R_MIN_PX)) {
        /* inside the zone the touch can no longer arm, but may still be a tap */
        k->noarm = true;
        k->why = VK_WHY_ZONE;
        return 0;
    }
    double th = angle_of(x, y);
    if (k->reseed) {
        k->reseed = false;
        k->theta_last = th;
        k->last_t = t_ms;
        return 0;
    }
    double d = wrap_delta(th - k->theta_last), dt = t_ms - k->last_t;
    dt = dt > VOL_JUMP_DT_CAP_MS ? VOL_JUMP_DT_CAP_MS : dt;
    k->theta_last = th;
    k->last_t = t_ms;
    if (fabs(d) > VOL_JUMP_MIN_DEG + VOL_OMEGA_MAX_DPS * dt / 1000.0) {
        if (k->state == VK_ARMED) {
            k->v = k->v0;
            kill(k, VK_WHY_JUMP);
            return VK_EV_CANCEL;
        }
        kill(k, VK_WHY_JUMP);
        return 0;
    }
    if (k->state == VK_EDGE) {
        k->travel += d;
        if (fabs(k->travel) < VOL_ARM_DEG) {
            return 0;
        }
        k->state = VK_ARMED;
        k->armed_ever = true;
        k->acc = 0;
        k->v = k->v0;
        return VK_EV_ARM;
    }
    k->travel += d;
    k->acc += VOL_CW_SIGN * d * 100.0 / VOL_SWEEP_DEG;
    int n = (int)k->acc, nv = clamp_v(k->v + n);
    k->acc -= n;
    if (nv >= 100 && k->acc > 0) {
        k->acc = 0;
    }
    if (nv <= 0 && k->acc < 0) {
        k->acc = 0;
    }
    int ev = nv != k->v ? VK_EV_VOL : 0;
    k->v = nv;
    return ev;
}

void vk_rebase(volknob_t *k, int v)
{
    k->v = k->v0 = v;
}

int vk_tick(volknob_t *k, uint32_t t_ms)
{
    if (k->state == VK_CENTRE && !k->moved && !k->long_sent && t_ms - k->t0 >= LONG_PRESS_MS) {
        k->long_sent = true;
        return VK_EV_LONG;
    }
    return 0;
}

static int end_touch(volknob_t *k, uint32_t t_ms, bool tap_ok)
{
    int ev = 0;
    if (k->state == VK_ARMED) {
        ev = VK_EV_COMMIT;
    } else if ((k->state == VK_CENTRE || k->state == VK_EDGE) && tap_ok && !k->moved && !k->long_sent &&
               t_ms - k->t0 < TAP_MAX_MS) {
        ev = VK_EV_TAP;
    }
    k->state = VK_IDLE;
    return ev;
}

int vk_release(volknob_t *k, uint32_t t_ms)
{
    return end_touch(k, t_ms, true);
}

int vk_lost(volknob_t *k)
{
    return end_touch(k, 0, false);
}
