#pragma once
#include <stdbool.h>

/* Press/release debounce for one polled key; pure, no ESP-IDF includes. */

typedef struct {
    int down, up;
    bool sent;
} keydeb_t;

void keydeb_init(keydeb_t *k);
/* Feeds one poll; returns true exactly once per debounced press. */
bool keydeb_poll(keydeb_t *k, bool pressed, int polls);
