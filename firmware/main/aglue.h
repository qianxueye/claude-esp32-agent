#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"
#include "aplay.h"
#include "dpower.h"
#include "rotblit.h"

/* Pure glue between the clip player and the UI task: pickup bit, stage map, frame buffer and placement in the scene. */

#define AG_PICKUP_BIT (1u << 31) /* beside the dp_cause_t bits in the UI's wake word */
_Static_assert(DP_CAUSE_N < 31, "the pickup bit sits above every cause bit");

/* The 2 s log lines; two lines so the worst case fits LOG_LINE_MAX_BYTES with its prefix, checked by aglue_test. */
#define AG_LINE_FMT "anim: pack=%d skin=%d clip=%d frames=%u late=%u cuts=%u seek=%u verify_ms=%u iheap=%u/%u ilarge=%u"
#define AG_TIME_LINE_FMT "animt: dec_us=%u/%u work_us=%u/%u cut_ms=%u/%u boot_ms=%u patch_us=%u/%u ring_defer=%u"

/* Wake bits for one IMU sample: the lifted level wakes, only the pickup edge also carries AG_PICKUP_BIT. */
uint32_t ag_imu_bits(int motion_ev, bool lifted);
/* The player's stage: off only while the panel is off, so the fade before display-off holds the frame. */
aplay_stage_t ag_stage(bool panel_on, dp_stage_t stage, bool dim_plays);

typedef struct {
    int clip, frame;         /* what the buffer holds; clip -1 when nothing */
    uint32_t frames, seeks;  /* frames brought in, and how many of them needed a decode from frame 0 */
} ag_held_t;

typedef enum { AG_OK, AG_NOT_READY, AG_FAIL } ag_bring_t;

/* Brings buf to (clip, frame): nothing when held, one delta for the next frame, else forward from key frame 0.
   AG_NOT_READY (frame >= ready) leaves buf and held untouched; AG_FAIL is a malformed frame and the buffer then holds nothing. */
ag_bring_t ag_bring(const ap_pack_t *p, int clip, int frame, int ready, uint8_t *buf, ag_held_t *held);

typedef struct {
    int ox, oy;         /* scene position of the crop's top-left pixel before clipping */
    int x0, y0, x1, y1; /* destination clipped to the scene, x1 and y1 exclusive */
} ag_place_t;

/* Scene position of the sticker crop's top-left pixel: canvas centre on the scene centre plus the recentred orbit, no bubble shift, not clipped. */
ag_place_t ag_sticker_place(const ap_pack_t *p, const ap_clip_t *c, int gx, int gy, int scene);

#define AG_BUBBLE_H_UNITS BUBBLE_HEIGHT_UNITS /* bubble height in units, as ui.c draws it */
#define AG_BUBBLE_Y_MAX_PX 96 /* plain configuration: largest bubble offset from the art centre */
#define AG_BUBBLE_FIT_MARGIN_PX 2 /* mathematical model: slack between a content corner and the ring clear radius, as volarc_test A2 and A2s keep it */

typedef struct {
    int x, y, w, h; /* scene pixels */
} ag_rect_t;

typedef struct {
    ag_rect_t r;
    bool ink; /* ink colour, else the bubble fill */
} ag_cell_t;

#define AG_SPINNER_UNITS 7 /* plain configuration: the spinner sprite's side in units, as ui.c's SPINNER rows */
#define AG_GLYPH_UNITS 12  /* plain configuration: one text glyph's width in units (48 px at the 4 px unit) */
#define AG_TEXT_ROWS_PX 48 /* plain configuration: height of the text mask in px */

typedef struct {
    int glyphs;
    bool spinner, dots; /* a spinner before the text, three pulsing dots after it */
} ag_content_t;

/* Content width in units: spinner and its gap, the glyphs, the dots. */
int ag_content_units(ag_content_t c);

/* Every cell the side bubble draws, in scene pixels and in drawing order: frame, tail or thought, spinner, dots, then the text mask. */
typedef struct {
    ag_rect_t body;      /* the bubble at this pop step */
    int n_frame;         /* 4 frame rectangles (two ink, two fill) from pop 1, else 0 */
    ag_cell_t frame[4];
    int n_cells;         /* tail cells (4) or thought dots (2) at pop 3, else 0 */
    ag_cell_t cells[4];
    bool spinner;        /* the spinner sprite's square at pop 3, drawn cell by cell at the unit */
    ag_rect_t spin;
    int n_dots;          /* 3 pulsing dots at pop 3 when the content has them, else 0 */
    ag_rect_t dots[3];
    ag_rect_t mask;      /* the text mask's rectangle at pop 3; w 0 otherwise */
} ag_bubble_t;

typedef struct {
    ag_rect_t body;
    int cx_u, bottom_u; /* tail column centre and the first row below the full bubble, in units */
} ag_top_bubble_t;

/* Bubble width in units for content_units of content, always even. */
int ag_bubble_w_units(int content_units);
/* Rounds toward zero to a multiple of the unit, then clamps to +-AG_BUBBLE_Y_MAX_PX. */
int ag_bub_y_snap(int y);
/* Outer edge in px from the art centre on the bubble's side: the largest multiple of the unit whose far corners, with the orbit's half ranges, stay inside the ring's clear radius. */
int ag_bubble_edge(int bub_y, int half_h_px, int orbit_hx_px, int orbit_hy_px);
/* The sticker skin's bubble beside the art; gx, gy is the raw orbit point, dot_phase the dot drawn larger. */
ag_bubble_t ag_side_bubble(ag_content_t content, bool thought, bool left, int bub_y, int gx, int gy, int pop, int dot_phase, int scene);
/* The body and the tail or thought cells, which reach past the body; empty (x1 <= x0) at pop 0. */
irect_t ag_bubble_bbox(const ag_bubble_t *b);
/* Clawd's bubble above the mascot, as ui.c draws it. */
ag_top_bubble_t ag_top_bubble(int w_units, bool thought, int pop, int gx, int gy, int scene);

/* The w x h battery icon below the legs on the raw orbit, the spot it takes with no bubble (docs/mascot.md "Battery icon"). */
ag_rect_t ag_batt_below(int w, int h, int gx, int gy, int scene);
/* The icon right of Clawd's full top bubble, centred on its body; its right edge at most BATT_BESIDE_MAX_DX_PX from the scene centre. */
ag_rect_t ag_batt_beside_top(const ag_top_bubble_t *t, int w, int h, int scene);
/* The icon on the inner side of a full side bubble, BATT_BESIDE_GAP_UNITS from its box with the tail, centred on its body. */
ag_rect_t ag_batt_beside_side(const ag_bubble_t *g, bool left, int w, int h);
/* The ink plate the sticker skin draws behind the icon, so the art does not show through it. */
ag_rect_t ag_batt_plate(ag_rect_t icon);

/* A running count, sum and maximum of per-frame times in microseconds. */
typedef struct {
    uint32_t n, sum, max;
} ag_acc_t;
void ag_acc_add(ag_acc_t *a, uint32_t us);
/* Mean over the window between two reads of the cumulative counters, safe across one wrap of the sum or the count; 0 for an empty window. */
uint32_t ag_window_mean(uint32_t prev_sum, uint32_t prev_n, uint32_t sum, uint32_t n);

/* The work statistics of decoded-art frames; the first refresh after start is the boot frame and is reported apart. */
typedef struct {
    bool armed;
    uint32_t boot_us;
    ag_acc_t work;
} ag_work_t;
/* Folds one finished refresh and returns the frame's next stage (0 none, 1 decoded, 2 presented). stage 2 carries tick_us, the time from decode to the end of its tick; refr_us is the refresh itself.
   The first call only records the boot frame and clears the stage, so a decode made before it never reaches a sample; later calls add a sample for stage 2. */
int ag_work_refresh(ag_work_t *w, int stage, uint32_t tick_us, uint32_t refr_us);
/* The boot frame's time in milliseconds, rounded to nearest; 0 before it. */
uint32_t ag_work_boot_ms(const ag_work_t *w);
