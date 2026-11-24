#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Battery icon state from the PMU status registers; rules in docs/mascot.md "Battery icon". */

typedef enum { BATT_HIDDEN, BATT_NORMAL, BATT_LOW, BATT_CHARGING, BATT_DONE, BATT_LOOK_N } batt_look_t;

#define BATT_UNKNOWN_PCT 101
#define BATT_BARS_MAX 4
#define BATT_PACK(look, bars) ((int)(look) | ((int)(bars) << 4))
#define BATT_LOOK(packed) ((packed) & 0xF)
#define BATT_BARS(packed) (((packed) >> 4) & 0xF)

/* Log line; fields are the raw register decodes, mv is the battery voltage or -1. */
#define BATT_LINE_FMT "batt pct=%d present=%d vbus=%d dir=%d chg=%d mv=%d bars=%d look=%s"

typedef struct {
    bool ok, reg00_ok;
    uint8_t reg00, reg01, pct;
    uint32_t now_ms;
} batt_in_t;

typedef struct {
    batt_look_t look;
    int bars;
    bool wake, log, log_fail, log_stale;
} batt_out_t;

typedef struct {
    bool seen, vbus_seen, vbus, low, failing, logged, stale_logged;
    uint32_t last_ok_ms, last_log_ms;
    batt_look_t look, logged_look;
    int bars, logged_key;
} batt_state_t;

void batt_init(batt_state_t *s);
/* One poll: ok means all registers read; reg00_ok alone still tracks VBUS for the plug-in edge. */
batt_out_t batt_step(batt_state_t *s, const batt_in_t *in);
const char *batt_look_name(batt_look_t look);
/* Decoded fields for the log line. */
bool batt_present(uint8_t reg00);
bool batt_vbus(uint8_t reg00);
int batt_dir(uint8_t reg01);
int batt_chg(uint8_t reg01);

/* One filled rectangle of the icon, relative to its bounding box. */
typedef struct {
    int x, y, w, h;
    uint8_t color;
} batt_rect_t;
enum { BATT_C_GREY, BATT_C_CREAM, BATT_C_RED };
#define BATT_RECTS_MAX 16
#define BATT_PLAIN_W 36
#define BATT_PLAIN_H 24
#define BATT_BOLT_W 64
#define BATT_BOLT_H 28
/* Fills the rectangles for a look and returns their count; w and h receive the bounding box. */
int batt_layout(batt_look_t look, int bars, batt_rect_t out[BATT_RECTS_MAX], int *w, int *h);
