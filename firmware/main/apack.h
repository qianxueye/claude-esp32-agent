#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Read-only view and frame decoder of an animation pack held in memory; pure, no ESP-IDF includes.
   All offsets in the pack are little-endian and counted from the pack start; the converter is firmware/tools/animpack.py. */

#define AP_MAGIC "CLWDANIM"
#define AP_VERSION 2
#define AP_HEADER_BYTES 48
#define AP_CLIP_ENTRY_BYTES 28
#define AP_FRAME_ENTRY_BYTES 16
#define AP_CODEC_DELTA 1
#define AP_FRAME_KEY 0x8000u
#define AP_FRAME_MS_MASK 0x7FFFu
#define AP_NO_CLIP 0xFF
#define AP_STATE_SLOTS 8
#define AP_EVENT_SLOTS 4
#define AP_MAX_CLIPS 16    /* plain configuration: size of the static pack view */
#define AP_MAX_COLORS 250  /* format: 256 minus black minus the five scene colours */
#define AP_CLIP_LOOP 1u        /* clip flags bit 0 */
#define AP_CLIP_BUBBLE_LEFT 2u /* clip flags bit 1 */
#define AP_CANVAS_MAX_PX 512     /* plain configuration: format cap on the canvas side in screen px */
#define AP_ART_REACH_MAX_PX 207  /* mathematical model: art reach x scale plus the 17.21 px recentred orbit reach stays within SCENE_MAX_RADIUS_PX, test L1 */

typedef enum { AP_EMPTY, AP_OK, AP_NO_PARTITION, AP_BAD_HEADER, AP_BAD_PAYLOAD, AP_BAD_LAYOUT, AP_DECODE_ERROR, AP_VERIFYING } ap_status_t;

typedef struct {
    uint16_t x, y, w, h, frames, colors, reach; /* reach: canvas px from the canvas centre to the farthest lit pixel corner, rounded up */
    int16_t bubble_y;                           /* screen px from the canvas centre, y down */
    const uint8_t *palette, *frame_tbl;         /* colors x u16 RGB565 for indices 1..colors; frames x 16-byte entries */
    bool loop, bubble_left;
} ap_clip_t;

typedef struct {
    const uint8_t *base;
    uint32_t total_len, payload_off, avail, max_area;
    uint16_t canvas_w, canvas_h;
    uint8_t scale, clip_count, state_clip[AP_STATE_SLOTS], pickup_clip;
    ap_clip_t clip[AP_MAX_CLIPS];
} ap_pack_t;

typedef struct {
    const uint8_t *data; /* NULL unless the frame bytes are mapped */
    uint32_t off, len, crc;
    uint16_t ms, colors; /* colors: the clip's palette size; valid indices are 0..colors */
    bool key;
} ap_frame_t;

/* Validates header, index and every frame entry of the avail bytes at base; total_len may reach limit and exceed avail.
   On any status but AP_OK the view is zeroed, so a player handed it shows Clawd. */
ap_status_t ap_parse(const uint8_t *base, uint32_t avail, uint32_t limit, ap_pack_t *out);
/* CRC-32/IEEE, chained like zlib.crc32: pass 0 first, then the previous result. */
uint32_t ap_crc32(uint32_t crc, const uint8_t *p, size_t n);
/* True when the frame's bytes are mapped and match its CRC. */
bool ap_frame_crc_ok(const ap_frame_t *f);
/* Looks up one frame of a parsed pack; false when clip or frame is out of range. */
bool ap_frame(const ap_pack_t *p, int clip, int frame, ap_frame_t *out);
/* Applies one frame's tokens to a w x h index buffer; false on a malformed frame, an index above colors or unmapped data,
   never touching bytes past area. */
bool ap_decode(const ap_frame_t *f, uint8_t *buf, uint32_t area);
/* True when the crop lies on the canvas, the canvas and the reach fit the caps, and reach agrees with the crop. */
bool ap_layout_ok(uint16_t canvas_w, uint16_t canvas_h, uint8_t scale, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t reach);
