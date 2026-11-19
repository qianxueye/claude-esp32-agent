#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Read-only view and frame decoder of an animation pack held in memory; pure, no ESP-IDF includes.
   All offsets in the pack are little-endian and counted from the pack start; the converter is firmware/tools/animpack.py. */

#define AP_MAGIC "CLWDANIM"
#define AP_VERSION 1
#define AP_HEADER_BYTES 48
#define AP_CLIP_ENTRY_BYTES 24
#define AP_FRAME_ENTRY_BYTES 8
#define AP_CODEC_DELTA 1
#define AP_FRAME_KEY 0x8000u
#define AP_FRAME_MS_MASK 0x7FFFu
#define AP_NO_CLIP 0xFF
#define AP_STATE_SLOTS 8
#define AP_EVENT_SLOTS 4
#define AP_MAX_CLIPS 16    /* plain configuration: size of the static pack view */
#define AP_MAX_COLORS 250  /* format: 256 minus black minus the five scene colours */
#define AP_CANVAS_MAX_PX 240 /* mathematical model: widest canvas whose rotated corners clear the ring, test L1 */
#define AP_SAFE_UP_PX 92     /* mathematical model: crop reach above the canvas centre, below the bubble, test L1 */
#define AP_SAFE_DOWN_PX 96   /* mathematical model: crop reach below the canvas centre, inside the scene, test L1 */

typedef enum { AP_EMPTY, AP_OK, AP_NO_PARTITION, AP_BAD_HEADER, AP_BAD_PAYLOAD, AP_BAD_LAYOUT, AP_DECODE_ERROR, AP_VERIFYING } ap_status_t;

typedef struct {
    uint16_t x, y, w, h, frames, colors;
    const uint8_t *palette, *frame_tbl; /* colors x u16 RGB565 for indices 1..colors; frames x 8-byte entries */
    bool loop;
} ap_clip_t;

typedef struct {
    const uint8_t *base;
    uint32_t total_len, payload_off, payload_crc, max_area;
    uint16_t canvas_w, canvas_h;
    uint8_t scale, clip_count, state_clip[AP_STATE_SLOTS], pickup_clip;
    ap_clip_t clip[AP_MAX_CLIPS];
} ap_pack_t;

typedef struct {
    const uint8_t *data;
    uint16_t len, ms;
    bool key;
} ap_frame_t;

/* Validates header, index and every frame entry of the avail bytes at base; the payload CRC is left to ap_payload_ok. */
ap_status_t ap_parse(const uint8_t *base, uint32_t avail, ap_pack_t *out);
/* CRC-32/IEEE, chained like zlib.crc32: pass 0 first, then the previous result. */
uint32_t ap_crc32(uint32_t crc, const uint8_t *p, size_t n);
bool ap_payload_ok(const ap_pack_t *p);
/* Looks up one frame of a parsed pack; false when clip or frame is out of range. */
bool ap_frame(const ap_pack_t *p, int clip, int frame, ap_frame_t *out);
/* Applies one frame's tokens to a w x h index buffer; false on a malformed frame, never touching bytes past area. */
bool ap_decode(const ap_frame_t *f, uint8_t *buf, uint32_t area);
/* True when the crop lies on the canvas and inside the safe box around the canvas centre, in screen pixels. */
bool ap_layout_ok(uint16_t canvas_w, uint16_t canvas_h, uint8_t scale, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
