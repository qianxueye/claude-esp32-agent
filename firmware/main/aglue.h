#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "aplay.h"
#include "dpower.h"

/* Pure glue between the clip player and the UI task: pickup bit, stage map, frame buffer and placement in the scene. */

#define AG_PICKUP_BIT (1u << 31) /* beside the dp_cause_t bits in the UI's wake word */
_Static_assert(DP_CAUSE_N < 31, "the pickup bit sits above every cause bit");

/* The 2 s log lines; two lines so the worst case fits LOG_LINE_MAX_BYTES with its prefix, checked by aglue_test. */
#define AG_LINE_FMT "anim: pack=%d skin=%d clip=%d frames=%u late=%u cuts=%u seek=%u verify_ms=%u iheap=%u/%u ilarge=%u"
#define AG_TIME_LINE_FMT "animt: dec_us=%u/%u work_us=%u/%u cut_ms=%u/%u"

/* Wake bits for one IMU sample: the lifted level wakes, only the pickup edge also carries AG_PICKUP_BIT. */
uint32_t ag_imu_bits(int motion_ev, bool lifted);
/* The player's stage: off only while the panel is off, so the fade before display-off holds the frame. */
aplay_stage_t ag_stage(bool panel_on, dp_stage_t stage, bool dim_plays);

typedef struct {
    int clip, frame;         /* what the buffer holds; clip -1 when nothing */
    uint32_t frames, seeks;  /* frames brought in, and how many of them needed a decode from frame 0 */
} ag_held_t;

/* Brings buf to (clip, frame): nothing when held, one delta for the next frame, else forward from key frame 0.
   False on a malformed frame; the buffer then holds nothing. */
bool ag_bring(const ap_pack_t *p, int clip, int frame, uint8_t *buf, ag_held_t *held);

typedef struct {
    int ox, oy;         /* scene position of the crop's top-left pixel before clipping */
    int x0, y0, x1, y1; /* destination clipped to the scene, x1 and y1 exclusive */
} ag_place_t;

/* Puts the canvas centre on the mascot centre, the scene centre moved by orbit and bubble shift; the mascot's own motion is not applied. */
ag_place_t ag_place(const ap_pack_t *p, const ap_clip_t *c, int gx, int gy, int shift, int scene);
/* Copies the decoded crop into the clipped rows of dst, each index widened to scale x scale pixels. */
void ag_copy(const uint8_t *buf, const ap_clip_t *c, uint8_t scale, const ag_place_t *pl, uint8_t *dst, int dst_stride);

/* Scene position of the sticker crop's top-left pixel: canvas centre on the scene centre plus the recentred orbit, no bubble shift, not clipped. */
ag_place_t ag_sticker_place(const ap_pack_t *p, const ap_clip_t *c, int gx, int gy, int scene);

#define AG_BUBBLE_H_UNITS 18 /* plain configuration: bubble height in units, as ui.c draws it */
#define AG_BUBBLE_Y_MAX_PX 96 /* plain configuration: largest bubble offset from the art centre */
#define AG_BUBBLE_FIT_MARGIN_PX 2 /* plain configuration: corner margin of the fit circle: one pixel of border and one of rounding */

typedef struct {
    int x, y, w, h; /* scene pixels */
} ag_rect_t;

typedef struct {
    ag_rect_t r;
    bool ink; /* ink colour, else the bubble fill */
} ag_cell_t;

typedef struct {
    ag_rect_t body;      /* the bubble at this pop step; empty at pop 0 */
    int text_x, text_y;  /* scene pixels of the content row's top-left corner, at pop 3 */
    int n_cells;         /* tail cells (4) or thought dots (2), drawn in order at pop 3, else 0 */
    ag_cell_t cells[4];
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
/* The sticker skin's bubble beside the art; gx, gy is the raw orbit point. */
ag_bubble_t ag_side_bubble(int w_units, int content_units, bool thought, bool left, int bub_y, int gx, int gy, int pop, int scene);
/* Clawd's bubble above the mascot, as ui.c draws it. */
ag_top_bubble_t ag_top_bubble(int w_units, bool thought, int pop, int gx, int gy, int scene);
