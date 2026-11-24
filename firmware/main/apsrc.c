#include "apsrc.h"
#include <stdio.h>
#include <string.h>
#include "aplay.h"
#include "app_config.h"

enum { PH_OPEN, PH_INDEX, PH_VERIFY, PH_HEADS, PH_SLOT, PH_WAIT, PH_DEAD };

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t now(const apsrc_t *s)
{
    return s->cfg.now_ms(s->cfg.ctx);
}

/* Milliseconds from since to t, wrap-safe by unsigned subtraction. */
static uint32_t elapsed(uint32_t t, uint32_t since)
{
    return (uint32_t)(t - since);
}

apsrc_cfg_t apsrc_cfg_default(void)
{
    static const uint32_t RETRY_S[] = ANIM_CARD_RETRY_S;
    apsrc_cfg_t c;
    memset(&c, 0, sizeof c);
    c.head_ms = ANIM_HEAD_MS;
    c.piece = ANIM_CARD_PIECE;
    c.read_retries = ANIM_CARD_READ_RETRIES;
    c.read_retry_ms = ANIM_CARD_READ_RETRY_MS;
    c.deadline_ms = ANIM_CARD_DEADLINE_MS;
    c.rungs = sizeof RETRY_S / sizeof RETRY_S[0];
    memcpy(c.retry_s, RETRY_S, sizeof RETRY_S);
    return c;
}

static void set_status(apsrc_t *s, int st)
{
    const int old = atomic_load(&s->status);
    if (old == st) {
        return;
    }
    atomic_store(&s->status, st);
    if (old == APSRC_OK && st >= APSRC_NOFILE) {
        atomic_fetch_add(&s->src_gen, 1);
    }
    if (!s->settled && st != APSRC_LOADING) {
        s->settled = true;
        s->card_ms = elapsed(now(s), s->t0);
    }
}

static void close_io(apsrc_t *s)
{
    if (s->opened) {
        s->cfg.close(s->cfg.ctx);
        s->opened = false;
    }
    s->cache_ok = false;
}

static void die(apsrc_t *s, int st)
{
    close_io(s);
    set_status(s, st);
    s->phase = PH_DEAD;
}

/* The next failure of a recoverable kind: wait for the next rung, or end when the ladder is spent. */
static void wait_next_rung(apsrc_t *s)
{
    close_io(s);
    set_status(s, APSRC_READERR);
    s->fail_ms = now(s);
    s->phase = s->tries >= (int)s->cfg.rungs ? PH_DEAD : PH_WAIT;
}

static void read_error(apsrc_t *s)
{
    s->cache_ok = false;
    if ((uint32_t)s->retry_n < s->cfg.read_retries) {
        s->retry_n++;
        s->gate = true;
        s->gate_at = now(s) + s->cfg.read_retry_ms;
        return;
    }
    s->retry_n = 0;
    wait_next_rung(s);
}

/* Ensures the piece at p is in the buffer, cut at limit so a pass over the index never touches payload bytes. */
static int read_piece(apsrc_t *s, uint32_t p, uint32_t limit)
{
    const uint32_t need = limit - p < s->cfg.piece ? limit - p : s->cfg.piece;
    if (s->cache_ok && s->cache_off == p && s->cache_len >= need) {
        return 0;
    }
    const uint32_t t0 = now(s);
    const int got = s->cfg.read(s->cfg.ctx, p, s->piece, need);
    s->rd_ms += elapsed(now(s), t0);
    if (got < 0 || (uint32_t)got != need) {
        s->cache_ok = false;
        return -1;
    }
    s->rd_bytes += need;
    s->cache_ok = true;
    s->cache_off = p;
    s->cache_len = need;
    return 0;
}

/* Copies file bytes [off, off + len) into dst through piece-aligned reads; only bytes inside the range are copied. */
static int read_span(apsrc_t *s, uint32_t off, uint32_t len, uint8_t *dst)
{
    for (uint32_t p = off / s->cfg.piece * s->cfg.piece; p < off + len; p += s->cfg.piece) {
        if (read_piece(s, p, s->size) < 0) {
            return -1;
        }
        const uint32_t lo = p > off ? p : off, end = p + s->cache_len, want_end = off + len, hi = end < want_end ? end : want_end;
        if (hi > lo) {
            memcpy(dst + (lo - off), s->piece + (lo - p), hi - lo);
        }
    }
    return 0;
}

enum { LF_OK = 0, LF_READ = -1, LF_CRC = -2 };

/* Reads one frame and checks its CRC; one re-read from the card when it does not match. */
static int load_frame(apsrc_t *s, const ap_frame_t *f, uint8_t *dst)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt) {
            s->rerd++;
            s->cache_ok = false;
        }
        if (read_span(s, f->off, f->len, dst) < 0) {
            return LF_READ;
        }
        if (ap_crc32(0, dst, f->len) == f->crc) {
            return LF_OK;
        }
    }
    return LF_CRC;
}

static int idle_clip(const apsrc_t *s)
{
    const uint8_t v = s->pack.state_clip[APLAY_STATE_IDLE];
    return v < s->pack.clip_count ? v : -1;
}

static int plan(apsrc_t *s)
{
    const ap_pack_t *p = &s->pack;
    uint32_t head_total = 0;
    s->slot_cap = 0;
    for (int c = 0; c < p->clip_count; c++) {
        const int n = p->clip[c].frames;
        uint32_t ms = 0, hb = 0, tb = 0;
        int h = 0;
        do {
            ap_frame_t f;
            ap_frame(p, c, h, &f);
            ms += f.ms;
            hb += f.len;
            h++;
        } while (h < n && ms < s->cfg.head_ms);
        for (int k = h; k < n; k++) {
            ap_frame_t f;
            ap_frame(p, c, k, &f);
            tb += f.len;
        }
        s->head_n[c] = (uint32_t)h;
        s->head_off[c] = head_total;
        head_total += hb;
        s->slot_cap = tb > s->slot_cap ? tb : s->slot_cap;
    }
    s->head_buf = s->cfg.alloc(s->cfg.ctx, head_total ? head_total : 1, APSRC_MEM_BULK);
    s->slot_buf = s->slot_cap ? s->cfg.alloc(s->cfg.ctx, s->slot_cap, APSRC_MEM_BULK) : NULL;
    if (!s->head_buf || (s->slot_cap && !s->slot_buf)) {
        return -1;
    }
    int k = 0;
    const int idle = idle_clip(s);
    if (idle >= 0) {
        s->order[k++] = (uint32_t)idle;
    }
    for (int c = 0; c < p->clip_count; c++) {
        if (c != idle) {
            s->order[k++] = (uint32_t)c;
        }
    }
    s->planned = true;
    return 0;
}

static uint8_t *head_dst(const apsrc_t *s, int clip, const ap_frame_t *f)
{
    ap_frame_t f0;
    ap_frame(&s->pack, clip, 0, &f0);
    return s->head_buf + s->head_off[clip] + (f->off - f0.off);
}

static uint8_t *slot_dst(const apsrc_t *s, int clip, const ap_frame_t *f)
{
    ap_frame_t fh;
    ap_frame(&s->pack, clip, (int)s->head_n[clip], &fh);
    return s->slot_buf + (f->off - fh.off);
}

static bool heads_done(const apsrc_t *s)
{
    for (int c = 0; c < s->pack.clip_count; c++) {
        if (s->head_done[c] < s->head_n[c]) {
            return false;
        }
    }
    return true;
}

static void step_open(apsrc_t *s)
{
    uint32_t size = 0;
    const int r = s->cfg.open(s->cfg.ctx, &size);
    if (r == APSRC_IO_OK) {
        s->opened = true;
        s->cache_ok = false;
        if (!atomic_load(&s->have_pack)) {
            s->size = size;
        }
        s->phase = atomic_load(&s->have_pack) ? PH_VERIFY : PH_INDEX;
    } else if (s->ladder && (r == APSRC_IO_ABSENT || r == APSRC_IO_ERR)) {
        wait_next_rung(s);
    } else if (r == APSRC_IO_ABSENT) {
        die(s, APSRC_ABSENT);
    } else if (r == APSRC_IO_NOFILE) {
        die(s, s->ladder ? APSRC_CHANGED : APSRC_NOFILE);
    } else if (r == APSRC_IO_NOFAT) {
        die(s, s->ladder ? APSRC_CHANGED : APSRC_NOFAT);
    } else {
        read_error(s);
    }
}

static void step_index(apsrc_t *s)
{
    const int bad = s->ladder ? APSRC_CHANGED : APSRC_BADHDR;
    if (s->size < AP_HEADER_BYTES) {
        die(s, bad);
        return;
    }
    if (read_piece(s, 0, s->size) < 0) {
        read_error(s);
        return;
    }
    const uint32_t index_len = rd32(s->piece + 20);
    if (index_len > s->size - AP_HEADER_BYTES) {
        die(s, bad);
        return;
    }
    const uint32_t payload_off = AP_HEADER_BYTES + index_len;
    if (!s->index || payload_off > s->index_cap) {
        s->index = s->cfg.alloc(s->cfg.ctx, payload_off, APSRC_MEM_BULK);
        s->index_cap = payload_off;
        if (!s->index) {
            die(s, APSRC_NOMEM);
            return;
        }
    }
    if (read_span(s, 0, payload_off, s->index) < 0) {
        read_error(s);
        return;
    }
    if (ap_parse(s->index, payload_off, s->size, &s->pack) != AP_OK) {
        die(s, bad);
        return;
    }
    s->payload_off = payload_off;
    s->index_crc = ap_crc32(0, s->index + AP_HEADER_BYTES, payload_off - AP_HEADER_BYTES);
    if (plan(s) < 0) {
        die(s, APSRC_NOMEM);
        return;
    }
    atomic_store(&s->have_pack, true);
    set_status(s, APSRC_LOADING);
    s->phase = PH_HEADS;
    s->retry_n = 0;
}

/* Compares the file's header and index with the loaded ones, reading through the piece buffer only. */
static void step_verify(apsrc_t *s)
{
    uint32_t crc = 0;
    bool same = true;
    for (uint32_t p = 0; p < s->payload_off; p += s->cfg.piece) {
        if (read_piece(s, p, s->payload_off) < 0) {
            read_error(s);
            return;
        }
        uint32_t lo = 0, hi = s->cache_len;
        if (p == 0) {
            same = memcmp(s->piece, s->index, AP_HEADER_BYTES) == 0;
            lo = AP_HEADER_BYTES;
        }
        hi = p + hi > s->payload_off ? s->payload_off - p : hi;
        if (hi > lo) {
            crc = ap_crc32(crc, s->piece + lo, hi - lo);
        }
    }
    s->retry_n = 0;
    if (!same || crc != s->index_crc) {
        die(s, APSRC_CHANGED);
        return;
    }
    if (heads_done(s)) {
        set_status(s, APSRC_OK);
        s->phase = PH_SLOT;
    } else {
        set_status(s, APSRC_LOADING);
        s->phase = PH_HEADS;
    }
}

static void step_heads(apsrc_t *s)
{
    for (int i = 0; i < s->pack.clip_count; i++) {
        const int c = (int)s->order[i];
        if (s->head_done[c] >= s->head_n[c]) {
            continue;
        }
        ap_frame_t f;
        ap_frame(&s->pack, c, (int)s->head_done[c], &f);
        const int r = load_frame(s, &f, head_dst(s, c, &f));
        if (r == LF_READ) {
            read_error(s);
        } else if (r == LF_CRC) {
            die(s, APSRC_BADCRC);
        } else {
            s->retry_n = 0;
            atomic_store_explicit(&s->ready[c], ++s->head_done[c], memory_order_release);
            if (heads_done(s)) {
                set_status(s, APSRC_OK);
                s->phase = PH_SLOT;
            }
        }
        return;
    }
    set_status(s, APSRC_OK);
    s->phase = PH_SLOT;
}

/* The clip the slot should hold: the want, else the slot's own clip, else the idle clip; -1 for none. */
static int claim_target(const apsrc_t *s)
{
    int w = atomic_load_explicit(&s->want, memory_order_acquire);
    if (w < 0) {
        w = s->slot_clip >= 0 ? s->slot_clip : idle_clip(s);
    }
    return w < s->pack.clip_count ? w : -1;
}

static void slot_claim(apsrc_t *s)
{
    const int w0 = claim_target(s);
    if (w0 < 0 || w0 == s->slot_clip) {
        return;
    }
    s->cfg.lock(s->cfg.ctx);
    const int w = claim_target(s);
    if (w >= 0 && w != s->slot_clip) {
        if (s->slot_clip >= 0) {
            atomic_store_explicit(&s->ready[s->slot_clip], s->head_n[s->slot_clip], memory_order_release);
        }
        s->slot_clip = w;
        s->slot_ready = 0;
        s->claim_gen++;
        atomic_store_explicit(&s->ready[w], s->head_n[w], memory_order_release);
    }
    s->cfg.unlock(s->cfg.ctx);
}

static bool slot_has_work(const apsrc_t *s)
{
    if (s->slot_clip >= 0 && s->slot_ready < (uint32_t)s->pack.clip[s->slot_clip].frames - s->head_n[s->slot_clip]) {
        return true;
    }
    const int w = claim_target(s);
    return w >= 0 && w != s->slot_clip;
}

static bool step_slot(apsrc_t *s)
{
    slot_claim(s);
    const int c = s->slot_clip;
    if (c < 0 || s->slot_ready >= (uint32_t)s->pack.clip[c].frames - s->head_n[c]) {
        return false;
    }
    ap_frame_t f;
    ap_frame(&s->pack, c, (int)(s->head_n[c] + s->slot_ready), &f);
    const int r = load_frame(s, &f, slot_dst(s, c, &f));
    if (r == LF_READ) {
        read_error(s);
    } else if (r == LF_CRC) {
        die(s, APSRC_BADCRC);
    } else {
        s->retry_n = 0;
        s->slot_ready++;
        atomic_store_explicit(&s->ready[c], s->head_n[c] + s->slot_ready, memory_order_release);
    }
    return true;
}

void apsrc_init(apsrc_t *s, const apsrc_cfg_t *cfg)
{
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    atomic_init(&s->status, APSRC_LOADING);
    atomic_init(&s->want, -1);
    s->slot_clip = -1;
    s->t0 = now(s);
    s->phase = PH_OPEN;
    s->piece = s->cfg.piece ? s->cfg.alloc(s->cfg.ctx, s->cfg.piece, APSRC_MEM_DMA) : NULL;
    if (!s->piece) {
        die(s, APSRC_NOMEM);
    }
}

bool apsrc_step(apsrc_t *s)
{
    const uint32_t t = now(s);
    if (s->gate) {
        if ((int32_t)(t - s->gate_at) < 0) {
            return false;
        }
        s->gate = false;
    }
    switch (s->phase) {
    case PH_OPEN:
        step_open(s);
        return true;
    case PH_INDEX:
        step_index(s);
        return true;
    case PH_VERIFY:
        step_verify(s);
        return true;
    case PH_HEADS:
        step_heads(s);
        return true;
    case PH_SLOT:
        return step_slot(s);
    case PH_WAIT:
        if (elapsed(t, s->fail_ms) < s->cfg.retry_s[s->tries] * 1000u) {
            return false;
        }
        s->tries++;
        s->ladder = true;
        step_open(s);
        return true;
    default:
        return false;
    }
}

uint32_t apsrc_wait_ms(apsrc_t *s)
{
    const uint32_t t = now(s);
    if (s->gate) {
        return (int32_t)(s->gate_at - t) > 0 ? s->gate_at - t : 0;
    }
    if (s->phase == PH_DEAD) {
        return UINT32_MAX;
    }
    if (s->phase == PH_WAIT) {
        const uint32_t need = s->cfg.retry_s[s->tries] * 1000u, el = elapsed(t, s->fail_ms);
        return el >= need ? 0 : need - el;
    }
    if (s->phase == PH_SLOT) {
        return slot_has_work(s) ? 0 : UINT32_MAX;
    }
    return 0;
}

void apsrc_want(apsrc_t *s, int clip)
{
    atomic_store_explicit(&s->want, clip, memory_order_release);
}

int apsrc_status(const apsrc_t *s)
{
    return atomic_load(&s->status);
}

uint32_t apsrc_src_gen(const apsrc_t *s)
{
    return atomic_load(&s->src_gen);
}

uint32_t apsrc_claim_gen(const apsrc_t *s)
{
    return s->claim_gen;
}

const ap_pack_t *apsrc_pack(const apsrc_t *s)
{
    return atomic_load(&s->have_pack) ? &s->pack : NULL;
}

int apsrc_ready(const apsrc_t *s, int clip)
{
    if (clip < 0 || clip >= AP_MAX_CLIPS) {
        return 0;
    }
    return (int)atomic_load_explicit(&s->ready[clip], memory_order_acquire);
}

int apsrc_head_frames(const apsrc_t *s, int clip)
{
    return clip >= 0 && clip < AP_MAX_CLIPS ? (int)s->head_n[clip] : 0;
}

bool apsrc_frame(const apsrc_t *s, int clip, int frame, ap_frame_t *out)
{
    if (frame < 0 || frame >= apsrc_ready(s, clip) || !ap_frame(&s->pack, clip, frame, out)) {
        return false;
    }
    out->data = frame < (int)s->head_n[clip] ? head_dst(s, clip, out) : slot_dst(s, clip, out);
    return true;
}

void apsrc_lock(apsrc_t *s)
{
    s->cfg.lock(s->cfg.ctx);
}

void apsrc_unlock(apsrc_t *s)
{
    s->cfg.unlock(s->cfg.ctx);
}

bool apsrc_deadline_passed(apsrc_t *s)
{
    if (!s->deadline && elapsed(now(s), s->t0) >= s->cfg.deadline_ms) {
        s->deadline = true;
    }
    return s->deadline;
}

apsrc_src_t apsrc_pick(int card_status, apsrc_flash_t flash, bool deadline_passed)
{
    if (card_status == APSRC_OK) {
        return APSRC_SRC_CARD;
    }
    if (flash == APSRC_FLASH_OK) {
        return !deadline_passed && card_status == APSRC_LOADING ? APSRC_SRC_VERIFYING : APSRC_SRC_FLASH;
    }
    if (!deadline_passed && (card_status == APSRC_LOADING || flash == APSRC_FLASH_VERIFYING)) {
        return APSRC_SRC_VERIFYING;
    }
    return APSRC_SRC_NONE;
}

uint32_t apsrc_rd_kbps(const apsrc_t *s)
{
    return s->rd_ms ? (uint32_t)(s->rd_bytes / s->rd_ms) : 0;
}

int apsrc_animc_line(char *buf, size_t n, int src, uint32_t starve, const apsrc_t *s)
{
    return snprintf(buf, n, APSRC_ANIMC_FMT, src, atomic_load(&s->status), (unsigned)s->card_ms, (unsigned)apsrc_rd_kbps(s),
                    (unsigned)s->rerd, (unsigned)starve, (unsigned)atomic_load(&s->src_gen), (unsigned)s->tries);
}
