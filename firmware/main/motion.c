#include "motion.h"
#include <math.h>
#include "app_config.h"

_Static_assert(MOTION_WINDOW_SAMPLES == 5, "motion_t.buf is sized for this window");

motion_cfg_t motion_default_cfg(void)
{
    return (motion_cfg_t){MOTION_THETA_ON_G, MOTION_THETA_OFF_G, MOTION_THETA_PICKUP_G, MOTION_SMALL_WAKE_ENABLED != 0};
}

void motion_init(motion_t *m, const motion_cfg_t *cfg)
{
    *m = (motion_t){.cfg = cfg ? *cfg : motion_default_cfg()};
}

void motion_noise_init(motion_noise_t *n)
{
    *n = (motion_noise_t){0};
}

void motion_noise_feed(motion_noise_t *n, const float a[3], bool masked)
{
    n->tainted = n->tainted || masked;
    for (int j = 0; j < 3; j++) {
        n->acc[j] += a[j];
    }
    if (++n->in_mean < MOTION_WINDOW_SAMPLES) {
        return;
    }
    for (int j = 0; j < 3; j++) {
        n->means[n->n_means][j] = n->acc[j] / MOTION_WINDOW_SAMPLES;
        n->acc[j] = 0;
    }
    n->in_mean = 0;
    if (++n->n_means < MOTION_NOISE_MEANS) {
        return;
    }
    n->n_means = 0;
    if (n->tainted) {
        n->tainted = false;
        return;
    }
    float sum = 0;
    for (int k = 0; k + 1 < MOTION_NOISE_MEANS; k++) {
        for (int j = 0; j < 3; j++) {
            float d = n->means[k + 1][j] - n->means[k][j];
            sum += d * d;
        }
    }
    n->blocks[n->head] = sum / (2.0f * 3.0f * (MOTION_NOISE_MEANS - 1));
    n->head = (n->head + 1) % MOTION_NOISE_BLOCKS;
    if (n->n_blocks < MOTION_NOISE_BLOCKS) {
        n->n_blocks++;
    }
}

float motion_noise_sigma_w(const motion_noise_t *n)
{
    if (n->n_blocks < MOTION_NOISE_BLOCKS) {
        return 0.0f;
    }
    float v[MOTION_NOISE_BLOCKS];
    for (int i = 0; i < MOTION_NOISE_BLOCKS; i++) {
        float x = n->blocks[i];
        int k = i;
        while (k > 0 && v[k - 1] > x) {
            v[k] = v[k - 1];
            k--;
        }
        v[k] = x;
    }
    return sqrtf(v[MOTION_NOISE_RANK - 1] / MOTION_NOISE_C);
}

float motion_noise_theta(const motion_noise_t *n)
{
    return MOTION_NOISE_K * motion_noise_sigma_w(n) * sqrtf(1.0f + (float)MOTION_WINDOW_SAMPLES / MOTION_REF_SAMPLES);
}

static float theta_on(const motion_t *m)
{
    float t = m->cfg.use_noise ? motion_noise_theta(&m->noise) : 0.0f;
    return t > m->cfg.theta_on ? t : m->cfg.theta_on;
}

static float theta_off(const motion_t *m)
{
    float t = m->cfg.use_noise ? motion_noise_theta(&m->noise) : 0.0f;
    return t > m->cfg.theta_off ? t : m->cfg.theta_off;
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
    if (sqrtf(dev) >= theta_off(m)) {
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
    if (m->pa_was_on && !amp_on) {
        m->pa_off_at = now_ms;
        m->pa_tail = true;
    }
    m->pa_was_on = amp_on;
    bool masked = amp_on || (m->pa_tail && now_ms - m->pa_off_at < MOTION_PA_TAIL_MS);
    motion_noise_feed(&m->noise, a, masked);
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
        if (d < theta_off(m)) {
            const float alpha = (float)IMU_PERIOD_MS / MOTION_TAU_REF_MS;
            for (int j = 0; j < 3; j++) {
                m->anchor[j] += (g[j] - m->anchor[j]) * alpha;
            }
        }
        m->run_on = (!masked && d >= theta_on(m)) ? m->run_on + 1 : 0;
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
