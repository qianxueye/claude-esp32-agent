#include <assert.h>
#include <stdio.h>
#include "volstate.h"

static void loaded(volst_t *s, int vol)
{
    vs_load(s, vol >= 0 ? VS_LOAD_FOUND : VS_LOAD_ABSENT, (uint32_t)vol);
}

static void test_s1_transient_never_stored(void)
{
    volst_t s;
    loaded(&s, 45);
    const uint8_t one[] = {0}, two[] = {0, 0};
    assert(!vs_host_set(&s, one, 1) && vs_effective(&s) == 0 && s.user == 45 && !vs_needs_save(&s));
    assert(!vs_host_set(&s, two, 2) && vs_effective(&s) == 0 && s.user == 45 && !vs_needs_save(&s));
    vs_session_reset(&s);
    assert(vs_effective(&s) == 45);
    const uint8_t bad[] = {10, 7}, three[] = {10, 1, 0};
    assert(!vs_host_set(&s, bad, 2) && !vs_host_set(&s, three, 3) && vs_effective(&s) == 45);
    const uint8_t big[] = {200};
    vs_host_set(&s, big, 1);
    assert(vs_effective(&s) == 100);
}

static void test_s2_seed_idempotent_and_loses(void)
{
    volst_t s;
    loaded(&s, -1);
    const uint8_t seed[] = {70, 2}, seed2[] = {20, 2};
    assert(!s.set && vs_host_set(&s, seed, 2) && s.set && vs_needs_save(&s) && s.user == 70);
    assert(!vs_host_set(&s, seed2, 2) && s.user == 70);
    loaded(&s, -1);
    vs_knob_arm(&s);
    vs_knob_set(&s, 33);
    assert(vs_knob_end(&s, false) && s.user == 33 && s.set);
    assert(!vs_host_set(&s, seed, 2) && s.user == 33);
    loaded(&s, 50);
    vs_knob_arm(&s);
    assert(!vs_host_set(&s, (const uint8_t[]){10, 1}, 2) && s.user == 50);
    vs_knob_set(&s, 60);
    assert(!vs_knob_end(&s, true) && s.user == 50);
}

static void test_knob_cancel_restores(void)
{
    volst_t s;
    loaded(&s, 50);
    vs_knob_arm(&s);
    vs_knob_set(&s, 80);
    assert(!vs_knob_end(&s, true) && s.user == 50 && !vs_needs_save(&s) && !s.knob);
    vs_knob_arm(&s);
    const uint8_t t[] = {5};
    vs_host_set(&s, t, 1);
    assert(vs_effective(&s) == 50);
    vs_knob_set(&s, 50);
    assert(!vs_knob_end(&s, false));
}

static void test_s3_save_and_load_rules(void)
{
    volst_t s;
    loaded(&s, 45);
    assert(s.set && s.user == 45 && s.stored == 45 && !vs_needs_save(&s));
    loaded(&s, -1);
    assert(!s.set && s.user == DEFAULT_VOLUME && s.stored == -1);
    vs_load(&s, VS_LOAD_FOUND, 200);
    assert(!s.set && s.user == DEFAULT_VOLUME);
    vs_load(&s, VS_LOAD_ABSENT, 0);
    assert(!s.set);
    loaded(&s, 45);
    uint32_t last = 0, t = 100000, writes = 0;
    for (int i = 0; i < 10; i++) {
        vs_knob_arm(&s);
        vs_knob_set(&s, 46 + i);
        vs_knob_end(&s, false);
        t += 400;
        assert(!vs_save_due(&s, t, last, false));
        if (vs_save_due(&s, t, last, true)) {
            vs_saved(&s, s.user);
            last = t;
            writes++;
        }
    }
    assert(writes == 1 && vs_needs_save(&s));
    t += VOL_SAVE_MIN_MS;
    assert(vs_save_due(&s, t, last, true));
    vs_knob_arm(&s);
    assert(!vs_save_due(&s, t, last, true));
    vs_knob_end(&s, false);
    vs_knob_arm(&s);
    vs_knob_set(&s, 99);
    vs_knob_end(&s, false);
    assert(vs_save_due(&s, 0xFFFFFF00u + VOL_SAVE_MIN_MS, 0xFFFFFF00u, true));
    assert(!vs_save_due(&s, 0xFFFFFF00u + VOL_SAVE_MIN_MS - 1, 0xFFFFFF00u, true));
}

/* Never due from t0 on: steps the clock past several write gaps. */
static bool never_due(const volst_t *s, uint32_t t0)
{
    for (uint32_t t = t0; t < t0 + 4 * VOL_SAVE_MIN_MS; t += VOL_SAVE_MIN_MS / 2) {
        if (vs_needs_save(s) || vs_save_due(s, t, t0 - VOL_SAVE_MIN_MS, true)) {
            return false;
        }
    }
    return true;
}

static void test_s4_read_error_never_writes(void)
{
    volst_t s;
    assert(!vs_load(&s, VS_LOAD_ERROR, 45) && !s.set && s.user == DEFAULT_VOLUME && never_due(&s, 100000));
    const uint8_t seed[] = {20, 2}, persist[] = {60, 1};
    assert(vs_host_set(&s, seed, 2) && s.set && s.user == 20 && never_due(&s, 200000));
    assert(vs_host_set(&s, persist, 2) && s.user == 60 && never_due(&s, 300000));
    vs_knob_arm(&s);
    vs_knob_set(&s, 33);
    assert(vs_knob_end(&s, false) && s.set && s.user == 33 && never_due(&s, 400000));
    vs_session_reset(&s);
    assert(s.set && never_due(&s, 500000));
}

static void test_s5_fresh_board_never_due(void)
{
    volst_t s;
    loaded(&s, -1);
    assert(never_due(&s, 100000));
    const uint8_t one[] = {30}, two[] = {40, 0};
    vs_host_set(&s, one, 1);
    vs_host_set(&s, two, 2);
    vs_session_reset(&s);
    assert(!s.set && never_due(&s, 200000));
    vs_knob_arm(&s);
    vs_knob_set(&s, 90);
    assert(!vs_knob_end(&s, true) && !s.set && s.user == DEFAULT_VOLUME && never_due(&s, 300000));
}

static void test_s6_save_in_flight_then_cancel(void)
{
    volst_t s;
    loaded(&s, 45);
    const uint8_t p50[] = {50, 1}, p60[] = {60, 1};
    uint32_t t = 100000, last = 0;
    vs_host_set(&s, p50, 2);
    assert(vs_save_due(&s, t, last, true));
    int writing = s.user;
    vs_host_set(&s, p60, 2);
    vs_knob_arm(&s);
    vs_knob_set(&s, 50);
    vs_saved(&s, writing);
    last = t;
    assert(writing == 50 && s.stored == 50 && !vs_save_due(&s, t + VOL_SAVE_MIN_MS, last, true));
    assert(!vs_knob_end(&s, true) && s.user == 60);
    t += VOL_SAVE_MIN_MS;
    assert(vs_save_due(&s, t, last, true) && s.user == 60);
    vs_saved(&s, s.user);
    assert(!vs_needs_save(&s));
}

static void test_s7_failed_write_stays_due(void)
{
    volst_t s;
    loaded(&s, 45);
    const uint8_t p50[] = {50, 1};
    vs_host_set(&s, p50, 2);
    uint32_t t = 100000;
    assert(vs_save_due(&s, t, 0, true));
    assert(!vs_save_due(&s, t + VOL_SAVE_MIN_MS - 1, t, true) && vs_save_due(&s, t + VOL_SAVE_MIN_MS, t, true));
    vs_saved(&s, 50);
    assert(!vs_needs_save(&s) && s.stored == 50);
}

int main(void)
{
    test_s1_transient_never_stored();
    test_s2_seed_idempotent_and_loses();
    test_knob_cancel_restores();
    test_s3_save_and_load_rules();
    test_s4_read_error_never_writes();
    test_s5_fresh_board_never_due();
    test_s6_save_in_flight_then_cancel();
    test_s7_failed_write_stays_due();
    puts("ok");
    return 0;
}
