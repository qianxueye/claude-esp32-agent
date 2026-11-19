#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apack.h"
#include "aplay.h"
#include "apsrc.h"
#include "app_config.h"

#define MAXC 8
#define LOG_N 60000
#define PIECE ANIM_CARD_PIECE

typedef struct {
    int frames;
    uint32_t key, delta;
    uint16_t ms_a, ms_b;
} spec_t;

/* Fake card on a virtual clock in microseconds. */
static struct {
    uint8_t *file;
    uint32_t size;
    int open_res;
    uint32_t opens;
    uint64_t open_us[32];
    int fail_n;
    bool fail_all, fail_range;
    uint32_t fail_lo, fail_hi, flip_pos;
    int flip_mode; /* 0 none, 1 first read only, 2 every read */
    bool flipped;
    uint32_t reads, nlog;
    struct {
        uint32_t off, len;
    } log[LOG_N];
    uint64_t rate_bps, lat_us, stall_at_us, stall_us;
    bool in_lock;
    uint32_t locks, claims_seen;
} fc;

static uint64_t g_us;
static uint32_t g_base_ms;
static void (*g_on_time)(uint64_t until_us);
static void (*g_lock_hook)(void);
static int g_alloc_n, g_alloc_fail_at;
static uint8_t *g_alloc_ptr[64];
static size_t g_alloc_len[64];
static int g_alloc_kind[64];

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint8_t pay(uint32_t off)
{
    return (uint8_t)(((off + 1u) * 2654435761u) >> 24);
}

/* Builds a valid pack file with the given clips, the idle clip bound to state 0. */
static void build(const spec_t *sp, int n, int idle)
{
    uint32_t idx = AP_HEADER_BYTES + (uint32_t)n * AP_CLIP_ENTRY_BYTES, total;
    for (int i = 0; i < n; i++) {
        idx += 2 + (uint32_t)sp[i].frames * AP_FRAME_ENTRY_BYTES;
    }
    total = idx;
    for (int i = 0; i < n; i++) {
        total += sp[i].key + (uint32_t)(sp[i].frames - 1) * sp[i].delta;
    }
    free(fc.file);
    fc.file = calloc(total, 1);
    fc.size = total;
    uint8_t *b = fc.file;
    memcpy(b, AP_MAGIC, 8);
    put16(b + 8, AP_VERSION);
    b[10] = 1;
    b[11] = (uint8_t)n;
    put16(b + 12, 16);
    put16(b + 14, 16);
    memset(b + 28, AP_NO_CLIP, 12);
    b[28] = (uint8_t)idle;
    for (int i = 1; i < n && i < AP_STATE_SLOTS; i++) {
        b[28 + i] = (uint8_t)i;
    }
    b[36] = 0;
    uint32_t off = AP_HEADER_BYTES + (uint32_t)n * AP_CLIP_ENTRY_BYTES, pal[MAXC], tbl[MAXC], pay_off = idx;
    for (int i = 0; i < n; i++) {
        pal[i] = off;
        put16(b + off, 0x0821);
        off += 2;
        tbl[i] = off;
        off += (uint32_t)sp[i].frames * AP_FRAME_ENTRY_BYTES;
    }
    for (int i = 0; i < n; i++) {
        uint8_t *e = b + AP_HEADER_BYTES + i * AP_CLIP_ENTRY_BYTES;
        put16(e + 4, 16);
        put16(e + 6, 16);
        put16(e + 8, (uint32_t)sp[i].frames);
        put16(e + 10, 1);
        put32(e + 12, pal[i]);
        put32(e + 16, tbl[i]);
        e[20] = AP_CLIP_LOOP;
        e[21] = AP_CODEC_DELTA;
        put16(e + 22, 12);
        for (int k = 0; k < sp[i].frames; k++) {
            uint8_t *fe = b + tbl[i] + k * AP_FRAME_ENTRY_BYTES;
            const uint32_t len = k == 0 ? sp[i].key : sp[i].delta;
            for (uint32_t j = 0; j < len; j++) {
                b[pay_off + j] = pay(pay_off + j);
            }
            put32(fe, pay_off);
            put32(fe + 4, len);
            put32(fe + 8, ap_crc32(0, b + pay_off, len));
            put16(fe + 12, (uint32_t)((k % 2 ? sp[i].ms_b : sp[i].ms_a) | (k == 0 ? AP_FRAME_KEY : 0)));
            pay_off += len;
        }
    }
    put32(b + 16, total);
    put32(b + 20, idx - AP_HEADER_BYTES);
    put32(b + 44, ap_crc32(ap_crc32(0, b, 44), b + AP_HEADER_BYTES, idx - AP_HEADER_BYTES));
}

static uint32_t first_frame_off(int clip)
{
    ap_pack_t p;
    ap_frame_t f;
    assert(ap_parse(fc.file, fc.size, fc.size, &p) == AP_OK && ap_frame(&p, clip, 0, &f));
    return f.off;
}

static uint32_t frame_off(int clip, int frame, uint32_t *len)
{
    ap_pack_t p;
    ap_frame_t f;
    assert(ap_parse(fc.file, fc.size, fc.size, &p) == AP_OK && ap_frame(&p, clip, frame, &f));
    if (len) {
        *len = f.len;
    }
    return f.off;
}

static uint32_t payload_off(void)
{
    return AP_HEADER_BYTES + (uint32_t)fc.file[20] + ((uint32_t)fc.file[21] << 8) + ((uint32_t)fc.file[22] << 16);
}

static int io_open(void *ctx, uint32_t *size)
{
    (void)ctx;
    assert(fc.opens < 32);
    fc.open_us[fc.opens++] = g_us;
    *size = fc.size;
    return fc.open_res;
}

static void io_close(void *ctx)
{
    (void)ctx;
}

static int io_read(void *ctx, uint32_t off, uint8_t *dst, uint32_t len)
{
    (void)ctx;
    assert(!fc.in_lock);
    if (fc.nlog < LOG_N) {
        fc.log[fc.nlog].off = off;
        fc.log[fc.nlog].len = len;
    }
    fc.nlog++;
    fc.reads++;
    uint64_t cost = fc.lat_us + (fc.rate_bps ? (uint64_t)len * 1000000u / fc.rate_bps : 0);
    if (fc.stall_us && g_us >= fc.stall_at_us) {
        cost += fc.stall_us;
        fc.stall_us = 0;
    }
    const uint64_t end = g_us + cost;
    if (g_on_time) {
        g_on_time(end);
    }
    g_us = end;
    if (fc.fail_all || fc.fail_n > 0 || (fc.fail_range && off < fc.fail_hi && off + len > fc.fail_lo)) {
        if (fc.fail_n > 0) {
            fc.fail_n--;
        }
        return -1;
    }
    memcpy(dst, fc.file + off, len);
    if (fc.flip_mode && fc.flip_pos >= off && fc.flip_pos < off + len && (fc.flip_mode == 2 || !fc.flipped)) {
        dst[fc.flip_pos - off] ^= 0x5A;
        fc.flipped = true;
    }
    return (int)len;
}

static uint32_t io_now(void *ctx)
{
    (void)ctx;
    return g_base_ms + (uint32_t)(g_us / 1000);
}

static void *io_alloc(void *ctx, size_t n, int kind)
{
    (void)ctx;
    const int i = g_alloc_n++;
    if (i == g_alloc_fail_at) {
        return NULL;
    }
    uint8_t *p = malloc(n);
    assert(p && i < 64);
    memset(p, 0xEE, n);
    g_alloc_ptr[i] = p;
    g_alloc_len[i] = n;
    g_alloc_kind[i] = kind;
    return p;
}

static void io_lock(void *ctx)
{
    (void)ctx;
    fc.in_lock = true;
    fc.locks++;
    if (g_lock_hook) {
        g_lock_hook();
    }
}

static void io_unlock(void *ctx)
{
    (void)ctx;
    fc.in_lock = false;
}

static void reset(void)
{
    for (int i = 0; i < 64; i++) {
        free(g_alloc_ptr[i]);
        g_alloc_ptr[i] = NULL;
    }
    uint8_t *f = fc.file;
    uint32_t sz = fc.size;
    memset(&fc, 0, sizeof fc);
    fc.file = f;
    fc.size = sz;
    fc.open_res = APSRC_IO_OK;
    fc.lat_us = 600;
    fc.rate_bps = 1300000;
    g_us = 0;
    g_base_ms = 0;
    g_on_time = NULL;
    g_lock_hook = NULL;
    g_alloc_n = 0;
    g_alloc_fail_at = -1;
}

static apsrc_cfg_t cfg(void)
{
    apsrc_cfg_t c = apsrc_cfg_default();
    c.open = io_open;
    c.read = io_read;
    c.close = io_close;
    c.now_ms = io_now;
    c.alloc = io_alloc;
    c.lock = io_lock;
    c.unlock = io_unlock;
    return c;
}

static apsrc_t g_s;

static void start(const apsrc_cfg_t *c)
{
    apsrc_init(&g_s, c);
}

/* Steps the reader, letting virtual time pass while it waits, until done() or the time limit. */
static bool drive(bool (*done)(void), uint64_t limit_us)
{
    while (g_us < limit_us) {
        if (done && done()) {
            return true;
        }
        if (!apsrc_step(&g_s)) {
            const uint32_t w = apsrc_wait_ms(&g_s);
            if (w == UINT32_MAX) {
                return done && done();
            }
            g_us += (w ? (uint64_t)w : 1u) * 1000u;
        }
    }
    return done && done();
}

static bool is_ok(void)
{
    return apsrc_status(&g_s) == APSRC_OK;
}

static bool is_6(void)
{
    return apsrc_status(&g_s) == APSRC_READERR;
}

static bool is_5(void)
{
    return apsrc_status(&g_s) == APSRC_BADCRC;
}

static void load_ok(const apsrc_cfg_t *c)
{
    start(c);
    assert(drive(is_ok, 600000000ull));
}

static bool idle_full(void)
{
    return apsrc_ready(&g_s, 1) == 60;
}

/* Loads the card and lets the reader prefill the idle clip (clip 1 in the SR1 pack). */
static void load_idle(const apsrc_cfg_t *c)
{
    start(c);
    assert(drive(idle_full, 600000000ull));
}

static const spec_t SR1_CLIPS[3] = {{60, 66000, 48000, 60, 70}, {60, 66000, 57000, 60, 70}, {60, 66000, 48000, 60, 70}};

static void sr1_build(void)
{
    build(SR1_CLIPS, 3, 1);
}

/* SR1: the plan after the index parses. */
static void test_sr1_plan(void)
{
    sr1_build();
    reset();
    apsrc_cfg_t c = cfg();
    start(&c);
    assert(apsrc_status(&g_s) == APSRC_LOADING && apsrc_pack(&g_s) == NULL);
    while (!apsrc_pack(&g_s)) {
        assert(apsrc_step(&g_s));
    }
    assert(g_s.head_n[0] == 4 && g_s.head_n[1] == 4 && g_s.head_n[2] == 4);
    assert(g_s.head_off[1] - g_s.head_off[0] == 210000 && g_s.head_off[2] - g_s.head_off[1] == 237000);
    assert(g_s.slot_cap == 3192000);
    assert(g_alloc_n == 4 && g_alloc_len[2] == 657000 && g_alloc_len[3] == 3192000 && g_alloc_kind[0] == APSRC_MEM_DMA && g_alloc_len[0] == PIECE);
    reset();
    c.head_ms = 0;
    start(&c);
    while (!apsrc_pack(&g_s)) {
        apsrc_step(&g_s);
    }
    assert(g_s.head_n[0] == 1 && g_s.head_n[1] == 1 && g_s.head_n[2] == 1);
    reset();
    c.head_ms = 10000;
    start(&c);
    while (!apsrc_pack(&g_s)) {
        apsrc_step(&g_s);
    }
    assert(g_s.head_n[0] == 60 && g_s.head_n[1] == 60 && g_s.head_n[2] == 60);
}

/* SR2: piece-aligned reads of at most one piece, the idle head first, nothing outside a target frame lands in a buffer. */
static void test_sr2_read_rules(void)
{
    sr1_build();
    reset();
    apsrc_cfg_t c = cfg();
    load_ok(&c);
    drive(NULL, g_us + 20000000ull);
    for (uint32_t i = 0; i < fc.nlog; i++) {
        assert(fc.log[i].off % PIECE == 0 && fc.log[i].len <= PIECE && fc.log[i].len > 0);
    }
    const uint32_t first_after_index = (payload_off() + PIECE - 1) / PIECE * PIECE;
    uint32_t i = 0;
    while (fc.log[i].off < first_after_index) {
        i++;
    }
    assert(fc.log[i].off == first_frame_off(1) / PIECE * PIECE);
    /* The idle clip's slot then filled: bytes past its tail stay untouched. */
    const uint32_t tail1 = 59u * 57000u - 3u * 57000u;
    assert(g_s.slot_clip == 1 && apsrc_ready(&g_s, 1) == 60);
    for (uint32_t k = tail1; k < g_s.slot_cap; k++) {
        assert(g_s.slot_buf[k] == 0xEE);
    }
    uint32_t n0;
    const uint32_t o0 = frame_off(1, 4, &n0);
    assert(!memcmp(g_s.slot_buf, fc.file + o0, n0));
}

/* SR3: ready and data follow verification, frame by frame. */
static void test_sr3_ready_and_data(void)
{
    sr1_build();
    reset();
    apsrc_cfg_t c = cfg();
    start(&c);
    ap_frame_t f;
    for (int k = 0; k < 3; k++) {
        assert(apsrc_ready(&g_s, k) == 0 && !apsrc_frame(&g_s, k, 0, &f));
    }
    int seen = 0;
    while (apsrc_status(&g_s) != APSRC_OK) {
        apsrc_step(&g_s);
        const int r1 = apsrc_ready(&g_s, 1);
        if (r1 > seen) {
            seen = r1;
            assert(apsrc_frame(&g_s, 1, r1 - 1, &f) && !apsrc_frame(&g_s, 1, r1, &f));
        }
    }
    assert(seen == 4);
    for (int k = 0; k < 3; k++) {
        assert(apsrc_ready(&g_s, k) >= 4);
        for (int j = 0; j < 4; j++) {
            uint32_t len;
            const uint32_t off = frame_off(k, j, &len);
            assert(apsrc_frame(&g_s, k, j, &f) && f.data && f.len == len && !memcmp(f.data, fc.file + off, len) && ap_frame_crc_ok(&f));
        }
    }
}

/* SR4: a want gives the clip's first frame at once, with no reader step. */
static void test_sr4_want_has_head(void)
{
    sr1_build();
    reset();
    apsrc_cfg_t c = cfg();
    load_ok(&c);
    const uint32_t reads = fc.reads;
    apsrc_want(&g_s, 2);
    ap_frame_t f;
    assert(apsrc_frame(&g_s, 2, 0, &f) && f.data != NULL && fc.reads == reads);
}

static int g_hook_set;

static void hook_set_want(void)
{
    apsrc_want(&g_s, g_hook_set);
}

/* SR5: the claim re-reads want inside the lock and does no IO under it. */
static void test_sr5_claim_rereads_want(void)
{
    for (int mode = 0; mode < 2; mode++) {
        sr1_build();
        reset();
        apsrc_cfg_t c = cfg();
        load_idle(&c);
        assert(g_s.slot_clip == 1);
        const uint32_t gen = apsrc_claim_gen(&g_s);
        apsrc_want(&g_s, 0);
        g_hook_set = mode == 0 ? 1 : 2;
        g_lock_hook = hook_set_want;
        assert(apsrc_step(&g_s) || mode == 0);
        g_lock_hook = NULL;
        if (mode == 0) {
            assert(g_s.slot_clip == 1 && apsrc_claim_gen(&g_s) == gen);
        } else {
            assert(g_s.slot_clip == 2 && apsrc_claim_gen(&g_s) == gen + 1 && apsrc_ready(&g_s, 1) == 4);
        }
        assert(fc.locks >= 1 && !fc.in_lock);
    }
}

/* SR6: want -1 keeps the slot; an empty slot prefills the idle clip. */
static void test_sr6_want_minus_one(void)
{
    sr1_build();
    reset();
    apsrc_cfg_t c = cfg();
    load_idle(&c);
    assert(g_s.slot_clip == 1);
    apsrc_want(&g_s, 0);
    for (int i = 0; i < 5; i++) {
        apsrc_step(&g_s);
    }
    assert(g_s.slot_clip == 0 && apsrc_ready(&g_s, 0) == 9 && apsrc_ready(&g_s, 1) == 4);
    const uint32_t gen = apsrc_claim_gen(&g_s);
    apsrc_want(&g_s, -1);
    for (int i = 0; i < 5; i++) {
        apsrc_step(&g_s);
    }
    assert(g_s.slot_clip == 0 && apsrc_claim_gen(&g_s) == gen && apsrc_ready(&g_s, 0) == 14);
}

/* Frame index within the slot clip 0 whose middle byte the fake card corrupts. */
static void corrupt_slot_frame(int clip, int frame, int mode)
{
    uint32_t len;
    const uint32_t off = frame_off(clip, frame, &len);
    fc.flip_pos = off + len / 2;
    fc.flip_mode = mode;
    fc.flipped = false;
}

/* SR7: a frame bad after one re-read stops ready and ends the card; a transient flip heals. */
static void test_sr7_crc(void)
{
    sr1_build();
    reset();
    apsrc_cfg_t c = cfg();
    load_ok(&c);
    corrupt_slot_frame(0, 20, 2);
    apsrc_want(&g_s, 0);
    drive(is_5, g_us + 100000000ull);
    assert(is_5() && apsrc_ready(&g_s, 0) == 20 && g_s.rerd == 1 && apsrc_src_gen(&g_s) == 1);
    const uint32_t reads = fc.reads;
    assert(!apsrc_step(&g_s) && fc.reads == reads);
    reset();
    load_ok(&c);
    corrupt_slot_frame(0, 20, 1);
    apsrc_want(&g_s, 0);
    drive(NULL, g_us + 100000000ull);
    assert(is_ok() && apsrc_ready(&g_s, 0) == 60 && g_s.rerd == 1 && apsrc_src_gen(&g_s) == 0);
}

/* SR8: three failed reads end in status 6, one is absorbed; retries keep their spacing. */
static void test_sr8_inline_retries(void)
{
    sr1_build();
    reset();
    apsrc_cfg_t c = cfg();
    load_ok(&c);
    fc.fail_n = 1;
    apsrc_want(&g_s, 0);
    drive(NULL, g_us + 2000000ull);
    assert(is_ok() && apsrc_ready(&g_s, 0) > 4 && apsrc_src_gen(&g_s) == 0);
    reset();
    load_ok(&c);
    fc.fail_n = 3;
    apsrc_want(&g_s, 0);
    const uint32_t r0 = fc.reads;
    assert(apsrc_step(&g_s) && fc.reads > r0);
    const uint64_t t1 = g_us;
    const uint32_t r1 = fc.reads;
    assert(!apsrc_step(&g_s) && fc.reads == r1);
    g_us += (ANIM_CARD_READ_RETRY_MS - 1) * 1000;
    assert(!apsrc_step(&g_s) && fc.reads == r1);
    g_us = t1 + ANIM_CARD_READ_RETRY_MS * 1000;
    assert(apsrc_step(&g_s));
    g_us += ANIM_CARD_READ_RETRY_MS * 1000;
    assert(apsrc_step(&g_s));
    assert(is_6() && apsrc_src_gen(&g_s) == 1 && g_s.tries == 0);
}

/* SR10: the pick table. */
static void test_sr10_pick(void)
{
    const bool D = true, N = false;
    const apsrc_flash_t FV = APSRC_FLASH_VERIFYING, FO = APSRC_FLASH_OK, FF = APSRC_FLASH_FAILED;
    /* Red (a): a latch on FLASH after the deadline must not hold off a card that turns ready. */
    assert(apsrc_pick(2, FO, D) == APSRC_SRC_FLASH);
    assert(apsrc_pick(1, FO, D) == APSRC_SRC_CARD);
    assert(apsrc_pick(1, FV, N) == APSRC_SRC_CARD && apsrc_pick(1, FO, D) == APSRC_SRC_CARD && apsrc_pick(1, FF, N) == APSRC_SRC_CARD);
    assert(apsrc_pick(2, FO, N) == APSRC_SRC_VERIFYING);
    assert(apsrc_pick(2, FO, D) == APSRC_SRC_FLASH);
    for (int cs = 0; cs <= 9; cs++) {
        if (cs != 1 && cs != 2) {
            assert(apsrc_pick(cs, FO, N) == APSRC_SRC_FLASH && apsrc_pick(cs, FO, D) == APSRC_SRC_FLASH);
            assert(apsrc_pick(cs, FF, N) == APSRC_SRC_NONE && apsrc_pick(cs, FF, D) == APSRC_SRC_NONE);
            assert(apsrc_pick(cs, FV, D) == APSRC_SRC_NONE);
        }
    }
    assert(apsrc_pick(2, FV, N) == APSRC_SRC_VERIFYING && apsrc_pick(2, FF, N) == APSRC_SRC_VERIFYING);
    assert(apsrc_pick(2, FV, D) == APSRC_SRC_NONE && apsrc_pick(2, FF, D) == APSRC_SRC_NONE);
    assert(apsrc_pick(0, FV, N) == APSRC_SRC_VERIFYING && apsrc_pick(6, FV, N) == APSRC_SRC_VERIFYING);
    /* Red (b): ignoring deadline_passed in rows 2 and 4 would wait on the card forever. */
    assert(apsrc_pick(2, FO, D) != APSRC_SRC_VERIFYING && apsrc_pick(2, FV, D) != APSRC_SRC_VERIFYING);
    assert(apsrc_pick(2, FO, D) == APSRC_SRC_FLASH);
}

/* SR10: the deadline bit sets after ANIM_CARD_DEADLINE_MS, across a clock wrap, and never clears. */
static void test_sr10_deadline_wrap(void)
{
    sr1_build();
    reset();
    apsrc_cfg_t c = cfg();
    g_base_ms = 0xFFFFFFFFu - 500;
    start(&c);
    assert(!apsrc_deadline_passed(&g_s));
    g_us = (uint64_t)(ANIM_CARD_DEADLINE_MS - 1) * 1000;
    assert(!apsrc_deadline_passed(&g_s));
    g_us = (uint64_t)ANIM_CARD_DEADLINE_MS * 1000;
    assert(apsrc_deadline_passed(&g_s));
    g_us = 100000ull * 1000;
    assert(apsrc_deadline_passed(&g_s));
    g_base_ms = 0;
    g_us = 0;
    assert(apsrc_deadline_passed(&g_s));
}

/* SR11: the animc line fits at worst values. */
static void test_sr11_animc(void)
{
    static apsrc_t s;
    memset(&s, 0, sizeof s);
    const uint32_t W = 4294967295u;
    atomic_init(&s.status, 9);
    atomic_init(&s.src_gen, W);
    s.card_ms = W;
    s.rd_ms = 1;
    s.rd_bytes = W;
    s.rerd = W;
    s.tries = (int)W;
    char line[512];
    const int n = apsrc_animc_line(line, sizeof line, 3, W, &s);
    assert(n > 0 && n + (int)strlen("I (4294967295) app: ") < LOG_LINE_MAX_BYTES);
    assert(strstr(line, "src=3") && strstr(line, "card=9") && strstr(line, "card_ms=4294967295") && strstr(line, "rd_kbps=") && strstr(line, "rerd=") &&
           strstr(line, "starve=") && strstr(line, "gen=") && strstr(line, "try="));
}

/* Sets the card to fail and lets the reader fall to status 6; returns the time of the failure. */
static uint64_t fail_card(void)
{
    fc.fail_all = true;
    fc.open_res = APSRC_IO_ERR;
    const int gen = (int)apsrc_src_gen(&g_s);
    while (apsrc_src_gen(&g_s) == (uint32_t)gen && !is_6()) {
        if (!apsrc_step(&g_s)) {
            g_us += 1000;
        }
    }
    return g_us;
}

/* SR12: after a read error the retry checks the index only and never rewrites a verified head. */
static void test_sr12_heads_write_once(void)
{
    sr1_build();
    reset();
    apsrc_cfg_t c = cfg();
    load_ok(&c);
    ap_frame_t f;
    assert(apsrc_frame(&g_s, 1, 1, &f));
    const uint8_t *held = f.data;
    uint8_t snap[256];
    memcpy(snap, held, sizeof snap);
    apsrc_want(&g_s, 0);
    fail_card();
    assert(is_6());
    /* The card now carries other bytes inside head frames 1 to 3; header and index are unchanged. */
    for (int k = 0; k < 3; k++) {
        for (int j = 1; j < 4; j++) {
            uint32_t len;
            const uint32_t off = frame_off(k, j, &len);
            fc.file[off + 7] ^= 0xFF;
        }
    }
    fc.fail_all = false;
    fc.open_res = APSRC_IO_OK;
    const uint32_t from = fc.nlog;
    g_us += 61000ull * 1000;
    while (!is_ok() && !is_5()) {
        assert(apsrc_step(&g_s));
    }
    assert(fc.nlog > from);
    const uint32_t po = payload_off();
    for (uint32_t i = from; i < fc.nlog; i++) {
        assert(fc.log[i].off < po);
    }
    assert(!memcmp(held, snap, sizeof snap) && held[7] == snap[7]);
    assert(is_ok());
    assert(apsrc_frame(&g_s, 1, 1, &f) && f.data == held);
}

/* SR13: the retry ladder, one terminal 5, a changed card, and the rung count across recoveries. */
static void test_sr13_ladder(void)
{
    sr1_build();
    reset();
    apsrc_cfg_t c = cfg();
    load_ok(&c);
    apsrc_want(&g_s, 0);
    const uint32_t opens0 = fc.opens;
    const uint64_t t_fail = fail_card();
    drive(NULL, t_fail + 100000ull * 1000000ull);
    static const uint32_t R[5] = ANIM_CARD_RETRY_S;
    assert(fc.opens == opens0 + 5 && is_6() && g_s.tries == 5);
    uint64_t prev = t_fail;
    for (int k = 0; k < 5; k++) {
        assert(fc.open_us[opens0 + k] - prev == (uint64_t)R[k] * 1000000u);
        prev = fc.open_us[opens0 + k];
    }
    assert(apsrc_wait_ms(&g_s) == UINT32_MAX && !apsrc_step(&g_s) && fc.opens == opens0 + 5);
    assert(prev - t_fail == 29220ull * 1000000u);

    /* Status 5 is never retried. */
    reset();
    load_ok(&c);
    corrupt_slot_frame(0, 10, 2);
    apsrc_want(&g_s, 0);
    drive(is_5, g_us + 100000000ull);
    assert(is_5());
    const uint32_t o5 = fc.opens;
    g_us += 30ull * 3600 * 1000000;
    assert(!apsrc_step(&g_s) && fc.opens == o5 && is_5());

    /* Changed index bytes end the card with status 9 and no more opens. */
    reset();
    load_ok(&c);
    apsrc_want(&g_s, 0);
    fail_card();
    fc.file[AP_HEADER_BYTES + 40] ^= 0x01;
    fc.fail_all = false;
    fc.open_res = APSRC_IO_OK;
    const uint32_t o9 = fc.opens;
    drive(NULL, g_us + 100000ull * 1000000ull);
    assert(apsrc_status(&g_s) == APSRC_CHANGED && fc.opens == o9 + 1);

    /* A changed header does the same. */
    reset();
    sr1_build();
    load_ok(&c);
    apsrc_want(&g_s, 0);
    fail_card();
    fc.file[28] ^= 0x01;
    fc.fail_all = false;
    fc.open_res = APSRC_IO_OK;
    drive(NULL, g_us + 100000ull * 1000000ull);
    assert(apsrc_status(&g_s) == APSRC_CHANGED);

    /* 6 -> 1 -> 6 continues at the next unused rung: the third wait is the third value. */
    sr1_build();
    reset();
    load_ok(&c);
    uint64_t tf = 0;
    for (int k = 0; k < 3; k++) {
        apsrc_want(&g_s, k % 2);
        tf = fail_card();
        fc.fail_all = false;
        fc.open_res = APSRC_IO_OK;
        const uint32_t o = fc.opens;
        assert(drive(is_ok, g_us + 40000ull * 1000000ull));
        assert(fc.open_us[o] - tf == (uint64_t)R[k] * 1000000u && g_s.tries == k + 1);
    }
}

/* SR14: a card that is ready after the deadline is adopted at the first loop wrap after it. */
static struct {
    uint32_t t_ready;
    apsrc_src_t src;
    uint64_t next_eval_us;
    uint64_t next_wrap_us;
    bool log_ready;
    int at100[100];
    uint32_t card_src_at;
} sr14;

static void sr14_time(uint64_t until_us)
{
    while (sr14.next_eval_us <= until_us) {
        const uint64_t t = sr14.next_eval_us;
        g_us = t;
        const uint32_t ms = (uint32_t)(t / 1000);
        if (apsrc_status(&g_s) == APSRC_OK && !sr14.t_ready) {
            sr14.t_ready = ms;
        }
        const bool dl = apsrc_deadline_passed(&g_s);
        const bool wrap = t >= sr14.next_wrap_us;
        if (wrap) {
            sr14.next_wrap_us += 1000000;
        }
        if (sr14.src == APSRC_SRC_VERIFYING || sr14.src == APSRC_SRC_NONE || wrap) {
            sr14.src = apsrc_pick(apsrc_status(&g_s), APSRC_FLASH_OK, dl);
            if (sr14.src == APSRC_SRC_CARD && !sr14.card_src_at) {
                sr14.card_src_at = ms;
            }
        }
        if (ms / 100 < 100) {
            sr14.at100[ms / 100] = (int)sr14.src;
        }
        sr14.next_eval_us += 10000;
    }
}

static void test_sr14_late_card(void)
{
    sr1_build();
    reset();
    fc.rate_bps = 195000;
    apsrc_cfg_t c = cfg();
    memset(&sr14, 0, sizeof sr14);
    sr14.next_eval_us = 10000;
    sr14.next_wrap_us = 1000000;
    sr14.src = APSRC_SRC_VERIFYING;
    g_on_time = sr14_time;
    start(&c);
    drive(is_ok, 20000000ull);
    g_on_time = NULL;
    sr14_time(g_us + 5000000);
    assert(sr14.t_ready > 3000 && sr14.t_ready < 4000);
    /* FLASH from the deadline, CARD at the first wrap after the card is ready. */
    const uint32_t wrap_after = (sr14.t_ready / 1000 + 1) * 1000;
    assert(sr14.card_src_at == wrap_after);
    for (uint32_t k = 1; k < 100 && k * 100 < wrap_after; k++) {
        assert(sr14.at100[k] == (k * 100 < ANIM_CARD_DEADLINE_MS ? APSRC_SRC_VERIFYING : APSRC_SRC_FLASH));
    }
}

/* SR15: a read error while loading heads, or before the index, resumes without rereading what is verified. */
static bool in_range(uint32_t off, uint32_t len, uint32_t lo, uint32_t hi)
{
    return off < hi && off + len > lo;
}

static void test_sr15_error_while_loading(void)
{
    sr1_build();
    reset();
    apsrc_cfg_t c = cfg();
    start(&c);
    uint32_t len;
    /* Heads load in the order idle (1), 0, 2: the read error hits clip 2's first frame. */
    const uint32_t o2 = frame_off(2, 0, &len);
    fc.fail_range = true;
    fc.fail_lo = o2;
    fc.fail_hi = o2 + len;
    while (!is_6()) {
        assert(apsrc_step(&g_s) || (g_us += 1000, true));
    }
    assert(apsrc_ready(&g_s, 1) == 4 && apsrc_ready(&g_s, 0) == 4 && apsrc_ready(&g_s, 2) == 0 && apsrc_src_gen(&g_s) == 0);
    fc.fail_range = false;
    const uint32_t from = fc.nlog;
    g_us += 61000ull * 1000;
    bool saw_loading = false;
    while (!is_ok()) {
        assert(apsrc_step(&g_s));
        saw_loading = saw_loading || apsrc_status(&g_s) == APSRC_LOADING;
    }
    assert(saw_loading && apsrc_ready(&g_s, 2) >= 4);
    for (uint32_t i = from; i < fc.nlog; i++) {
        const uint32_t h1 = frame_off(1, 0, &len), e1 = frame_off(1, 3, &len) + len, h0 = frame_off(0, 0, &len), e0 = frame_off(0, 3, &len) + len;
        assert(!in_range(fc.log[i].off, fc.log[i].len, h1, e1) && !in_range(fc.log[i].off, fc.log[i].len, h0, e0));
    }

    /* An error before the index loads: the retry loads it fresh. */
    reset();
    start(&c);
    fc.fail_n = 3;
    while (!is_6()) {
        assert(apsrc_step(&g_s) || (g_us += 1000, true));
    }
    assert(apsrc_pack(&g_s) == NULL && apsrc_src_gen(&g_s) == 0);
    g_us += 61000ull * 1000;
    saw_loading = false;
    while (!is_ok()) {
        assert(apsrc_step(&g_s));
        saw_loading = saw_loading || apsrc_status(&g_s) == APSRC_LOADING;
    }
    assert(saw_loading && apsrc_pack(&g_s) != NULL);
}

/* Boot statuses from the open result, and memory refusal. */
static void test_sr16_boot_codes(void)
{
    static const int IO[4] = {APSRC_IO_ABSENT, APSRC_IO_NOFILE, APSRC_IO_NOFAT, APSRC_IO_ERR};
    static const int WANT[3] = {APSRC_ABSENT, APSRC_NOFILE, APSRC_NOFAT};
    sr1_build();
    for (int i = 0; i < 3; i++) {
        reset();
        fc.open_res = IO[i];
        apsrc_cfg_t c = cfg();
        start(&c);
        drive(NULL, 1000000);
        assert(apsrc_status(&g_s) == WANT[i] && !apsrc_step(&g_s) && fc.opens == 1 && apsrc_wait_ms(&g_s) == UINT32_MAX);
    }
    reset();
    fc.open_res = APSRC_IO_ERR;
    apsrc_cfg_t c = cfg();
    start(&c);
    drive(NULL, 100000000ull);
    assert(apsrc_status(&g_s) == APSRC_READERR && g_s.phase == 5);
    for (int at = 0; at < 4; at++) {
        reset();
        g_alloc_fail_at = at;
        start(&c);
        drive(NULL, 100000000ull);
        assert(apsrc_status(&g_s) == APSRC_NOMEM);
    }
    reset();
    fc.file[0] = 'X';
    start(&c);
    drive(NULL, 1000000);
    assert(apsrc_status(&g_s) == APSRC_BADHDR && apsrc_pack(&g_s) == NULL);
    fc.file[0] = 'C';
}

/* SR9 --- player and reader on one virtual clock. */
static const spec_t SR9_CLIPS[7] = {
    {60, 60000, 40000, 60, 70}, {60, 66000, 48000, 60, 70}, {50, 66000, 57000, 80, 80}, {60, 70000, 62000, 60, 70},
    {40, 50000, 45000, 100, 100}, {60, 66000, 52000, 60, 70}, {30, 60000, 50000, 120, 120},
};

static struct {
    aplay_t a;
    bool on;
    uint64_t next_us;
    int state, cuts_left, bad_cut;
    uint32_t seed, next_cut_ms, base_ms;
    int forced_state;
} P;

static uint32_t rnd(void)
{
    P.seed = P.seed * 1103515245u + 12345u;
    return P.seed >> 8;
}

static int p_ready(void *ctx, int clip)
{
    (void)ctx;
    return apsrc_ready(&g_s, clip);
}

static void player_time(uint64_t until_us)
{
    while (P.on && P.next_us <= until_us) {
        const uint32_t ms = (uint32_t)(P.next_us / 1000);
        const int prev = P.a.running ? P.a.clip : -1;
        if (P.cuts_left > 0 && (int32_t)(ms - P.next_cut_ms) >= 0) {
            P.state = P.forced_state >= 0 ? P.forced_state : (P.state + 1 + (int)(rnd() % 6)) % 7;
            P.cuts_left--;
            P.next_cut_ms = ms + (P.forced_state >= 0 ? 6000u : 1000u + rnd() % 4001u);
            fc.stall_at_us = P.next_us + 300000;
            fc.stall_us = 100000;
        }
        const aplay_out_t o = aplay_tick(&P.a, ms, P.state, APS_ACTIVE);
        if (o.clip != prev) {
            apsrc_want(&g_s, o.clip);
            P.bad_cut += !(o.frame == 0 && apsrc_ready(&g_s, o.clip) >= 1);
        }
        P.next_us += 16000;
    }
}

static uint32_t sr9_run(uint64_t rate, int cuts, int forced_state)
{
    build(SR9_CLIPS, 7, 0);
    reset();
    fc.rate_bps = rate;
    apsrc_cfg_t c = cfg();
    load_ok(&c);
    memset(&P, 0, sizeof P);
    P.seed = 4242;
    P.cuts_left = cuts;
    P.forced_state = forced_state;
    P.state = 0;
    P.next_cut_ms = (uint32_t)(g_us / 1000) + 1500;
    P.next_us = (g_us / 16000 + 1) * 16000;
    aplay_init(&P.a, apsrc_pack(&g_s));
    aplay_set_ready(&P.a, p_ready, NULL);
    P.on = true;
    g_on_time = player_time;
    const uint64_t end_us = g_us + 6000000ull + (uint64_t)cuts * 6000000ull;
    while (g_us < end_us && (P.cuts_left > 0 || g_us < end_us - 5500000ull + (uint64_t)(forced_state >= 0 ? 0 : 5500000ull))) {
        if (!apsrc_step(&g_s)) {
            g_us += 8000;
            player_time(g_us);
        }
    }
    g_on_time = NULL;
    return P.a.starve;
}

/* SR9: at the design rate every cut shows frame 0 in its own tick and never starves; a slower card starves on the heaviest clip. */
static void test_sr9_throughput(void)
{
    const uint32_t ok = sr9_run(1300000, 40, -1);
    assert(P.bad_cut == 0 && P.a.cuts >= 39);
    assert(ok == 0);
    const uint32_t held_ok = sr9_run(1300000, 1, 3);
    assert(held_ok == 0 && P.bad_cut == 0);
    const uint32_t slow = sr9_run(800000, 1, 3);
    assert(slow > 0);
}

#define RUN(fn)                                  \
    do {                                         \
        if (argc < 2 || strstr(#fn, argv[1])) {  \
            fn();                                \
            printf("ok %s\n", #fn);              \
        }                                        \
    } while (0)

int main(int argc, char **argv)
{
    RUN(test_sr1_plan);
    RUN(test_sr2_read_rules);
    RUN(test_sr3_ready_and_data);
    RUN(test_sr4_want_has_head);
    RUN(test_sr5_claim_rereads_want);
    RUN(test_sr6_want_minus_one);
    RUN(test_sr7_crc);
    RUN(test_sr8_inline_retries);
    RUN(test_sr9_throughput);
    RUN(test_sr10_pick);
    RUN(test_sr10_deadline_wrap);
    RUN(test_sr11_animc);
    RUN(test_sr12_heads_write_once);
    RUN(test_sr13_ladder);
    RUN(test_sr14_late_card);
    RUN(test_sr15_error_while_loading);
    RUN(test_sr16_boot_codes);
    puts("apsrc_test ok");
    return 0;
}
