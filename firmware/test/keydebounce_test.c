#include <assert.h>
#include <stdio.h>
#include "keydebounce.h"

#define N 3

static int feed(keydeb_t *k, bool pressed, int count)
{
    int sends = 0;
    for (int i = 0; i < count; i++) {
        sends += keydeb_poll(k, pressed, N);
    }
    return sends;
}

static void test_one_press_sends_once(void)
{
    keydeb_t k;
    keydeb_init(&k);
    assert(feed(&k, false, 10) == 0);
    assert(feed(&k, true, N - 1) == 0);
    assert(feed(&k, true, 1) == 1);
    assert(feed(&k, false, N) == 0);
    assert(feed(&k, true, N) == 1);
}

static void test_short_bounce_sends_nothing(void)
{
    keydeb_t k;
    keydeb_init(&k);
    int sends = 0;
    for (int i = 0; i < 20; i++) {
        sends += feed(&k, true, N - 1);
        sends += feed(&k, false, 1);
    }
    assert(sends == 0);
}

static void test_release_bounce_sends_no_second_press(void)
{
    keydeb_t k;
    keydeb_init(&k);
    int sends = feed(&k, true, 10);
    sends += feed(&k, false, N - 1);
    sends += feed(&k, true, N + 2);
    sends += feed(&k, false, N);
    assert(sends == 1);
    assert(feed(&k, true, N) == 1);
}

static void test_held_key_sends_once(void)
{
    keydeb_t k;
    keydeb_init(&k);
    assert(feed(&k, true, 100000) == 1);
}

static void test_failed_reads_do_not_rearm_a_held_key(void)
{
    keydeb_t k;
    keydeb_init(&k);
    int sends = 0;
    for (int i = 0; i < N + 2; i++) {
        sends += keydeb_poll_read(&k, true, true, N);
    }
    for (int i = 0; i < 10 * N; i++) {
        sends += keydeb_poll_read(&k, false, false, N);
    }
    for (int i = 0; i < N + 2; i++) {
        sends += keydeb_poll_read(&k, true, true, N);
    }
    assert(sends == 1);
    for (int i = 0; i < N; i++) {
        sends += keydeb_poll_read(&k, true, false, N);
    }
    for (int i = 0; i < N; i++) {
        sends += keydeb_poll_read(&k, true, true, N);
    }
    assert(sends == 2);
}

int main(void)
{
    test_one_press_sends_once();
    test_short_bounce_sends_nothing();
    test_release_bounce_sends_no_second_press();
    test_held_key_sends_once();
    test_failed_reads_do_not_rearm_a_held_key();
    puts("keydebounce_test ok");
    return 0;
}
