#include "brtmap.h"
#include <math.h>

/* Computed on each call: no shared table, so any task may call it at any time. */
int brt_level(int i)
{
    i = i < 0 ? 0 : i > BRT_STEPS ? BRT_STEPS : i;
    return (int)lround(BRT_MIN_PERCENT * pow(100.0 / BRT_MIN_PERCENT, (double)i / BRT_STEPS));
}

int brt_index(int pct)
{
    int lo = 0, hi = BRT_STEPS;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (brt_level(mid) < pct) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo > 0 && pct - brt_level(lo - 1) <= brt_level(lo) - pct) {
        return lo - 1;
    }
    return lo;
}

int brt_snap(int pct)
{
    pct = pct < BRT_MIN_PERCENT ? BRT_MIN_PERCENT : pct > 100 ? 100 : pct;
    return brt_level(brt_index(pct));
}
