#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

/* Board-motion detector on the 100 ms mean acceleration vector against a rest anchor. Model in docs/display-power.md. */

typedef enum { MOTION_REST, MOTION_MOVING, MOTION_LIFTED } motion_state_t;
enum { MOTION_EV_MOVING = 1, MOTION_EV_PICKUP = 2, MOTION_EV_REST = 4, MOTION_EV_SETDOWN = 8 };

typedef struct {
    float theta_on, theta_off, theta_pickup;
} motion_cfg_t;

typedef struct {
    motion_cfg_t cfg;
    float buf[5][3], anchor[3], settle_mean[3], settle_max, d, rest_spread;
    int fill, idx, run_on, run_pickup, settle_n;
    bool anchor_ok, pa_was_on, pa_tail;
    uint32_t pa_off_at;
    motion_state_t state, rest_from;
} motion_t;

motion_cfg_t motion_default_cfg(void);
void motion_init(motion_t *m, const motion_cfg_t *cfg);
/* Restarts the detector and keeps the configuration. */
void motion_reset(motion_t *m);
/* One call per sample (axes in g, nominal IMU_PERIOD_MS apart); returns MOTION_EV_* bits. */
/* A REST that ends a LIFTED spell also carries MOTION_EV_SETDOWN; rest_spread (g) and rest_from describe the settle window of any REST. */
int motion_feed(motion_t *m, const float a[3], uint32_t now_ms, bool amp_on);
