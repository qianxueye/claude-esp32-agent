#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "apack.h"

/* Card source of the animation pack: resident clip heads plus one whole-clip slot, loaded through injected IO; pure, no ESP-IDF includes. */

/* Card status codes; 0, 3, 4, 5, 7, 8 and 9 are terminal for the boot, 6 is recovered by the retry ladder until it is spent. */
enum {
    APSRC_ABSENT = 0,
    APSRC_OK = 1,
    APSRC_LOADING = 2,
    APSRC_NOFILE = 3,
    APSRC_BADHDR = 4,
    APSRC_BADCRC = 5,
    APSRC_READERR = 6,
    APSRC_NOFAT = 7,
    APSRC_NOMEM = 8,
    APSRC_CHANGED = 9,
};

/* open() results. */
enum { APSRC_IO_OK = 0, APSRC_IO_ABSENT, APSRC_IO_NOFILE, APSRC_IO_NOFAT, APSRC_IO_ERR };
/* alloc() kinds: the piece buffer is DMA-capable, the rest is bulk memory. */
enum { APSRC_MEM_DMA = 0, APSRC_MEM_BULK };

#define APSRC_MAX_RUNGS 8

typedef struct {
    void *ctx;
    int (*open)(void *ctx, uint32_t *size);                                /* APSRC_IO_*; size of the file when OK */
    int (*read)(void *ctx, uint32_t off, uint8_t *dst, uint32_t len);      /* bytes read, or negative on error */
    void (*close)(void *ctx);
    uint32_t (*now_ms)(void *ctx);
    void *(*alloc)(void *ctx, size_t n, int kind);                         /* NULL gives status 8 */
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    uint32_t head_ms, piece, read_retries, read_retry_ms, deadline_ms;
    uint32_t retry_s[APSRC_MAX_RUNGS];
    uint32_t rungs;
} apsrc_cfg_t;

typedef struct {
    apsrc_cfg_t cfg;
    _Atomic int status, want;
    _Atomic uint32_t src_gen;
    _Atomic uint32_t ready[AP_MAX_CLIPS];
    _Atomic bool have_pack;
    ap_pack_t pack;
    uint8_t *index, *piece, *head_buf, *slot_buf;
    uint32_t size, payload_off, index_cap, index_crc, slot_cap, cache_off, cache_len;
    uint32_t head_n[AP_MAX_CLIPS], head_off[AP_MAX_CLIPS], head_done[AP_MAX_CLIPS], order[AP_MAX_CLIPS];
    int slot_clip, phase, tries, retry_n;
    uint32_t slot_ready, claim_gen, fail_ms, gate_at, t0, card_ms, rerd;
    uint64_t rd_bytes, rd_ms;
    bool cache_ok, gate, planned, ladder, opened, settled, deadline;
} apsrc_t;

/* Defaults from app_config.h; callers set the IO, clock, allocator and lock fields. */
apsrc_cfg_t apsrc_cfg_default(void);
/* Allocates the piece buffer (status 8 when the allocator refuses) and starts loading; want starts at -1. */
void apsrc_init(apsrc_t *s, const apsrc_cfg_t *cfg);
/* One reader step: at most one frame or one index pass of IO. False when idle, retry-gated or terminal. */
bool apsrc_step(apsrc_t *s);
/* Milliseconds until a gated step may run again; 0 when work is ready, UINT32_MAX when only a want change can wake it. */
uint32_t apsrc_wait_ms(apsrc_t *s);

/* Only the player calls this: the clip it plays, or -1 for none; -1 keeps the slot. */
void apsrc_want(apsrc_t *s, int clip);
int apsrc_status(const apsrc_t *s);
/* Counts only transitions from OK to a failure code; the player rebinds when it changes. */
uint32_t apsrc_src_gen(const apsrc_t *s);
/* Counts slot claims; changes only under the lock. */
uint32_t apsrc_claim_gen(const apsrc_t *s);
/* The pack view once its index parsed, else NULL; frame data is read through apsrc_frame. */
const ap_pack_t *apsrc_pack(const apsrc_t *s);
/* Frames of the clip that are verified in memory; matches aplay's ready callback when wrapped. */
int apsrc_ready(const apsrc_t *s, int clip);
/* A verified frame with its data pointer; false at or past ready. The player holds the lock from its ready check through ag_bring for slot frames (frame >= head frames), head frames need none. */
bool apsrc_frame(const apsrc_t *s, int clip, int frame, ap_frame_t *out);
int apsrc_head_frames(const apsrc_t *s, int clip);
void apsrc_lock(apsrc_t *s);
void apsrc_unlock(apsrc_t *s);
/* Latches once ANIM_CARD_DEADLINE_MS has passed since init, wrap-safe; never cleared. Call it every tick. */
bool apsrc_deadline_passed(apsrc_t *s);

typedef enum { APSRC_SRC_NONE = 0, APSRC_SRC_FLASH = 1, APSRC_SRC_CARD = 2, APSRC_SRC_VERIFYING = 3 } apsrc_src_t;
typedef enum { APSRC_FLASH_VERIFYING, APSRC_FLASH_OK, APSRC_FLASH_FAILED } apsrc_flash_t;

/* Pure source pick, no latch. Evaluate it every tick while the result is VERIFYING or NONE; otherwise at each cut, each loop wrap
   of the current clip and whenever src_gen changes. The deadline bounds VERIFYING only; a card ready later is adopted at the next evaluation. */
apsrc_src_t apsrc_pick(int card_status, apsrc_flash_t flash, bool deadline_passed);

#define APSRC_ANIMC_FMT "animc: src=%d card=%d card_ms=%u rd_kbps=%u rerd=%u starve=%u gen=%u try=%u"
/* Writes the animc log line; returns snprintf's result. */
int apsrc_animc_line(char *buf, size_t n, int src, uint32_t starve, const apsrc_t *s);
/* Read rate of the card in bytes per millisecond (kB/s) over all reads so far. */
uint32_t apsrc_rd_kbps(const apsrc_t *s);
