#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

/* Touch gesture classifier: tap, long press, the edge volume knob and the centre brightness swipe. Models in docs/protocol.md "Volume" and docs/display-power.md "User brightness". */

enum { VK_EV_TAP = 1, VK_EV_LONG = 2, VK_EV_ARM = 4, VK_EV_VOL = 8, VK_EV_COMMIT = 16, VK_EV_CANCEL = 32, VK_EV_BRT_ARM = 64, VK_EV_BRT = 128, VK_EV_BRT_STOP = 256, VK_EV_BRT_COMMIT = 512, VK_EV_BRT_CANCEL = 1024 };
enum { VK_IDLE, VK_DEAD, VK_CENTRE, VK_EDGE, VK_ARMED, VK_SWIPE, VK_BRIGHT, VK_STATE_N };
enum { VK_WHY_NONE, VK_WHY_DARK, VK_WHY_MOVED, VK_WHY_ZONE, VK_WHY_JUMP, VK_WHY_LATERAL, VK_WHY_SLOW, VK_WHY_N };

typedef struct {
    int state, why, cls;
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
