#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "brtmap.h"

/* The ladder formula for any minimum and step count, unrounded. */
static double ladder_exact(int min, int n, int i)
{
    return min * pow(100.0 / min, (double)i / n);
}

static bool strictly_increasing(int min, int n)
{
    for (int i = 1; i <= n; i++) {
        if (lround(ladder_exact(min, n, i)) <= lround(ladder_exact(min, n, i - 1))) {
            return false;
        }
    }
    return true;
}

static void test_m1_ladder(void)
{
#if BRT_MIN_PERCENT == 20 && BRT_STEPS == 32
    static const int WANT[33] = {20, 21, 22, 23, 24, 26, 27, 28, 30, 31, 33, 35, 37, 38, 40, 43, 45,
                                 47, 49, 52, 55, 58, 60, 64, 67, 70, 74, 78, 82, 86, 90, 95, 100};
    for (int i = 0; i <= BRT_STEPS; i++) {
        assert(brt_level(i) == WANT[i]);
    }
#endif
    assert(brt_level(0) == BRT_MIN_PERCENT && brt_level(BRT_STEPS) == 100);
    assert(brt_level(-1) == BRT_MIN_PERCENT && brt_level(BRT_STEPS + 1) == 100);
    for (int i = 1; i <= BRT_STEPS; i++) {
        assert(brt_level(i) > brt_level(i - 1));
        double x = ladder_exact(BRT_MIN_PERCENT, BRT_STEPS, i), frac = x - floor(x);
        assert(fabs(frac - 0.5) >= 0.001);
    }
    assert(strictly_increasing(BRT_MIN_PERCENT, BRT_STEPS));
    /* A minimum of 10 with 32 steps repeats a level: the red proof for the strictness check. */
    assert(!strictly_increasing(10, 32));
}

static void test_m2_register(void)
{
    for (int i = 1; i <= BRT_STEPS; i++) {
        assert(brt_level(i) * 255 / 100 > brt_level(i - 1) * 255 / 100);
    }
}

static void test_m3_index_and_snap(void)
{
    for (int i = 0; i <= BRT_STEPS; i++) {
        assert(brt_index(brt_level(i)) == i);
    }
    for (int p = BRT_MIN_PERCENT; p <= 100; p++) {
        int s = brt_snap(p), best = 1000;
        for (int i = 0; i <= BRT_STEPS; i++) {
            best = abs(brt_level(i) - p) < best ? abs(brt_level(i) - p) : best;
        }
        assert(abs(s - p) == best);
        int i = brt_index(p);
        assert(i == 0 || p - brt_level(i - 1) > abs(s - p));
    }
    for (int p = -5; p < BRT_MIN_PERCENT; p++) {
        assert(brt_snap(p) == BRT_MIN_PERCENT);
    }
    assert(brt_snap(101) == 100 && brt_snap(255) == 100);
}

int main(void)
{
    test_m1_ladder();
    test_m2_register();
    test_m3_index_and_snap();
    puts("ok");
    return 0;
}
