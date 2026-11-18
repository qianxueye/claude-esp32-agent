#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

/* Board-motion detector on the 100 ms mean acceleration vector against a rest anchor. Model in docs/display-power.md. */

typedef enum { MOTION_REST, MOTION_MOVING, MOTION_LIFTED } motion_state_t;
enum { MOTION_EV_MOVING = 1, MOTION_EV_PICKUP = 2, MOTION_EV_REST = 4, MOTION_EV_REST_DESK = 8 };

typedef struct {
    float theta_on, theta_off, theta_pickup;
    bool use_noise; /* raise theta_on and theta_off to the estimated noise floor */
} motion_cfg_t;

/* Rest-noise estimator: Allan variance of ten 100 ms means per 1 s block, lower quartile over the last 64 blocks. */
typedef struct {
    float means[MOTION_NOISE_MEANS][3], acc[3], blocks[MOTION_NOISE_BLOCKS];
    int in_mean, n_means, n_blocks, head;
    bool tainted;
} motion_noise_t;

typedef struct {
    motion_cfg_t cfg;
    motion_noise_t noise;
    float buf[5][3], anchor[3], settle_mean[3], settle_max, d;
    int fill, idx, run_on, run_pickup, settle_n;
    bool anchor_ok, pa_was_on, pa_tail;
    uint32_t pa_off_at;
    motion_state_t state;
} motion_t;

motion_cfg_t motion_default_cfg(void);
void motion_init(motion_t *m, const motion_cfg_t *cfg);
/* One call per sample (axes in g, nominal IMU_PERIOD_MS apart); returns MOTION_EV_* bits. */
int motion_feed(motion_t *m, const float a[3], uint32_t now_ms, bool amp_on);

void motion_noise_init(motion_noise_t *n);
/* One raw sample; masked samples (amplifier on) discard the block they fall in. */
void motion_noise_feed(motion_noise_t *n, const float a[3], bool masked);
/* Estimated sigma of a 100 ms mean in g; 0 until 64 blocks have been seen. */
float motion_noise_sigma_w(const motion_noise_t *n);
/* Floor on theta_on: k times the deviation sigma that follows from sigma_w; 0 when not valid. */
float motion_noise_theta(const motion_noise_t *n);
