#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

/* Touch gesture classifier: tap, long press and the edge volume knob. Model in docs/protocol.md "Volume". */

enum { VK_EV_TAP = 1, VK_EV_LONG = 2, VK_EV_ARM = 4, VK_EV_VOL = 8, VK_EV_COMMIT = 16, VK_EV_CANCEL = 32 };
enum { VK_IDLE, VK_DEAD, VK_CENTRE, VK_EDGE, VK_ARMED };
enum { VK_WHY_NONE, VK_WHY_DARK, VK_WHY_MOVED, VK_WHY_ZONE, VK_WHY_JUMP };

typedef struct {
    int state, why, cls;
    int x0, y0, lx, ly;
    uint32_t t0, last_t;
    double theta_last, travel, acc, r0, max_disp, rmin, rmax;
    bool moved, long_sent, reseed, armed_ever, noarm;
    int v, v0;
} volknob_t;

void vk_init(volknob_t *k);
int vk_press(volknob_t *k, int x, int y, uint32_t t_ms, bool dark, uint8_t user_vol);
int vk_move(volknob_t *k, int x, int y, uint32_t t_ms);
/* Sets the volume baseline (v, v0) to the user volume the knob armed with. */
void vk_rebase(volknob_t *k, int v);
int vk_tick(volknob_t *k, uint32_t t_ms);
int vk_release(volknob_t *k, uint32_t t_ms);
/* The touch ended without a release: as release, but never a tap. */
int vk_lost(volknob_t *k);
