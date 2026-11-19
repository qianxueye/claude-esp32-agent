#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_config.h"

/* Touch gesture classifier: tap, long press, the edge volume knob and the brightness swipe. Models in docs/protocol.md "Volume" and docs/display-power.md "User brightness". */

enum { VK_EV_TAP = 1, VK_EV_LONG = 2, VK_EV_ARM = 4, VK_EV_VOL = 8, VK_EV_COMMIT = 16, VK_EV_CANCEL = 32, VK_EV_BRT_ARM = 64, VK_EV_BRT = 128, VK_EV_BRT_STOP = 256, VK_EV_BRT_COMMIT = 512, VK_EV_BRT_CANCEL = 1024 };
enum { VK_IDLE, VK_DEAD, VK_CENTRE, VK_EDGE, VK_ARMED, VK_SWIPE, VK_BRIGHT, VK_STATE_N };
enum { VK_WHY_NONE, VK_WHY_DARK, VK_WHY_MOVED, VK_WHY_ZONE, VK_WHY_JUMP, VK_WHY_LATERAL, VK_WHY_SLOW, VK_WHY_TANGENT, VK_WHY_N };

typedef struct {
    int state, why, cls;
    int swhy; /* the swipe candidate's drop reason; why keeps the knob's */
    int x0, y0, lx, ly;
    uint32_t t0, last_t;
    double theta_last, travel, acc, r0, max_disp, rmin, rmax;
    bool moved, long_sent, reseed, armed_ever, noarm;
    int v, v0;
    double ux, uy; /* UI-up unit vector in the touch frame, latched at press */
    double up, up_peak, lat_peak, up_last, r1;
    double bacc; /* pixels of UI-up travel not yet turned into a ladder step */
    double push; /* ladder steps of travel past an end */
    int quad, bi, bi0, rev, dir, arm_ms, qx, qy;
    uint32_t t_moved, q_t;
    bool latched, slid, bright_ever, bumped;
    bool rim_dead; /* an edge touch's brightness candidate has dropped */
    uint32_t gap; /* largest time between changed samples, press included */
    double stp, ku, kl; /* largest step; up and lat at the candidate's arm or drop sample */
} volknob_t;

void vk_init(volknob_t *k);
int vk_press(volknob_t *k, int x, int y, uint32_t t_ms, bool dark, uint8_t user_vol);
/* Sets the UI angle the swipe is read in; call right after vk_press. Ignored once the point has changed. */
void vk_swipe_frame(volknob_t *k, float ui_deg);
int vk_move(volknob_t *k, int x, int y, uint32_t t_ms);
/* Sets the volume baseline (v, v0) to the user volume the knob armed with. */
void vk_rebase(volknob_t *k, int v);
/* Sets the ladder index (bi, bi0) the swipe armed with. */
void vk_swipe_rebase(volknob_t *k, int index);
int vk_tick(volknob_t *k, uint32_t t_ms);
int vk_release(volknob_t *k, uint32_t t_ms);
/* The touch ended without a release: as release, but never a tap. */
int vk_lost(volknob_t *k);
const char *vk_cls_name(int cls);
const char *vk_why_name(int why);

#define VK_SWIPE_LINE_FMT "swipe: end cls=%s q=%d why=%s arm=%d b0=%d b=%d dx=%d dy=%d up=%.0f pk=%.0f lat=%.0f arm_ms=%d r1=%.0f rev=%d gap=%u stp=%.0f ku=%.0f kl=%.0f dur=%u"
#define VK_REV_LOG_MAX 999 /* plain configuration: log cap on rev */
#define VK_MS_LOG_MAX 999999u /* plain configuration: log cap on gap and dur, about 17 minutes */
/* Formats the swipe line of a touch that ended at now; b0 and b are percent or -1. rev, gap and dur are capped at the *_LOG_MAX
   values so the line fits LOG_LINE_MAX_BYTES with its log prefix, checked by brtswipe_test. */
int vk_swipe_line(char *out, size_t cap, const volknob_t *k, int b0, int b, uint32_t now);
