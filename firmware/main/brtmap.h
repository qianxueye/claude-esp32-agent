#pragma once
#include "app_config.h"

/* Brightness ladder: BRT_STEPS + 1 equal-ratio levels from BRT_MIN_PERCENT to 100. Model in docs/display-power.md "User brightness". */

/* Level in percent of ladder index i, clamped to 0..BRT_STEPS. */
int brt_level(int i);
/* Index of the level nearest to pct; a tie goes to the lower index. */
int brt_index(int pct);
/* The ladder level nearest to pct after clamping it to BRT_MIN_PERCENT..100. */
int brt_snap(int pct);
