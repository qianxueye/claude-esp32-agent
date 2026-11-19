#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "dpower.h"
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
enum { UI_BENCH_OFF, UI_BENCH_SPIN, UI_BENCH_REDRAW, UI_BENCH_CANVAS, UI_BENCH_SCREEN, UI_BENCH_SHRINK, UI_BENCH_SERIAL };
void ui_set_bench(int mode);
/* Posts an activity cause from any task; the UI timer applies it. */
void ui_wake(dp_cause_t cause);
/* Posts several cause bits and AG_PICKUP_BIT in one atomic step, so a pickup and its wake land in the same UI tick. */
void ui_wake_bits(uint32_t bits);
/* Test aids: pin the power stage (0 active, 1 dim, 2 off; negative releases), run its timeline N times faster, pin the orbit index (negative releases), fill the screen with a grey 0-255 (negative restores). */
void ui_set_disp_force(int stage);
/* Cumulative count of brightening stage changes by cause, indexed by dp_cause_t. */
void ui_wake_counts(uint32_t out[DP_CAUSE_N]);
void ui_set_disp_scale(int n);
void ui_set_orbit(int index);
void ui_set_fill(int grey);
/* Test aid: shows the volume arc at a level 0-100 without touching the volume; negative releases. */
void ui_set_arc(int level);
/* Test aid: shows the brightness arc at ladder index 0-32 without touching the brightness; negative releases. One forced arc at a time. */
void ui_set_brt_arc(int index);
/* Test aid: stretches the forced arc by -3..3 cells past its end (negative at the low end); the level must rest there. 0 releases. */
void ui_set_arc_over(int cells);
/* Test aids: pin the ACTIVE brightness without storing it (negative releases), or set and store it as a swipe would; values are snapped to the ladder. */
void ui_set_brightness_pin(int pct);
void ui_set_brightness(int pct);
/* The percent last written to the panel. */
int ui_panel_level(void);
/* Renders the active screen into an RGB565 buffer; release it with ui_snapshot_free. */
lv_draw_buf_t *ui_snapshot_take(void);
void ui_snapshot_free(lv_draw_buf_t *buf);
/* Test aids: turn the animation pack off or on until reboot; play one pack clip whatever the state (-1 releases). */
void ui_set_skin(bool on);
void ui_set_anim_pin(int clip);

typedef struct {
    int pack, skin, clip;
    uint32_t frames, dec_sum, dec_max, work_sum, work_max, late, cuts, cut_last, cut_max, seeks, verify_ms;
    uint32_t boot_ms, patch_n, patch_sum, patch_max, ring_defer;
} ui_anim_stats_t;
/* Cumulative animation counters; times in microseconds except cut, verify and boot, in milliseconds; work excludes the boot frame. */
void ui_anim_stats(ui_anim_stats_t *out);
