#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "aplay.h"
#include "dpower.h"

/* Pure glue between the clip player and the UI task: pickup bit, stage map, frame buffer and placement in the scene. */

#define AG_PICKUP_BIT (1u << 31) /* beside the dp_cause_t bits in the UI's wake word */
_Static_assert(DP_CAUSE_N < 31, "the pickup bit sits above every cause bit");

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
