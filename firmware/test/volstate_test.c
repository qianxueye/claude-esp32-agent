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

static void test_s8_write_failures_logged_once_per_streak(void)
{
    volst_t s;
    loaded(&s, 45);
    const bool outcome[] = {true, false, false, false, true, true, false, true};
    const int want[] = {VS_STREAK_SAME, VS_STREAK_START, VS_STREAK_SAME, VS_STREAK_SAME, VS_STREAK_END, VS_STREAK_SAME, VS_STREAK_START, VS_STREAK_END};
    for (int i = 0; i < 8; i++) {
        assert(vs_write_streak(&s, outcome[i]) == want[i]);
    }
}

/* Loads a brightness value the way bright.c does. */
static bool brt_loaded(volst_t *s, int result, uint32_t raw)
{
    return vs_load_range(s, result, raw, BRT_MIN_PERCENT, 100, DISP_ACTIVE_PERCENT);
}

static void test_s9_range_load(void)
{
    volst_t s;
    assert(brt_loaded(&s, VS_LOAD_FOUND, 49) && s.set && s.user == 49 && s.stored == 49 && !vs_needs_save(&s));
    const uint32_t bad[] = {5, 200, BRT_MIN_PERCENT - 1, 101};
    for (int i = 0; i < 4; i++) {
        assert(!brt_loaded(&s, VS_LOAD_FOUND, bad[i]) && !s.set && s.user == DISP_ACTIVE_PERCENT && never_due(&s, 100000));
    }
    assert(!brt_loaded(&s, VS_LOAD_ABSENT, 0) && !s.set && s.user == DISP_ACTIVE_PERCENT && never_due(&s, 100000));
    assert(!brt_loaded(&s, VS_LOAD_ERROR, 49) && s.ram && s.user == DISP_ACTIVE_PERCENT);
    vs_knob_arm(&s);
    vs_knob_set(&s, 64);
    assert(vs_knob_end(&s, false) && s.user == 64 && never_due(&s, 200000));
    /* The volume range accepts 5: the red proof for the brightness range. */
    assert(vs_load(&s, VS_LOAD_FOUND, 5) && s.user == 5);
}

static void test_s10_swipe_save_rules(void)
{
    volst_t s;
    brt_loaded(&s, VS_LOAD_FOUND, 49);
    uint32_t last = 0, t = 100000, writes = 0;
    for (int i = 0; i < 10; i++) {
        vs_knob_arm(&s);
        vs_knob_set(&s, 52 + i);
        assert(!vs_save_due(&s, t, last, true));
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
    vs_saved(&s, s.user);
    vs_knob_arm(&s);
    vs_knob_set(&s, 90);
    assert(!vs_knob_end(&s, true) && !vs_needs_save(&s));
    const uint8_t pin[] = {20}, persist[] = {20, VOL_MODE_PERSIST};
    vs_host_set(&s, pin, 1);
    assert(vs_effective(&s) == 20 && !vs_needs_save(&s));
    /* Treating the pin as a persist marks a write: the red proof. */
    assert(vs_host_set(&s, persist, 2) && vs_needs_save(&s));
}

static void test_s11_one_clock_for_both_keys(void)
{
    volst_t vol, brt;
    loaded(&vol, 45);
    brt_loaded(&brt, VS_LOAD_FOUND, 49);
    vs_host_set(&vol, (const uint8_t[]){50, VOL_MODE_PERSIST}, 2);
    vs_host_set(&brt, (const uint8_t[]){64, VOL_MODE_PERSIST}, 2);
    uint32_t shared = 0, own_vol = 0, own_brt = 0, t = 100000;
    int writes = 0, wrong_writes = 0;
    for (uint32_t u = t; u < t + VOL_SAVE_MIN_MS; u += LIFE_PERIOD_MS) {
        volst_t *keys[2] = {&vol, &brt};
        for (int i = 0; i < 2; i++) {
            if (vs_save_due(keys[i], u, shared, true)) {
                writes++;
                shared = u;
            }
        }
        wrong_writes += vs_save_due(&vol, u, own_vol, true) && (own_vol = u, true);
        wrong_writes += vs_save_due(&brt, u, own_brt, true) && (own_brt = u, true);
        if (u == t) {
            vs_saved(&vol, vol.user);
        }
    }
    assert(writes == 1 && vs_needs_save(&brt) && !vs_save_due(&brt, t + VOL_SAVE_MIN_MS - 1, shared, true));
    assert(vs_save_due(&brt, t + VOL_SAVE_MIN_MS, shared, true));
    /* A clock per key writes both within one gap: the red proof. */
    assert(wrong_writes == 2);
}

static void test_s12_fresh_board_unchanged_swipe_stores_nothing(void)
{
    volst_t s, wrong;
    assert(!brt_loaded(&s, VS_LOAD_ABSENT, 0) && !s.set && s.user == DISP_ACTIVE_PERCENT);
    vs_knob_arm(&s);
    vs_knob_set(&s, 100);
    wrong = s;
    assert(!vs_swipe_end(&s, false) && !s.set && !vs_needs_save(&s) && s.user == 100 && !s.knob);
    /* The plain knob commit marks the default for a write: the red proof. */
    assert(!vs_knob_end(&wrong, false) && wrong.set && vs_needs_save(&wrong));
}

static void test_s13_swipe_back_to_start_stores_nothing(void)
{
    volst_t s;
    brt_loaded(&s, VS_LOAD_FOUND, 49);
    vs_knob_arm(&s);
    vs_knob_set(&s, 55);
    vs_knob_set(&s, 49);
    assert(!vs_swipe_end(&s, false) && !vs_needs_save(&s) && s.stored == 49 && s.user == 49);
}

static void test_s14_swipe_change_is_stored(void)
{
    volst_t s;
    brt_loaded(&s, VS_LOAD_FOUND, 49);
    vs_knob_arm(&s);
    vs_knob_set(&s, 55);
    assert(vs_swipe_end(&s, false) && vs_needs_save(&s) && s.user == 55 && vs_save_due(&s, 100000, 100000 - VOL_SAVE_MIN_MS, true));
}

typedef int (*pick_fn)(bool a_due, bool b_due, int last);

/* Red proof for S15: volume whenever it is due, the order of two polls in a row. */
static int wrong_pick_first_key(bool a_due, bool b_due, int last)
{
    (void)last;
    return a_due ? VS_PICK_A : b_due ? VS_PICK_B : VS_PICK_NONE;
}

typedef struct {
    volst_t key[2];
    uint32_t shared, written_at[2], min_gap;
    int last, writes[2], attempts;
    bool fails[2], rotate_on_success_only;
} life_t;

static void life_init(life_t *l)
{
    *l = (life_t){.min_gap = UINT32_MAX};
    loaded(&l->key[0], 45);
    brt_loaded(&l->key[1], VS_LOAD_FOUND, 49);
}

/* One life tick as main.c runs it: peek both keys, pick one, and attempt only that key; the attempt stamps the clock and the last key. */
static void life_slot(life_t *l, uint32_t u, pick_fn pick)
{
    int p = pick(vs_save_due(&l->key[0], u, l->shared, true), vs_save_due(&l->key[1], u, l->shared, true), l->last);
    if (p == VS_PICK_NONE) {
        return;
    }
    int i = p == VS_PICK_A ? 0 : 1;
    assert(vs_save_due(&l->key[i], u, l->shared, true));
    if (l->attempts++ > 0) {
        l->min_gap = u - l->shared < l->min_gap ? u - l->shared : l->min_gap;
    }
    l->shared = u;
    bool wrote = !l->fails[i];
    if (wrote) {
        vs_saved(&l->key[i], l->key[i].user);
        l->writes[i]++;
        l->written_at[i] = u;
    }
    vs_write_streak(&l->key[i], wrote);
    if (wrote || !l->rotate_on_success_only) {
        l->last = p;
    }
}

/* Runs 60 s of life ticks with the volume changed again before every slot and the brightness changed once. */
static void life_run(life_t *l, uint32_t t, pick_fn pick)
{
    vs_host_set(&l->key[1], (const uint8_t[]){64, VOL_MODE_PERSIST}, 2);
    for (uint32_t u = t; u < t + 60000; u += LIFE_PERIOD_MS) {
        if (!vs_needs_save(&l->key[0])) {
            vs_host_set(&l->key[0], (const uint8_t[]){(uint8_t)(l->key[0].user == 50 ? 51 : 50), VOL_MODE_PERSIST}, 2);
        }
        life_slot(l, u, pick);
    }
}

static void test_s15_pick_alternates(void)
{
    const uint32_t t = 100000;
    life_t l, wrong;
    life_init(&l);
    life_run(&l, t, vs_save_pick);
    assert(l.writes[1] == 1 && l.written_at[1] <= t + VOL_SAVE_MIN_MS && l.writes[0] >= 10 && l.min_gap >= VOL_SAVE_MIN_MS);
    life_init(&wrong);
    life_run(&wrong, t, wrong_pick_first_key);
    /* Volume first whenever it is due starves the brightness for the whole minute: the red proof. */
    assert(wrong.writes[1] == 0 && vs_needs_save(&wrong.key[1]));
}

static void test_s16_pick_table(void)
{
    const int lasts[3] = {VS_PICK_NONE, VS_PICK_A, VS_PICK_B};
    for (int a = 0; a <= 1; a++) {
        for (int b = 0; b <= 1; b++) {
            for (int i = 0; i < 3; i++) {
                int p = vs_save_pick(a, b, lasts[i]);
                if (!a && !b) {
                    assert(p == VS_PICK_NONE);
                } else if (a != b) {
                    assert(p == (a ? VS_PICK_A : VS_PICK_B));
                } else {
                    assert(p == (lasts[i] == VS_PICK_A ? VS_PICK_B : VS_PICK_A));
                }
            }
        }
    }
    life_t l;
    life_init(&l);
    vs_host_set(&l.key[0], (const uint8_t[]){50, VOL_MODE_PERSIST}, 2);
    vs_host_set(&l.key[1], (const uint8_t[]){64, VOL_MODE_PERSIST}, 2);
    for (uint32_t u = 100000; u < 130000; u += LIFE_PERIOD_MS) {
        if (u % 7000 == 0) {
            vs_host_set(&l.key[u % 14000 == 0], (const uint8_t[]){(uint8_t)(40 + u % 20), VOL_MODE_PERSIST}, 2);
        }
        life_slot(&l, u, vs_save_pick);
    }
    assert(l.attempts >= 4 && l.min_gap >= VOL_SAVE_MIN_MS && !vs_needs_save(&l.key[0]) && !vs_needs_save(&l.key[1]));
    /* Writing every key the peek found due, without the pick, writes twice inside the gap: the red proof. */
    volst_t wrong[2];
    loaded(&wrong[0], 45);
    brt_loaded(&wrong[1], VS_LOAD_FOUND, 49);
    vs_host_set(&wrong[0], (const uint8_t[]){50, VOL_MODE_PERSIST}, 2);
    vs_host_set(&wrong[1], (const uint8_t[]){64, VOL_MODE_PERSIST}, 2);
    bool due[2] = {vs_save_due(&wrong[0], 100000, 0, true), vs_save_due(&wrong[1], 100000, 0, true)};
    assert(due[0] && due[1] && vs_save_pick(due[0], due[1], VS_PICK_NONE) == VS_PICK_A);
}

static void test_s17_failed_attempt_rotates(void)
{
    const uint32_t t = 100000;
    life_t l, wrong;
    life_init(&l);
    l.fails[0] = true;
    life_run(&l, t, vs_save_pick);
    assert(l.writes[0] == 0 && l.writes[1] == 1 && l.written_at[1] == t + VOL_SAVE_MIN_MS && l.min_gap >= VOL_SAVE_MIN_MS);
    life_init(&wrong);
    wrong.fails[0] = true;
    wrong.rotate_on_success_only = true;
    life_run(&wrong, t, vs_save_pick);
    /* Rotating on a successful write only retries the failing key forever: the red proof. */
    assert(wrong.writes[1] == 0 && wrong.attempts >= 10);
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
    test_s8_write_failures_logged_once_per_streak();
    test_s9_range_load();
    test_s10_swipe_save_rules();
    test_s11_one_clock_for_both_keys();
    test_s12_fresh_board_unchanged_swipe_stores_nothing();
    test_s13_swipe_back_to_start_stores_nothing();
    test_s14_swipe_change_is_stored();
    test_s15_pick_alternates();
    test_s16_pick_table();
    test_s17_failed_attempt_rotates();
    puts("ok");
    return 0;
}
