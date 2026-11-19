#include "volknob.h"
#include <math.h>
#include <stdio.h>

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
    k->x0 = k->lx = k->qx = x;
    k->y0 = k->ly = k->qy = y;
    k->t0 = k->last_t = k->q_t = t_ms;
    k->r0 = k->rmin = k->rmax = k->r1 = radius_of(x, y);
    k->ux = 0;
    k->uy = -1;
    k->arm_ms = -1;
    k->theta_last = angle_of(x, y);
    k->v = k->v0 = user_vol;
    k->state = dark ? VK_DEAD : k->r0 >= VOL_R_MIN_PX ? VK_EDGE : VK_CENTRE;
    k->cls = k->state;
    k->why = k->swhy = dark ? VK_WHY_DARK : VK_WHY_NONE;
    return 0;
}

void vk_swipe_frame(volknob_t *k, float ui_deg)
{
    static const double UX[4] = {0, 1, 0, -1}, UY[4] = {-1, 0, 1, 0};
    if (k->latched) {
        return;
    }
    long tenths = lround(ui_deg * 10.0);
    k->quad = (int)(((lround(ui_deg / 90.0) % 4) + 4) % 4);
    if (tenths % 900 == 0) {
        k->ux = UX[k->quad];
        k->uy = UY[k->quad];
        return;
    }
    double a = ui_deg * PI_D / 180.0;
    k->ux = sin(a);
    k->uy = -cos(a);
}

static int clamp_v(int v)
{
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static void kill(volknob_t *k, int why)
{
    k->state = VK_DEAD;
    k->why = why;
    if (k->swhy == VK_WHY_NONE) {
        k->swhy = why;
    }
}

/* One brightness ladder step per sample travel quantum; edges saturate and count the push. */
static int brt_step(volknob_t *k)
{
    const double pitch = (double)BRT_TRAVEL_PX / BRT_STEPS;
    double d = BRT_UP_SIGN * (k->up - k->up_last);
    k->up_last = k->up;
    int dir = d > 0 ? 1 : d < 0 ? -1 : 0;
    if (dir != 0 && k->dir != 0 && dir != k->dir) {
        k->rev++;
    }
    k->dir = dir != 0 ? dir : k->dir;
    k->bacc += d;
    int n = (int)(fabs(k->bacc) / pitch);
    n = k->bacc < 0 ? -n : n;
    k->bacc -= n * pitch;
    int nb = k->bi + n;
    if (nb > BRT_STEPS || (nb == BRT_STEPS && k->bacc > 0)) {
        k->push += nb - BRT_STEPS + k->bacc / pitch;
        nb = BRT_STEPS;
        k->bacc = 0;
    } else if (nb < 0 || (nb == 0 && k->bacc < 0)) {
        k->push += -nb - k->bacc / pitch;
        nb = 0;
        k->bacc = 0;
    } else if (dir != 0) {
        k->push = 0;
    }
    if (nb != 0 && nb != BRT_STEPS) {
        k->bumped = false;
    }
    int ev = nb != k->bi ? VK_EV_BRT : 0;
    k->bi = nb;
    if (k->push >= 1.0 && !k->bumped) {
        k->bumped = true;
        ev |= VK_EV_BRT_STOP;
    }
    return ev;
}

#define ARM_OK (-1)

/* Arming tests of a brightness candidate in order; returns ARM_OK, the drop reason, or VK_WHY_NONE while undecided. */
static int arm_test(volknob_t *k, int x, int y, bool jump, double lat, uint32_t t_ms, bool rim)
{
    if (jump) {
        return VK_WHY_JUMP;
    }
    if (fabs(lat) > BRT_LAT_PX && fabs(lat) * BRT_CONE_DEN > fabs(k->up) * BRT_CONE_NUM) {
        return VK_WHY_LATERAL;
    }
    if (t_ms - k->t_moved > BRT_ARM_MAX_MS) {
        return VK_WHY_SLOW;
    }
    if (fabs(k->up) < BRT_ARM_PX) {
        return VK_WHY_NONE;
    }
    if (rim) {
        /* mathematical model, placeholder: a rim stroke arms only when it runs at least as much inward as along the rim */
        double rx = (k->x0 - CENTRE_PX) / k->r0, ry = (k->y0 - CENTRE_PX) / k->r0, dx = x - k->x0, dy = y - k->y0;
        if (-(dx * rx + dy * ry) < fabs(dx * ry - dy * rx)) {
            return VK_WHY_TANGENT;
        }
    }
    return ARM_OK;
}

static int arm_bright(volknob_t *k, uint32_t t_ms)
{
    k->state = VK_BRIGHT;
    k->bright_ever = true;
    k->bacc = 0;
    k->up_last = k->up;
    k->arm_ms = (int)(t_ms - k->t_moved);
    k->bi = k->bi0;
    return VK_EV_BRT_ARM;
}

/* Arming rule of a centre slide, then the ladder while armed; rules in docs/display-power.md "User brightness". */
static int swipe_move(volknob_t *k, int x, int y, bool jump, double lat, uint32_t t_ms)
{
    if (k->state == VK_BRIGHT) {
        if (jump) {
            k->bi = k->bi0;
            kill(k, VK_WHY_JUMP);
            return VK_EV_BRT_CANCEL;
        }
        return brt_step(k);
    }
    int r = arm_test(k, x, y, jump, lat, t_ms, false);
    if (r == VK_WHY_NONE) {
        return 0;
    }
    k->ku = k->up;
    k->kl = lat;
    if (r == ARM_OK) {
        return arm_bright(k, t_ms);
    }
    kill(k, r);
    return 0;
}

/* The rim candidate of an edge touch: arms the swipe, or drops without touching the knob path. */
static int rim_move(volknob_t *k, int x, int y, bool jump, double lat, uint32_t t_ms)
{
    int r = arm_test(k, x, y, jump, lat, t_ms, true);
    if (r == VK_WHY_NONE) {
        return 0;
    }
    k->ku = k->up;
    k->kl = lat;
    if (r == ARM_OK) {
        return arm_bright(k, t_ms);
    }
    k->rim_dead = true;
    k->swhy = r;
    return 0;
}

int vk_move(volknob_t *k, int x, int y, uint32_t t_ms)
{
    if (k->state == VK_IDLE || (x == k->lx && y == k->ly)) {
        return 0;
    }
    k->lx = x;
    k->ly = y;
    k->latched = true;
    double disp = hypot(x - k->x0, y - k->y0), r = radius_of(x, y);
    k->max_disp = disp > k->max_disp ? disp : k->max_disp;
    k->rmin = r < k->rmin ? r : k->rmin;
    k->rmax = r > k->rmax ? r : k->rmax;
    k->r1 = r;
    double lat = (x - k->x0) * -k->uy + (y - k->y0) * k->ux;
    k->up = (x - k->x0) * k->ux + (y - k->y0) * k->uy;
    k->up_peak = fabs(k->up) > k->up_peak ? fabs(k->up) : k->up_peak;
    k->lat_peak = fabs(lat) > k->lat_peak ? fabs(lat) : k->lat_peak;
    uint32_t qdt = t_ms - k->q_t;
    double step = hypot(x - k->qx, y - k->qy);
    k->gap = qdt > k->gap ? qdt : k->gap;
    k->stp = step > k->stp ? step : k->stp;
    qdt = qdt > VOL_JUMP_DT_CAP_MS ? VOL_JUMP_DT_CAP_MS : qdt;
    bool jump = step > BRT_JUMP_MIN_PX + BRT_V_MAX_PX_S * qdt / 1000.0;
    k->qx = x;
    k->qy = y;
    k->q_t = t_ms;
    if (disp > TOUCH_SLOP_PX) {
        if (!k->moved && k->cls == VK_EDGE) {
            k->t_moved = t_ms;
        }
        k->moved = true;
    }
    if (k->state == VK_CENTRE && k->moved) {
        if (k->long_sent) {
            kill(k, VK_WHY_MOVED);
        } else {
            k->state = VK_SWIPE;
            k->slid = true;
            k->t_moved = t_ms;
        }
    }
    if (k->state == VK_SWIPE || k->state == VK_BRIGHT) {
        return swipe_move(k, x, y, jump, lat, t_ms);
    }
    if (k->state == VK_EDGE && k->moved && !k->rim_dead) {
        int ev = rim_move(k, x, y, jump, lat, t_ms);
        if (ev) {
            return ev;
        }
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

void vk_swipe_rebase(volknob_t *k, int index)
{
    k->bi = k->bi0 = index;
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
    } else if (k->state == VK_BRIGHT) {
        ev = VK_EV_BRT_COMMIT;
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

static const char *const CLS[] = {"idle", "dead", "centre", "edge", "armed", "swipe", "bright"};
static const char *const WHY[] = {"none", "dark", "moved", "zone", "jump", "lateral", "slow", "tangent"};
_Static_assert(sizeof(CLS) / sizeof(CLS[0]) == VK_STATE_N, "CLS is indexed by the classifier states");
_Static_assert(sizeof(WHY) / sizeof(WHY[0]) == VK_WHY_N, "WHY is indexed by the classifier drop reasons");

const char *vk_cls_name(int cls)
{
    return cls >= 0 && cls < VK_STATE_N ? CLS[cls] : "?";
}

const char *vk_why_name(int why)
{
    return why >= 0 && why < VK_WHY_N ? WHY[why] : "?";
}

static unsigned cap_ms(uint32_t v)
{
    return v > VK_MS_LOG_MAX ? VK_MS_LOG_MAX : (unsigned)v;
}

int vk_swipe_line(char *out, size_t cap, const volknob_t *k, int b0, int b, uint32_t now)
{
    return snprintf(out, cap, VK_SWIPE_LINE_FMT, vk_cls_name(k->cls), k->quad, vk_why_name(k->swhy), k->bright_ever, b0, b, k->lx - k->x0, k->ly - k->y0,
                    k->up, k->up_peak, k->lat_peak, k->arm_ms, k->r1, k->rev > VK_REV_LOG_MAX ? VK_REV_LOG_MAX : k->rev, cap_ms(k->gap), k->stp, k->ku, k->kl,
                    cap_ms(now - k->t0));
}
