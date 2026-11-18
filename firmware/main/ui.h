#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    ST_IDLE = 0,
    ST_LISTENING = 1,
    ST_THINKING = 2,
    ST_SPEAKING = 3,
    ST_ERROR = 4,
    ST_OFFLINE = 5,
    ST_COUNT
} dev_state_t;

void ui_start(void);
void ui_set_state(dev_state_t s);
void ui_set_angle_override(bool on, float deg);
/* Test aid for frame-rate measurement; 0 off, 1 spin, 2 redraw, 3 invalidate canvas, 4 invalidate screen, 5 spin with the flushed area deliberately shrunk. */
enum { UI_BENCH_OFF, UI_BENCH_SPIN, UI_BENCH_REDRAW, UI_BENCH_CANVAS, UI_BENCH_SCREEN, UI_BENCH_SHRINK };
void ui_set_bench(int mode);
/* Renders the active screen into an RGB565 buffer; release it with ui_snapshot_free. */
lv_draw_buf_t *ui_snapshot_take(void);
void ui_snapshot_free(lv_draw_buf_t *buf);
