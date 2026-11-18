#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Device-owned user brightness: storage, the swipe and the test hooks. Rules in docs/display-power.md "User brightness". */

typedef struct {
    uint32_t arms, cancels, slides, stops, saves;
    int user, effective;
    bool set;
} bright_stats_t;

/* Loads the stored level; call after nvstore_init and before ui_start. */
void bright_init(void);
/* The level ACTIVE shows: the test pin when one is set, else the user level. */
int bright_effective(void);
/* Arms a swipe and ends a test pin; returns the user level it armed with, read under the same lock. */
int bright_swipe_arm(void);
void bright_swipe_set(int pct);
/* Ends a swipe; a commit that left the level unchanged stores nothing. */
void bright_swipe_end(bool cancel);
/* Pins the level without storing it until released, rebooted or a swipe arms; a negative value releases it. */
void bright_test_pin(int pct);
/* Sets and stores the user level, as a swipe would. */
void bright_set(int pct);
void bright_count_slide(void);
void bright_count_stop(void);
/* Writes a due change; call from the life task after volume_poll. Returns true when a write happened. */
bool bright_poll(uint32_t now_ms, bool quiet);
void bright_stats(bright_stats_t *out);
