#pragma once
#include "apack.h"

/* Scene palette indices: 0 is black and clears, pack colours take 1..AP_MAX_COLORS, the scene's own colours the top five. */
enum {
    PAL_BG = 0,
    PAL_PACK_FIRST = 1,
    PAL_MASCOT = AP_MAX_COLORS + 1,
    PAL_FILL,
    PAL_INK,
    PAL_ERROR,
    PAL_OFFLINE_FILL,
    PAL_N,
};

_Static_assert(PAL_BG == 0, "rotblit clears with palette entry 0");
_Static_assert(PAL_N == 256, "the five scene colours fill 251-255, so a one-byte index reaches every entry");
