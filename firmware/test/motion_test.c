#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "app_config.h"
#include "motion.h"

#define PI_F 3.14159265358979323846
#define DT_MS IMU_PERIOD_MS
#define SAMPLES_24H (24L * 3600L * 1000L / DT_MS)

static uint64_t s_rng = 0x9E3779B97F4A7C15ULL;

static uint64_t rng_next(void)
{
    s_rng ^= s_rng >> 12;
    s_rng ^= s_rng << 25;
    s_rng ^= s_rng >> 27;
    return s_rng * 2685821657736338717ULL;
}

static double rng_uniform(void)
{
    return ((double)(rng_next() >> 11) + 0.5) / 9007199254740992.0;
}

static double rng_gauss(void)
{
    return sqrt(-2.0 * log(rng_uniform())) * cos(2.0 * PI_F * rng_uniform());
}

/* Survival function of a chi-square variable with 3 degrees of freedom. */
static double chi3_sf(double x)
{
    return erfc(sqrt(x / 2.0)) + sqrt(2.0 * x / PI_F) * exp(-x / 2.0);
}

typedef struct {
    motion_t m;
    uint32_t now;
    long n;
    int moving, pickup, rest;
    long first_moving, first_pickup, first_rest;
} rig_t;

static void rig_init(rig_t *r, const motion_cfg_t *cfg, uint32_t now0)
{
    motion_init(&r->m, cfg);
    r->now = now0;
    r->n = 0;
    r->moving = r->pickup = r->rest = 0;
    r->first_moving = r->first_pickup = r->first_rest = -1;
}

static int rig_feed(rig_t *r, const float a[3], bool amp)
{
    int ev = motion_feed(&r->m, a, r->now, amp);
    if (ev & MOTION_EV_MOVING) {
        r->moving++;
        if (r->first_moving < 0) {
            r->first_moving = r->n;
        }
    }
    if (ev & MOTION_EV_PICKUP) {
        r->pickup++;
        if (r->first_pickup < 0) {
            r->first_pickup = r->n;
        }
    }
    if (ev & MOTION_EV_REST) {
        r->rest++;
        if (r->first_rest < 0) {
            r->first_rest = r->n;
        }
    }
    r->now += DT_MS;
    r->n++;
    return ev;
}

/* Gravity along z plus white noise of the given per-sample sigma, in g. */
static void noisy(float a[3], double sigma)
{
    a[0] = (float)(sigma * rng_gauss());
    a[1] = (float)(sigma * rng_gauss());
    a[2] = 1.0f + (float)(sigma * rng_gauss());
}

#define SIGMA_W 0.0003
#define SIGMA_S (SIGMA_W * 2.2360679775) /* per-sample sigma giving SIGMA_W after the 5-sample mean */

static motion_cfg_t noise_cfg(double k, float *theta)
{
    double sigma_d = SIGMA_W * sqrt(1.0 + (double)MOTION_WINDOW_SAMPLES / MOTION_REF_SAMPLES);
    *theta = (float)(k * sigma_d);
    return (motion_cfg_t){.theta_on = *theta, .theta_off = 2.0f * *theta, .theta_pickup = 1.0f};
}

/* Counts MOVING events over 24 h of rest noise and the two-look statistic from the module's own D. */
static void rest_noise_24h(double k, int *events, long *pairs)
{
    float theta;
    motion_cfg_t cfg = noise_cfg(k, &theta);
    rig_t r;
    rig_init(&r, &cfg, 0);
    long pair = 0;
    bool prev_look = false;
    for (long i = 0; i < SAMPLES_24H; i++) {
        float a[3];
        noisy(a, SIGMA_S);
        rig_feed(&r, a, false);
        if (i % MOTION_WINDOW_SAMPLES == MOTION_WINDOW_SAMPLES - 1 && i > MOTION_REF_SAMPLES) {
            bool look = r.m.d >= theta;
            pair += look && prev_look;
            prev_look = look;
        }
    }
    *events = r.moving;
    *pairs = pair;
}

/*
 * Two consecutive non-overlapping 100 ms looks above theta have probability q^2 per look, q = P(chi2_3 >= (theta/sigma)^2).
 * The module's anchor is an exponential average, so its noise is sigma_s^2 a/(2-a) and the true sigma is a little below the model sigma_D.
 * Count over N looks is binomial; the band is 5 standard deviations sqrt(N q^2 (1-q^2)(1+2q)).
 */
static void test_rest_noise(void)
{
    const double looks = (double)SAMPLES_24H / MOTION_WINDOW_SAMPLES;
    const double alpha = (double)IMU_PERIOD_MS / MOTION_TAU_REF_MS;
    const double var_ratio = 1.0 + MOTION_WINDOW_SAMPLES * alpha / (2.0 - alpha);
    const double model_ratio = 1.0 + (double)MOTION_WINDOW_SAMPLES / MOTION_REF_SAMPLES;
    int events;
    long pairs;

    rest_noise_24h(4.44, &events, &pairs);
    assert(events <= 1);
    assert(pairs <= 1);

    rest_noise_24h(3.0, &events, &pairs);
    double q = chi3_sf(9.0 * model_ratio / var_ratio), mean = looks * q * q;
    double band = 5.0 * sqrt(looks * q * q * (1.0 - q * q) * (1.0 + 2.0 * q));
    printf("k=3: two-look pairs %ld expected %.0f +- %.0f, module events %d\n", pairs, mean, band, events);
    assert(fabs((double)pairs - mean) <= band);
    assert(events <= pairs);
    assert(mean > 300.0 && mean < 800.0);
}

static void test_defaults_at_rest(void)
{
    rig_t r;
    rig_init(&r, NULL, 0);
    for (long i = 0; i < SAMPLES_24H; i++) {
        float a[3];
        noisy(a, SIGMA_S);
        rig_feed(&r, a, false);
    }
    assert(r.moving == 0 && r.pickup == 0 && r.m.state == MOTION_REST);
}

static void settle_rest(rig_t *r, int samples)
{
    for (int i = 0; i < samples; i++) {
        float a[3];
        noisy(a, SIGMA_S);
        rig_feed(r, a, false);
    }
}

static void test_knock_and_impulses(void)
{
    rig_t r;
    rig_init(&r, NULL, 0);
    settle_rest(&r, 500);
    for (int knock = 0; knock < 10; knock++) {
        for (int i = 0; i < 500; i++) {
            float a[3];
            noisy(a, SIGMA_S);
            const int len = 150 / DT_MS;
            if (i < len) {
                double t = i * DT_MS / 1000.0, env = 0.5 * (1.0 - cos(2.0 * PI_F * i / len));
                a[2] += (float)(0.5 * env * sin(2.0 * PI_F * 30.0 * t));
            }
            rig_feed(&r, a, false);
        }
    }
    assert(r.moving == 0 && r.pickup == 0 && r.m.state == MOTION_REST);

    for (int i = 0; i < 60 * 1000 / DT_MS; i++) {
        float a[3];
        noisy(a, SIGMA_S);
        if (i % 10 == 0) {
            a[0] += 0.5f;
        }
        rig_feed(&r, a, false);
    }
    assert(r.moving == 0 && r.pickup == 0 && r.m.state == MOTION_REST);
}

static void test_speaker_mask(void)
{
    for (int masked = 0; masked <= 1; masked++) {
        rig_t r;
        rig_init(&r, NULL, 0);
        settle_rest(&r, 500);
        for (int i = 0; i < 5000 / DT_MS; i++) {
            float a[3];
            noisy(a, 0.1);
            rig_feed(&r, a, masked);
        }
        if (masked) {
            assert(r.moving == 0);
        } else {
            assert(r.moving >= 1);
        }
    }
}

/* A step offset that starts when the amplifier stops fires N_ON samples after the tail; the delay is the same across the clock wrap. */
static void test_tail_and_wrap(void)
{
    long first[2];
    const uint32_t starts[2] = {1000u, 0u - 50u * DT_MS};
    for (int v = 0; v < 2; v++) {
        rig_t r;
        rig_init(&r, NULL, starts[v]);
        settle_rest(&r, 300);
        for (int i = 0; i < 10; i++) {
            float a[3] = {0, 0, 1};
            rig_feed(&r, a, true);
        }
        for (int i = 0; i < 100; i++) {
            float a[3] = {0.02f, 0, 1};
            rig_feed(&r, a, false);
        }
        assert(r.moving == 1);
        first[v] = r.first_moving - 310;
    }
    assert(first[0] == first[1]);
    assert(first[0] >= MOTION_PA_TAIL_MS / DT_MS + MOTION_N_ON - 1);
}

static void tilt(float a[3], double deg)
{
    double rad = deg * PI_F / 180.0;
    a[0] = 0;
    a[1] = (float)sin(rad);
    a[2] = (float)cos(rad);
}

static void test_pickup_tremor_setdown(void)
{
    rig_t r;
    rig_init(&r, NULL, 0);
    settle_rest(&r, 500);
    long onset = r.n;
    const int ramp = 300 / DT_MS;
    for (int i = 0; i < 60 * 1000 / DT_MS; i++) {
        float a[3];
        tilt(a, 15.0 * (i < ramp ? (double)i / ramp : 1.0));
        double t = i * DT_MS / 1000.0;
        a[0] += (float)(0.04 * sin(2.0 * PI_F * 6.0 * t) + 0.02 * sin(2.0 * PI_F * 9.3 * t + 1.0) + SIGMA_S * rng_gauss());
        a[1] += (float)(0.04 * sin(2.0 * PI_F * 7.1 * t + 2.0));
        rig_feed(&r, a, false);
    }
    assert(r.first_moving >= 0 && (r.first_moving - onset) * DT_MS <= 400);
    assert(r.first_pickup >= 0 && (r.first_pickup - onset) * DT_MS <= 900);
    assert(r.pickup == 1 && r.rest == 0 && r.m.state == MOTION_LIFTED);

    long down = r.n;
    for (int i = 0; i < 6000 / DT_MS; i++) {
        float a[3];
        tilt(a, 15.0 * (i < ramp ? 1.0 - (double)i / ramp : 0.0));
        a[0] += (float)(SIGMA_S * rng_gauss());
        rig_feed(&r, a, false);
    }
    assert(r.rest == 1 && r.m.state == MOTION_REST);
    assert((r.first_rest - down) * DT_MS <= 3500);
}

static void test_pickup_under_speaker(void)
{
    rig_t r;
    rig_init(&r, NULL, 0);
    settle_rest(&r, 500);
    for (int i = 0; i < 2000 / DT_MS; i++) {
        float a[3];
        tilt(a, 20.0);
        a[0] += (float)(0.05 * rng_gauss());
        rig_feed(&r, a, true);
    }
    assert(r.pickup == 1);
}

static void test_drift(void)
{
    rig_t r;
    rig_init(&r, NULL, 0);
    for (long i = 0; i < 30L * 60L * 1000L / DT_MS; i++) {
        float a[3];
        noisy(a, SIGMA_S);
        a[2] += (float)(0.001 / 60.0 * (double)(i * DT_MS) / 1000.0);
        rig_feed(&r, a, false);
    }
    assert(r.moving == 0 && r.pickup == 0 && r.m.state == MOTION_REST);
}

/* Feeds 64 one-second blocks of rest noise; a block is disturbed with probability p by extra white noise of 3 to 10 times the rest sigma. */
static float estimate_sigma_w(double p, float *q25_over_var)
{
    motion_noise_t n;
    motion_noise_init(&n);
    for (int b = 0; b < MOTION_NOISE_BLOCKS; b++) {
        double extra = rng_uniform() < p ? SIGMA_S * (3.0 + 7.0 * rng_uniform()) : 0.0;
        for (int i = 0; i < 50; i++) {
            float a[3];
            noisy(a, SIGMA_S);
            if (extra > 0.0) {
                a[0] += (float)(extra * rng_gauss());
                a[1] += (float)(extra * rng_gauss());
                a[2] += (float)(extra * rng_gauss());
            }
            motion_noise_feed(&n, a, false);
        }
    }
    float sw = motion_noise_sigma_w(&n);
    *q25_over_var = sw * sw * MOTION_NOISE_C / (float)(SIGMA_W * SIGMA_W);
    return sw;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void estimator_stats(double p, int trials, double *median_ratio, double *frac_within5)
{
    static float ratio[8000];
    float q;
    int within = 0;
    for (int t = 0; t < trials; t++) {
        ratio[t] = estimate_sigma_w(p, &q) / (float)SIGMA_W;
        within += fabsf(ratio[t] - 1.0f) <= 0.05f;
    }
    qsort(ratio, (size_t)trials, sizeof ratio[0], cmp_float);
    *median_ratio = ratio[trials / 2];
    *frac_within5 = (double)within / trials;
}

static void test_noise_estimator(void)
{
    enum { TRIALS = 4000 };
    static float v[TRIALS];
    float q;
    for (int t = 0; t < TRIALS; t++) {
        estimate_sigma_w(0.0, &q);
        v[t] = q;
    }
    qsort(v, TRIALS, sizeof v[0], cmp_float);
    double c_sim = v[TRIALS / 2];
    printf("c by simulation %.4f (configured %.4f)\n", c_sim, (double)MOTION_NOISE_C);
    /*
     * One block estimate has about 3 % relative spread in sigma, so a +-5 % band holds for about 90 % of 64-block windows (1.65 sigma).
     * A rank statistic cannot ignore disturbance it cannot see: with a share p of blocks disturbed, the lower quartile of 64 sits at
     * rank 16 among about 64(1-p) clean blocks, so sigma is over-estimated by about 5 % at p = 0.3 and 30 % at p = 0.7, never under-estimated.
     */
    double med, frac;
    estimator_stats(0.0, TRIALS, &med, &frac);
    printf("clean: median sigma ratio %.3f, within 5%% in %.1f%% of trials\n", med, frac * 100);
    assert(fabs(med - 1.0) <= 0.01 && frac >= 0.85);
    assert(fabs(c_sim - MOTION_NOISE_C) / c_sim <= 0.01);
    estimator_stats(0.3, TRIALS, &med, &frac);
    printf("30%% disturbed: median sigma ratio %.3f\n", med);
    assert(med >= 1.0 && med <= 1.10);
    estimator_stats(0.7, TRIALS, &med, &frac);
    printf("70%% disturbed: median sigma ratio %.3f\n", med);
    assert(med >= 1.2 && med <= 1.4);
}

/* Samples spent in MOVING during an hour of rest noise far above the placeholder threshold, counted after the estimator has 64 blocks. */
static long noisy_rest_moving(bool use_noise, double sigma_w)
{
    motion_cfg_t cfg = motion_default_cfg();
    cfg.use_noise = use_noise;
    rig_t r;
    rig_init(&r, &cfg, 0);
    long moving = 0;
    for (long i = 0; i < 3600L * 1000L / DT_MS; i++) {
        float a[3];
        noisy(a, sigma_w * 2.2360679775);
        rig_feed(&r, a, false);
        moving += i > 70L * 1000L / DT_MS && r.m.state == MOTION_MOVING;
    }
    return moving;
}

static void test_noise_floor_raises_threshold(void)
{
    long without = noisy_rest_moving(false, 0.008);
    long with = noisy_rest_moving(true, 0.008);
    printf("noisy rest, one hour: %ld samples in MOVING without the floor, %ld with it\n", without, with);
    assert(without >= 100000);
    assert(with == 0);
    assert(motion_default_cfg().use_noise == (MOTION_SMALL_WAKE_ENABLED != 0));
}

int main(void)
{
    test_rest_noise();
    test_defaults_at_rest();
    test_knock_and_impulses();
    test_speaker_mask();
    test_tail_and_wrap();
    test_pickup_tremor_setdown();
    test_pickup_under_speaker();
    test_drift();
    test_noise_estimator();
    test_noise_floor_raises_threshold();
    puts("ok");
    return 0;
}
