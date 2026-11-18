#include "motion.h"
#include <math.h>
#include "app_config.h"

_Static_assert(MOTION_WINDOW_SAMPLES == 5, "motion_t.buf is sized for this window");

motion_cfg_t motion_default_cfg(void)
{
    return (motion_cfg_t){MOTION_THETA_ON_G, MOTION_THETA_OFF_G, MOTION_THETA_PICKUP_G};
}

void motion_init(motion_t *m, const motion_cfg_t *cfg)
{
    *m = (motion_t){.cfg = cfg ? *cfg : motion_default_cfg()};
}

static void restart_settle(motion_t *m, const float g[3])
{
    for (int j = 0; j < 3; j++) {
        m->settle_mean[j] = g[j];
    }
    m->settle_n = 1;
}

/* Tracks the settle window; returns true once the mean has been steady for MOTION_REF_SAMPLES. */
static bool settled(motion_t *m, const float g[3])
{
    float dev = 0;
    for (int j = 0; j < 3; j++) {
        dev += (g[j] - m->settle_mean[j]) * (g[j] - m->settle_mean[j]);
    }
    if (sqrtf(dev) >= m->cfg.theta_off) {
        restart_settle(m, g);
        return false;
    }
    m->settle_n++;
    for (int j = 0; j < 3; j++) {
        m->settle_mean[j] += (g[j] - m->settle_mean[j]) / (float)m->settle_n;
    }
    return m->settle_n >= MOTION_REF_SAMPLES;
}

int motion_feed(motion_t *m, const float a[3], uint32_t now_ms, bool amp_on)
{
    for (int j = 0; j < 3; j++) {
        m->buf[m->idx][j] = a[j];
    }
    m->idx = (m->idx + 1) % MOTION_WINDOW_SAMPLES;
    if (m->fill < MOTION_WINDOW_SAMPLES) {
        m->fill++;
        if (m->fill < MOTION_WINDOW_SAMPLES) {
            return 0;
        }
    }
    float g[3] = {0, 0, 0};
    for (int i = 0; i < MOTION_WINDOW_SAMPLES; i++) {
        for (int j = 0; j < 3; j++) {
            g[j] += m->buf[i][j] / MOTION_WINDOW_SAMPLES;
        }
    }
    if (!m->anchor_ok) {
        for (int j = 0; j < 3; j++) {
            m->anchor[j] = g[j];
        }
        m->anchor_ok = true;
        return 0;
    }
    if (m->pa_was_on && !amp_on) {
        m->pa_off_at = now_ms;
        m->pa_tail = true;
    }
    m->pa_was_on = amp_on;
    bool masked = amp_on || (m->pa_tail && now_ms - m->pa_off_at < MOTION_PA_TAIL_MS);

    float dev = 0;
    for (int j = 0; j < 3; j++) {
        dev += (g[j] - m->anchor[j]) * (g[j] - m->anchor[j]);
    }
    float d = sqrtf(dev);
    m->d = d;
    int ev = 0;

    m->run_pickup = d >= m->cfg.theta_pickup ? m->run_pickup + 1 : 0;
    if (m->run_pickup >= MOTION_N_PICKUP && m->state != MOTION_LIFTED) {
        m->state = MOTION_LIFTED;
        m->run_on = 0;
        restart_settle(m, g);
        ev |= MOTION_EV_PICKUP;
        return ev;
    }
    if (m->state == MOTION_REST) {
        if (d < m->cfg.theta_off) {
            const float alpha = (float)IMU_PERIOD_MS / MOTION_TAU_REF_MS;
            for (int j = 0; j < 3; j++) {
                m->anchor[j] += (g[j] - m->anchor[j]) * alpha;
            }
        }
        m->run_on = (!masked && d >= m->cfg.theta_on) ? m->run_on + 1 : 0;
        if (m->run_on >= MOTION_N_ON) {
            m->state = MOTION_MOVING;
            m->run_on = 0;
            restart_settle(m, g);
            ev |= MOTION_EV_MOVING;
        }
    } else if (settled(m, g)) {
        m->state = MOTION_REST;
        for (int j = 0; j < 3; j++) {
            m->anchor[j] = m->settle_mean[j];
        }
        m->run_on = 0;
        m->run_pickup = 0;
        ev |= MOTION_EV_REST;
    }
    return ev;
}
