#include <assert.h>
#include <stdio.h>
#include "app_config.h"
#include "battstate.h"

#define PRESENT 0x08
#define VBUS 0x20
#define DIR(d) ((d) << 5)
enum { STANDBY, CHARGE, DISCHARGE };

static batt_out_t read_at(batt_state_t *s, uint32_t t, uint8_t r00, uint8_t r01, int pct)
{
    batt_in_t in = {.ok = true, .reg00 = r00, .reg01 = r01, .pct = (uint8_t)pct, .now_ms = t};
    return batt_step(s, &in);
}

static batt_out_t fail_at(batt_state_t *s, uint32_t t)
{
    batt_in_t in = {.ok = false, .now_ms = t};
    return batt_step(s, &in);
}

static batt_out_t on_battery(int pct)
{
    batt_state_t s;
    batt_init(&s);
    return read_at(&s, 0, PRESENT, DIR(DISCHARGE), pct);
}

static void test_absent_hides(void)
{
    batt_state_t s;
    batt_init(&s);
    assert(read_at(&s, 0, 0, DIR(DISCHARGE), 50).look == BATT_HIDDEN);
}

static void test_vbus_without_battery_hides(void)
{
    batt_state_t s;
    batt_init(&s);
    assert(read_at(&s, 0, VBUS, DIR(STANDBY) | 5, 0).look == BATT_HIDDEN);
}

static void test_unknown_percentage_has_no_bars(void)
{
    batt_out_t o = on_battery(101);
    assert(o.look == BATT_NORMAL && o.bars == 0);
}

static void test_bar_steps(void)
{
    static const int PCT[] = {0, 1, 25, 26, 50, 75, 76, 100};
    static const int BARS[] = {1, 1, 1, 2, 2, 3, 4, 4};
    for (int i = 0; i < 8; i++) {
        batt_out_t o = on_battery(PCT[i]);
        if (PCT[i] <= BATT_LOW_ENTER_PCT) {
            assert(o.look == BATT_LOW && o.bars == 1);
        } else {
            assert(o.look == BATT_NORMAL && o.bars == BARS[i]);
        }
    }
}

static void test_low_enters_at_20_not_21(void)
{
    assert(on_battery(20).look == BATT_LOW);
    assert(on_battery(21).look == BATT_NORMAL);
}

static void test_low_leaves_at_25_not_24(void)
{
    batt_state_t s;
    batt_init(&s);
    read_at(&s, 0, PRESENT, DIR(DISCHARGE), 20);
    assert(read_at(&s, 2000, PRESENT, DIR(DISCHARGE), 24).look == BATT_LOW);
    assert(read_at(&s, 4000, PRESENT, DIR(DISCHARGE), 25).look == BATT_NORMAL);
}

static void test_charging_overrides_low(void)
{
    batt_state_t s;
    batt_init(&s);
    read_at(&s, 0, PRESENT, DIR(DISCHARGE), 10);
    batt_out_t o = read_at(&s, 2000, PRESENT | VBUS, DIR(CHARGE) | 2, 10);
    assert(o.look == BATT_CHARGING && o.bars == 1);
    assert(read_at(&s, 4000, PRESENT, DIR(DISCHARGE), 22).look == BATT_NORMAL);
}

static void test_done_shows_four_bars(void)
{
    batt_state_t s;
    batt_init(&s);
    batt_out_t o = read_at(&s, 0, PRESENT | VBUS, DIR(STANDBY) | 4, 97);
    assert(o.look == BATT_DONE && o.bars == 4);
}

static void test_vbus_not_charging_is_normal(void)
{
    batt_state_t s;
    batt_init(&s);
    assert(read_at(&s, 0, PRESENT | VBUS, DIR(STANDBY) | 5, 60).look == BATT_NORMAL);
}

static void test_stale_after_ten_seconds_of_failures(void)
{
    batt_state_t s;
    batt_init(&s);
    read_at(&s, 0, PRESENT, DIR(DISCHARGE), 60);
    assert(fail_at(&s, BATT_STALE_MS - 1).look == BATT_NORMAL);
    assert(fail_at(&s, BATT_STALE_MS).look == BATT_HIDDEN);
}

static void test_never_read_is_hidden(void)
{
    batt_state_t s;
    batt_init(&s);
    assert(fail_at(&s, 0).look == BATT_HIDDEN);
}

static void test_first_read_never_wakes_and_plug_in_does(void)
{
    batt_state_t s;
    batt_init(&s);
    assert(!read_at(&s, 0, PRESENT | VBUS, DIR(CHARGE) | 2, 50).wake);
    assert(!read_at(&s, 2000, PRESENT, DIR(DISCHARGE), 50).wake);
    assert(read_at(&s, 4000, PRESENT | VBUS, DIR(CHARGE) | 2, 50).wake);
    assert(!read_at(&s, 6000, PRESENT | VBUS, DIR(CHARGE) | 2, 50).wake);
}

static void test_log_on_change_and_every_minute(void)
{
    batt_state_t s;
    batt_init(&s);
    assert(read_at(&s, 0, PRESENT, DIR(DISCHARGE), 60).log);
    assert(!read_at(&s, 2000, PRESENT, DIR(DISCHARGE), 60).log);
    assert(read_at(&s, 4000, PRESENT, DIR(DISCHARGE), 80).log);
    assert(!read_at(&s, 4000 + BATT_LOG_EVERY_MS - 1, PRESENT, DIR(DISCHARGE), 80).log);
    assert(read_at(&s, 4000 + BATT_LOG_EVERY_MS, PRESENT, DIR(DISCHARGE), 80).log);
}

static void test_failure_logs_once_per_run(void)
{
    batt_state_t s;
    batt_init(&s);
    assert(fail_at(&s, 0).log_fail);
    assert(!fail_at(&s, 2000).log_fail);
    read_at(&s, 4000, PRESENT, DIR(DISCHARGE), 60);
    assert(fail_at(&s, 6000).log_fail);
}

static void test_layout_fits_its_box(void)
{
    for (int look = BATT_NORMAL; look < BATT_LOOK_N; look++) {
        batt_rect_t r[BATT_RECTS_MAX];
        int w, h, n = batt_layout((batt_look_t)look, BATT_BARS_MAX, r, &w, &h);
        assert(n > 0 && n <= BATT_RECTS_MAX);
        for (int i = 0; i < n; i++) {
            assert(r[i].x >= 0 && r[i].y >= 0 && r[i].x + r[i].w <= w && r[i].y + r[i].h <= h);
        }
    }
}

int main(void)
{
    test_absent_hides();
    test_vbus_without_battery_hides();
    test_unknown_percentage_has_no_bars();
    test_bar_steps();
    test_low_enters_at_20_not_21();
    test_low_leaves_at_25_not_24();
    test_charging_overrides_low();
    test_done_shows_four_bars();
    test_vbus_not_charging_is_normal();
    test_stale_after_ten_seconds_of_failures();
    test_never_read_is_hidden();
    test_first_read_never_wakes_and_plug_in_does();
    test_log_on_change_and_every_minute();
    test_failure_logs_once_per_run();
    test_layout_fits_its_box();
    puts("battstate_test ok");
    return 0;
}
