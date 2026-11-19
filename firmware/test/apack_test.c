#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apack.h"
#include "apack_golden.h"
#include "aplay.h"
#include "app_config.h"
#include "dpower.h"

#define CAP 4096

typedef struct {
    const uint8_t *tok;
    uint32_t len;
    uint16_t meta;
} tframe_t;

typedef struct {
    uint16_t x, y, w, h, colors;
    uint8_t flags;
    int frames;
    tframe_t f[4];
    uint16_t reach; /* 0: the farthest crop corner, rounded up */
} tclip_t;

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

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Recomputes the index CRC after a mutation; skipped when index_len points past the buffer. */
static void reseal(uint8_t *b, uint32_t n)
{
    uint32_t index_len = get32(b + 20);
    if (index_len <= n - AP_HEADER_BYTES) {
        put32(b + 44, ap_crc32(ap_crc32(0, b, 44), b + AP_HEADER_BYTES, index_len));
    }
}

/* Smallest reach in canvas px that covers the farthest crop corner; distances are doubled so an odd side stays exact. */
static uint16_t corner_reach(uint16_t canvas, const tclip_t *c)
{
    const int ex = abs(2 * c->x - canvas) > abs(2 * (c->x + c->w) - canvas) ? abs(2 * c->x - canvas) : abs(2 * (c->x + c->w) - canvas);
    const int ey = abs(2 * c->y - canvas) > abs(2 * (c->y + c->h) - canvas) ? abs(2 * c->y - canvas) : abs(2 * (c->y + c->h) - canvas);
    int r = 0;
    while (4 * r * r < ex * ex + ey * ey) {
        r++;
    }
    return (uint16_t)r;
}

/* Lays out header, clip table, palettes, frame tables and payload in the documented order, then seals the index CRC and every frame CRC. */
static uint32_t build_cap(uint8_t *b, uint32_t cap, uint16_t canvas, uint8_t scale, const tclip_t *c, int n, const uint8_t states[8], uint8_t pickup)
{
    memset(b, 0, cap);
    memcpy(b, AP_MAGIC, 8);
    put16(b + 8, AP_VERSION);
    b[10] = scale;
    b[11] = (uint8_t)n;
    put16(b + 12, canvas);
    put16(b + 14, canvas);
    memcpy(b + 28, states, 8);
    b[36] = pickup;
    b[37] = b[38] = b[39] = AP_NO_CLIP;
    uint32_t off = AP_HEADER_BYTES + (uint32_t)n * AP_CLIP_ENTRY_BYTES, pal[4], tbl[4];
    for (int i = 0; i < n; i++) {
        pal[i] = off;
        for (int k = 0; k < c[i].colors; k++) {
            put16(b + off + 2 * k, 0x0821u * (uint32_t)(k + 1));
        }
        off += 2u * c[i].colors;
        tbl[i] = off;
        off += (uint32_t)c[i].frames * AP_FRAME_ENTRY_BYTES;
    }
    const uint32_t index_len = off - AP_HEADER_BYTES;
    for (int i = 0; i < n; i++) {
        uint8_t *e = b + AP_HEADER_BYTES + i * AP_CLIP_ENTRY_BYTES;
        put16(e, c[i].x);
        put16(e + 2, c[i].y);
        put16(e + 4, c[i].w);
        put16(e + 6, c[i].h);
        put16(e + 8, (uint32_t)c[i].frames);
        put16(e + 10, c[i].colors);
        put32(e + 12, pal[i]);
        put32(e + 16, tbl[i]);
        e[20] = c[i].flags;
        e[21] = AP_CODEC_DELTA;
        put16(e + 22, c[i].reach ? c[i].reach : corner_reach(canvas, &c[i]));
        put16(e + 24, (uint16_t)(int16_t)-20);
        for (int k = 0; k < c[i].frames; k++) {
            uint8_t *fe = b + tbl[i] + k * AP_FRAME_ENTRY_BYTES;
            put32(fe, off);
            put32(fe + 4, c[i].f[k].len);
            put32(fe + 8, ap_crc32(0, c[i].f[k].tok, c[i].f[k].len));
            put16(fe + 12, c[i].f[k].meta);
            memcpy(b + off, c[i].f[k].tok, c[i].f[k].len);
            off += c[i].f[k].len;
        }
    }
    put32(b + 16, off);
    put32(b + 20, index_len);
    reseal(b, off);
    return off;
}

#define build(b, ...) build_cap(b, CAP, __VA_ARGS__)

static uint8_t *dup(const uint8_t *b, uint32_t n)
{
    uint8_t *p = malloc(n ? n : 1);
    assert(p);
    memcpy(p, b, n);
    return p;
}

/* Clip 0: 6 x 5 crop, key frame then a delta; clip 1: 16 x 16 key frame with a long SKIP. */
static const uint8_t K0[] = {0xC2, 1, 0x81, 2, 3, 0x09, 0xCC, 2};
static const uint8_t D0[] = {0x05, 0x81, 1, 2};
static const uint8_t K1[] = {0x7F, 100, 0, 0xDA, 1};
static const uint8_t K0_PIX[30] = {1, 1, 1, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
static const uint8_t D0_PIX[30] = {1, 1, 1, 1, 2, 3, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
static const uint8_t STATES[8] = {0, 1, AP_NO_CLIP, AP_NO_CLIP, AP_NO_CLIP, AP_NO_CLIP, AP_NO_CLIP, AP_NO_CLIP};
static const uint8_t STATES_ONE[8] = {0, AP_NO_CLIP, AP_NO_CLIP, AP_NO_CLIP, AP_NO_CLIP, AP_NO_CLIP, AP_NO_CLIP, AP_NO_CLIP};

static uint32_t base_pack(uint8_t *b)
{
    const tclip_t c[2] = {
        {2, 3, 6, 5, 3, 1, 2, {{K0, sizeof K0, AP_FRAME_KEY | 50}, {D0, sizeof D0, 40}}, 0},
        {0, 0, 16, 16, 1, 0, 1, {{K1, sizeof K1, AP_FRAME_KEY | 30}}, 0},
    };
    return build(b, 16, 1, c, 2, STATES, 1);
}

/* Decodes every frame of every clip in order into exact-size buffers; returns the number of rejected frames. */
static int decode_all(const ap_pack_t *p)
{
    int bad = 0;
    for (int c = 0; c < p->clip_count; c++) {
        const uint32_t area = (uint32_t)p->clip[c].w * p->clip[c].h;
        uint8_t *buf = malloc(area);
        assert(buf);
        for (int k = 0; k < p->clip[c].frames; k++) {
            ap_frame_t f;
            assert(ap_frame(p, c, k, &f));
            bad += !ap_decode(&f, buf, area);
        }
        free(buf);
    }
    return bad;
}

static uint16_t pal_at(const ap_clip_t *c, int i)
{
    return (uint16_t)(c->palette[2 * i] | (c->palette[2 * i + 1] << 8));
}

/* Frames of a parsed pack whose bytes do not match their CRC. */
static int bad_frames(const ap_pack_t *p)
{
    int bad = 0;
    for (int c = 0; c < p->clip_count; c++) {
        for (int k = 0; k < p->clip[c].frames; k++) {
            ap_frame_t f;
            assert(ap_frame(p, c, k, &f));
            bad += !ap_frame_crc_ok(&f);
        }
    }
    return bad;
}

/* The converter's synthetic pack: every field, and every frame decoded in order equals the converter's own decode. */
static void test_p1_golden(void)
{
    uint8_t *m = dup(APACK_GOLDEN_PACK, sizeof APACK_GOLDEN_PACK);
    ap_pack_t p;
    assert(ap_parse(m, sizeof APACK_GOLDEN_PACK, sizeof APACK_GOLDEN_PACK, &p) == AP_OK && bad_frames(&p) == 0);
    assert(p.total_len == sizeof APACK_GOLDEN_PACK && p.canvas_w == 16 && p.canvas_h == 16 && p.scale == 1);
    assert(p.clip_count == 2 && p.state_clip[0] == 0 && p.state_clip[1] == 1 && p.pickup_clip == 0);
    for (int i = 2; i < AP_STATE_SLOTS; i++) {
        assert(p.state_clip[i] == AP_NO_CLIP);
    }
    const ap_clip_t *c0 = &p.clip[0], *c1 = &p.clip[1];
    assert(c0->x == 3 && c0->y == 4 && c0->w == 10 && c0->h == 6 && c0->frames == 2 && c0->colors == 4 && c0->loop);
    assert(c1->x == 0 && c1->y == 2 && c1->w == 16 && c1->h == 12 && c1->frames == 1 && c1->colors == 3 && !c1->loop);
    assert(pal_at(c0, 0) == 0x1082 && pal_at(c0, 1) == 0xCA08 && pal_at(c0, 2) == 0xF79D && pal_at(c0, 3) == 0xFBCB);
    assert(c0->reach == 7 && c1->reach == 10 && !c0->bubble_left && !c1->bubble_left && c0->bubble_y == 0 && c1->bubble_y == 0);
    assert(p.max_area == 192);
    static const uint16_t MS[2][2] = {{50, 40}, {30, 0}};
    for (int c = 0; c < p.clip_count; c++) {
        const uint32_t area = (uint32_t)p.clip[c].w * p.clip[c].h;
        uint8_t *buf = malloc(area);
        assert(buf);
        memset(buf, 0xEE, area);
        for (int k = 0; k < p.clip[c].frames; k++) {
            ap_frame_t f;
            assert(ap_frame(&p, c, k, &f) && f.ms == MS[c][k] && f.key == (k == 0));
            assert(ap_decode(&f, buf, area) && !memcmp(buf, APACK_GOLDEN_CLIPS[c] + (size_t)k * area, area));
        }
        free(buf);
    }
    free(m);
}

static void test_crc_check_value(void)
{
    const uint8_t s[] = "123456789";
    assert(ap_crc32(0, s, 9) == 0xCBF43926u);
    assert(ap_crc32(ap_crc32(0, s, 4), s + 4, 5) == 0xCBF43926u);
    assert(ap_crc32(0, s, 0) == 0);
}

static void test_built_pack_parses(void)
{
    uint8_t b[CAP];
    const uint32_t n = base_pack(b);
    uint8_t *m = dup(b, n);
    ap_pack_t p;
    assert(ap_parse(m, n, n, &p) == AP_OK);
    assert(p.total_len == n && p.canvas_w == 16 && p.canvas_h == 16 && p.scale == 1 && p.clip_count == 2);
    assert(p.state_clip[0] == 0 && p.state_clip[1] == 1 && p.state_clip[2] == AP_NO_CLIP && p.pickup_clip == 1);
    assert(p.clip[0].x == 2 && p.clip[0].y == 3 && p.clip[0].w == 6 && p.clip[0].h == 5 && p.clip[0].loop);
    assert(p.clip[0].frames == 2 && p.clip[0].colors == 3 && !p.clip[1].loop && p.max_area == 256);
    assert(p.clip[0].palette[2] == 0x42 && p.clip[0].palette[3] == 0x10);
    assert(p.clip[0].reach == 8 && p.clip[0].bubble_y == -20 && !p.clip[0].bubble_left && bad_frames(&p) == 0);
    ap_frame_t f;
    assert(ap_frame(&p, 0, 1, &f) && f.ms == 40 && !f.key && f.len == sizeof D0 && f.data == m + f.off);
    assert(!ap_frame(&p, 0, 2, &f) && !ap_frame(&p, 2, 0, &f) && !ap_frame(&p, -1, 0, &f) && !ap_frame(NULL, 0, 0, &f));
    uint8_t buf[30];
    memset(buf, 0x55, sizeof buf);
    assert(ap_frame(&p, 0, 0, &f) && f.key && f.ms == 50 && ap_decode(&f, buf, 30) && !memcmp(buf, K0_PIX, 30));
    assert(ap_frame(&p, 0, 1, &f) && ap_decode(&f, buf, 30) && !memcmp(buf, D0_PIX, 30));
    uint8_t big[256];
    assert(ap_frame(&p, 1, 0, &f) && ap_decode(&f, big, 256));
    for (int i = 0; i < 256; i++) {
        assert(big[i] == (i < 228 ? 0 : 1));
    }
    free(m);
}

/* Models a parser that never looks at the magic: it rewrites the magic and reseals before parsing. */
static ap_status_t parse_without_magic(const uint8_t *b, uint32_t n, ap_pack_t *out)
{
    uint8_t *m = dup(b, n);
    memcpy(m, AP_MAGIC, 8);
    reseal(m, n);
    ap_status_t st = ap_parse(m, n, n, out);
    free(m);
    return st;
}

static void test_p2_empty_and_foreign(void)
{
    const uint32_t n = 64 * 1024;
    uint8_t *m = malloc(n);
    assert(m);
    ap_pack_t p;
    memset(m, 0xFF, n);
    assert(ap_parse(m, n, n, &p) == AP_EMPTY);
    memset(m, 0, n);
    assert(ap_parse(m, n, n, &p) == AP_BAD_HEADER);
    free(m);
    uint8_t b[CAP];
    const uint32_t len = base_pack(b);
    for (uint32_t avail = 0; avail < AP_HEADER_BYTES; avail++) {
        uint8_t *s = dup(b, avail);
        assert(ap_parse(s, avail, avail, &p) == AP_BAD_HEADER);
        free(s);
    }
    assert(ap_parse(NULL, len, len, &p) == AP_BAD_HEADER);
    b[3] ^= 0x20;
    reseal(b, len);
    uint8_t *s = dup(b, len);
    assert(ap_parse(s, len, len, &p) == AP_BAD_HEADER);
    /* Red proof: only the magic check refuses this pack. */
    assert(parse_without_magic(s, len, &p) == AP_OK);
    free(s);
}

typedef void (*mutate_fn)(uint8_t *b);

static ap_status_t parse_mutated(mutate_fn fn, bool seal)
{
    uint8_t b[CAP];
    const uint32_t n = base_pack(b);
    fn(b);
    if (seal) {
        reseal(b, n);
    }
    uint8_t *m = dup(b, n);
    ap_pack_t p;
    ap_status_t st = ap_parse(m, n, n, &p);
    free(m);
    return st;
}

static uint8_t *clip_entry(uint8_t *b, int i)
{
    return b + AP_HEADER_BYTES + i * AP_CLIP_ENTRY_BYTES;
}

static uint8_t *frame_entry(uint8_t *b, int clip, int frame)
{
    return b + get32(clip_entry(b, clip) + 16) + frame * AP_FRAME_ENTRY_BYTES;
}

static void m_version1(uint8_t *b) { put16(b + 8, 1); }
static void m_version3(uint8_t *b) { put16(b + 8, 3); }
static void m_total_short(uint8_t *b) { put32(b + 16, get32(b + 16) - 1); }
static void m_total_long(uint8_t *b) { put32(b + 16, get32(b + 16) + 1); }
static void m_total_below_index(uint8_t *b) { put32(b + 16, AP_HEADER_BYTES + get32(b + 20) - 1); }
static void m_index_huge(uint8_t *b) { put32(b + 20, 0xFFFFFFF0u); }
static void m_scale0(uint8_t *b) { b[10] = 0; }
static void m_scale3(uint8_t *b) { b[10] = 3; }
static void m_clips0(uint8_t *b) { b[11] = 0; }
static void m_clips17(uint8_t *b) { b[11] = 17; }
static void m_canvas513(uint8_t *b) { put16(b + 12, 513); }
static void m_canvas257_x2(uint8_t *b) { b[10] = 2; put16(b + 14, 257); }
static void m_state_ge_count(uint8_t *b) { b[28 + 4] = 2; }
static void m_state_slot7(uint8_t *b) { b[28 + 7] = 7; }
static void m_event_slot2(uint8_t *b) { b[36 + 2] = 2; }
static void m_crop_outside_canvas(uint8_t *b) { put16(clip_entry(b, 0), 11); }
static void m_crop_flipped(uint8_t *b) { put16(clip_entry(b, 0), 3); }
static void m_crop_zero_w(uint8_t *b) { put16(clip_entry(b, 0) + 4, 0); }
static void m_reach_over_cap(uint8_t *b) { put16(clip_entry(b, 0) + 22, 208); }
static void m_reach_below_edge(uint8_t *b) { put16(clip_entry(b, 0) + 22, 4); }
static void m_reach_above_corner(uint8_t *b) { put16(clip_entry(b, 0) + 22, 9); }
static void m_reach0(uint8_t *b) { put16(clip_entry(b, 0) + 22, 0); }
static void m_frames0(uint8_t *b) { put16(clip_entry(b, 0) + 8, 0); }
static void m_colors0(uint8_t *b) { put16(clip_entry(b, 0) + 10, 0); }
static void m_colors251(uint8_t *b) { put16(clip_entry(b, 0) + 10, 251); }
static void m_codec2(uint8_t *b) { clip_entry(b, 0)[21] = 2; }
static void m_palette_in_table(uint8_t *b) { put32(clip_entry(b, 0) + 12, AP_HEADER_BYTES); }
static void m_palette_wraps(uint8_t *b) { put32(clip_entry(b, 0) + 12, 0xFFFFFFFFu); }
static void m_frames_past_index(uint8_t *b) { put32(clip_entry(b, 1) + 16, AP_HEADER_BYTES + get32(b + 20) - 4); }
static void m_frame_off_past_end(uint8_t *b) { put32(frame_entry(b, 0, 1), get32(b + 16) + 1); }
static void m_frame_len_past_end(uint8_t *b) { put32(frame_entry(b, 1, 0) + 4, sizeof K1 + 1); }
static void m_frame_len_huge(uint8_t *b) { put32(frame_entry(b, 1, 0) + 4, 0xFFFFFFFFu); }
static void m_frame1_off_plus1(uint8_t *b) { put32(frame_entry(b, 0, 1), get32(frame_entry(b, 0, 1)) + 1); }
static void m_frame1_off_minus1(uint8_t *b) { put32(frame_entry(b, 0, 1), get32(frame_entry(b, 0, 1)) - 1); }
static void m_frame_off_wraps(uint8_t *b) { put32(frame_entry(b, 0, 0), 0xFFFFFFFEu); }
static void m_frame_off_in_index(uint8_t *b) { put32(frame_entry(b, 0, 0), AP_HEADER_BYTES); }
static void m_frame0_not_key(uint8_t *b) { put16(frame_entry(b, 0, 0) + 12, 50); }
static void m_frame_ms0(uint8_t *b) { put16(frame_entry(b, 0, 1) + 12, 0); }

static void test_p3_index_rules(void)
{
    static const struct { mutate_fn fn; ap_status_t want; } CASES[] = {
        {m_version1, AP_BAD_HEADER}, {m_version3, AP_BAD_HEADER}, {m_total_short, AP_BAD_HEADER}, {m_total_long, AP_BAD_HEADER},
        {m_total_below_index, AP_BAD_HEADER}, {m_index_huge, AP_BAD_HEADER}, {m_scale0, AP_BAD_HEADER},
        {m_scale3, AP_BAD_HEADER}, {m_clips0, AP_BAD_HEADER}, {m_clips17, AP_BAD_HEADER},
        {m_canvas513, AP_BAD_LAYOUT}, {m_canvas257_x2, AP_BAD_LAYOUT}, {m_state_ge_count, AP_BAD_HEADER},
        {m_state_slot7, AP_BAD_HEADER}, {m_event_slot2, AP_BAD_HEADER}, {m_crop_outside_canvas, AP_BAD_LAYOUT},
        {m_crop_zero_w, AP_BAD_LAYOUT}, {m_reach_over_cap, AP_BAD_LAYOUT}, {m_reach_below_edge, AP_BAD_LAYOUT}, {m_reach_above_corner, AP_BAD_LAYOUT},
        {m_reach0, AP_BAD_LAYOUT}, {m_frame_len_huge, AP_BAD_HEADER}, {m_frame1_off_plus1, AP_BAD_HEADER}, {m_frame1_off_minus1, AP_BAD_HEADER},
        {m_frames0, AP_BAD_HEADER}, {m_colors0, AP_BAD_HEADER}, {m_colors251, AP_BAD_HEADER}, {m_codec2, AP_BAD_HEADER},
        {m_palette_in_table, AP_BAD_HEADER}, {m_palette_wraps, AP_BAD_HEADER}, {m_frames_past_index, AP_BAD_HEADER},
        {m_frame_off_past_end, AP_BAD_HEADER}, {m_frame_len_past_end, AP_BAD_HEADER}, {m_frame_off_wraps, AP_BAD_HEADER},
        {m_frame_off_in_index, AP_BAD_HEADER}, {m_frame0_not_key, AP_BAD_HEADER}, {m_frame_ms0, AP_BAD_HEADER},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        ap_status_t st = parse_mutated(CASES[i].fn, true);
        if (st != CASES[i].want) {
            printf("case %zu: status %d, want %d\n", i, st, CASES[i].want);
        }
        assert(st == CASES[i].want);
    }
    /* A flipped crop that every structural rule accepts is caught by the index CRC alone. */
    assert(parse_mutated(m_crop_flipped, false) == AP_BAD_HEADER);
    /* Red proof: the parser without the CRC (here: resealed) accepts it. */
    assert(parse_mutated(m_crop_flipped, true) == AP_OK);
}

/* Clip shown in IDLE by a player handed the view of a mutated pack, whatever ap_parse returned. */
static int idle_clip_of(mutate_fn fn, ap_status_t *st)
{
    uint8_t b[CAP];
    const uint32_t n = base_pack(b);
    if (fn) {
        fn(b);
    }
    reseal(b, n);
    ap_pack_t p;
    *st = ap_parse(b, n, n, &p);
    aplay_t a;
    aplay_init(&a, &p);
    const int clip = aplay_tick(&a, 0, 0, APS_ACTIVE).clip;
    ap_frame_t f;
    assert(*st == AP_OK || (!ap_frame(&p, 0, 0, &f) && p.clip_count == 0 && p.base == NULL));
    return clip;
}

static void test_s2_rejected_view_gives_clawd(void)
{
    ap_status_t st;
    /* One rule fails on clip 0, one on clip 1 after clip 0 has passed every check. */
    assert(idle_clip_of(m_reach_over_cap, &st) == APLAY_CLAWD && st == AP_BAD_LAYOUT);
    assert(idle_clip_of(m_frame_len_past_end, &st) == APLAY_CLAWD && st == AP_BAD_HEADER);
    /* Red proof: the same player plays clip 0 from a populated view, so a view kept after a rejection would play. */
    assert(idle_clip_of(NULL, &st) == 0 && st == AP_OK);
}

static ap_status_t parse_with_colors(uint16_t colors)
{
    uint8_t b[CAP];
    const tclip_t c[1] = {{2, 3, 6, 5, colors, 1, 1, {{K0, sizeof K0, AP_FRAME_KEY | 50}}, 0}};
    const uint32_t n = build(b, 16, 1, c, 1, STATES_ONE, AP_NO_CLIP);
    uint8_t *m = dup(b, n);
    ap_pack_t p;
    ap_status_t st = ap_parse(m, n, n, &p);
    free(m);
    return st;
}

static void test_p3_colour_limit(void)
{
    assert(parse_with_colors(AP_MAX_COLORS) == AP_OK);
    assert(parse_with_colors(AP_MAX_COLORS + 1) == AP_BAD_HEADER);
}

static void test_p3_every_index_byte(void)
{
    uint8_t b[CAP];
    const uint32_t n = base_pack(b);
    const uint32_t index_end = AP_HEADER_BYTES + get32(b + 20);
    static const uint8_t FLIPS[] = {0x01, 0x80, 0xFF};
    static const uint8_t VALUES[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFE, 0xFF};
    ap_pack_t p;
    int resealed_ok = 0;
    for (uint32_t i = 0; i < index_end; i++) {
        for (size_t k = 0; k < sizeof FLIPS; k++) {
            uint8_t *m = dup(b, n);
            m[i] ^= FLIPS[k];
            assert(ap_parse(m, n, n, &p) != AP_OK);
            free(m);
        }
        if (i >= 44 && i < AP_HEADER_BYTES) {
            continue;
        }
        for (size_t k = 0; k < sizeof VALUES; k++) {
            uint8_t *m = dup(b, n);
            m[i] = VALUES[k];
            reseal(m, n);
            if (ap_parse(m, n, n, &p) == AP_OK) {
                resealed_ok++;
                decode_all(&p);
                bad_frames(&p);
            }
            free(m);
        }
    }
    printf("resealed index mutations that still parse: %d\n", resealed_ok);
    for (uint32_t avail = AP_HEADER_BYTES; avail < n; avail++) {
        uint8_t *m = dup(b, avail);
        assert(ap_parse(m, avail, avail, &p) == AP_BAD_HEADER);
        assert(ap_parse(m, avail, n, &p) == (avail < index_end ? AP_BAD_HEADER : AP_OK));
        free(m);
    }
}

static void test_p4_payload(void)
{
    uint8_t b[CAP];
    const uint32_t n = base_pack(b);
    const uint32_t start = AP_HEADER_BYTES + get32(b + 20);
    static const uint8_t VALUES[] = {0x00, 0x01, 0x7E, 0x7F, 0x80, 0xBF, 0xC0, 0xFF};
    ap_pack_t p;
    for (uint32_t i = start; i < n; i++) {
        uint8_t *m = dup(b, n);
        m[i] ^= 0x01;
        /* Red proof: ap_parse alone accepts the flipped payload byte; exactly the one frame holding it fails its CRC. */
        assert(ap_parse(m, n, n, &p) == AP_OK && bad_frames(&p) == 1);
        free(m);
        for (size_t k = 0; k < sizeof VALUES; k++) {
            m = dup(b, n);
            m[i] = VALUES[k];
            assert(ap_parse(m, n, n, &p) == AP_OK);
            decode_all(&p);
            free(m);
        }
    }
}

#define AREA 10
#define GUARD 96

/* The same token walk with every bound check removed. */
static void decode_unchecked(const uint8_t *s, uint32_t len, uint8_t *buf)
{
    const uint8_t *e = s + len;
    uint32_t p = 0;
    while (s < e) {
        uint8_t c = *s++;
        if (c < 0x7F) {
            p += (uint32_t)c + 1;
        } else if (c == 0x7F) {
            p += 128u + (uint32_t)(s[0] | (s[1] << 8));
            s += 2;
        } else if (c < 0xC0) {
            uint32_t n = (uint32_t)c - 0x7F;
            memcpy(buf + p, s, n);
            s += n;
            p += n;
        } else {
            uint32_t n = (uint32_t)c - 0xBE;
            memset(buf + p, *s++, n);
            p += n;
        }
    }
}

/* Decodes tokens held in an exact-size block into AREA bytes followed by a guard; returns the decoder's verdict. */
static bool decode_guarded(const uint8_t *tok, uint16_t len, bool key, bool *guard_ok)
{
    uint8_t *t = dup(tok, len), *buf = malloc(AREA + GUARD);
    assert(buf);
    memset(buf, 0xA5, AREA + GUARD);
    ap_frame_t f = {.data = t, .len = len, .ms = 10, .key = key};
    bool ok = ap_decode(&f, buf, AREA);
    *guard_ok = true;
    for (int i = AREA; i < AREA + GUARD; i++) {
        *guard_ok = *guard_ok && buf[i] == 0xA5;
    }
    free(t);
    free(buf);
    return ok;
}

static bool unchecked_keeps_guard(const uint8_t *tok, uint16_t len)
{
    uint8_t *buf = malloc(AREA + GUARD);
    assert(buf);
    memset(buf, 0xA5, AREA + GUARD);
    decode_unchecked(tok, len, buf);
    bool kept = true;
    for (int i = AREA; i < AREA + GUARD; i++) {
        kept = kept && buf[i] == 0xA5;
    }
    free(buf);
    return kept;
}

static void test_p5_decoder_bounds(void)
{
    static const uint8_t SKIP_OVER[] = {0x0A};                       /* SKIP 11 */
    static const uint8_t SKIP_LONG_OVER[] = {0x7F, 0x00, 0x00};      /* SKIP 128 */
    static const uint8_t LIT_OVER[] = {0x85, 1, 2, 3, 4, 5, 6, 0x89, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}; /* 6 + 10 */
    static const uint8_t FILL_OVER[] = {0x02, 0xC7, 9};              /* SKIP 3, FILL 9 */
    static const uint8_t LIT_SHORT[] = {0x84, 1, 2, 3};              /* LIT 5 with 3 bytes */
    static const uint8_t FILL_SHORT[] = {0xC0};                      /* FILL without its value */
    static const uint8_t SKIP_LONG_SHORT[] = {0x7F, 0x00};           /* long SKIP with one length byte */
    static const uint8_t EXACT[] = {0x01, 0x83, 1, 2, 3, 4, 0xC2, 7}; /* SKIP 2, LIT 4, FILL 4: exactly 10 */
    static const struct { const uint8_t *t; uint16_t n; bool ok; } CASES[] = {
        {SKIP_OVER, sizeof SKIP_OVER, false}, {SKIP_LONG_OVER, sizeof SKIP_LONG_OVER, false},
        {LIT_OVER, sizeof LIT_OVER, false}, {FILL_OVER, sizeof FILL_OVER, false}, {LIT_SHORT, sizeof LIT_SHORT, false},
        {FILL_SHORT, sizeof FILL_SHORT, false}, {SKIP_LONG_SHORT, sizeof SKIP_LONG_SHORT, false}, {EXACT, sizeof EXACT, true},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        for (int key = 0; key < 2; key++) {
            bool guard;
            assert(decode_guarded(CASES[i].t, CASES[i].n, key, &guard) == CASES[i].ok && guard);
        }
    }
    /* Red proof: without the checks the overlong LIT and FILL write into the guard. */
    assert(!unchecked_keeps_guard(LIT_OVER, sizeof LIT_OVER));
    assert(!unchecked_keeps_guard(FILL_OVER, sizeof FILL_OVER));
    ap_frame_t f = {.data = EXACT, .len = sizeof EXACT, .ms = 10, .key = true};
    assert(!ap_decode(NULL, (uint8_t[1]){0}, 1) && !ap_decode(&f, NULL, AREA));
}

static bool run_tokens(const uint8_t *tok, uint16_t len, bool key, uint8_t *buf, uint32_t area)
{
    uint8_t *t = dup(tok, len);
    ap_frame_t f = {.data = t, .len = len, .ms = 10, .key = key};
    bool ok = ap_decode(&f, buf, area);
    free(t);
    return ok;
}

static void test_p6_token_semantics(void)
{
    uint8_t buf[300];
    memset(buf, 0x55, sizeof buf);
    static const uint8_t SKIP_ALL[] = {0x7F, 172, 0}; /* SKIP 300 */
    assert(run_tokens(SKIP_ALL, sizeof SKIP_ALL, true, buf, 300));
    for (int i = 0; i < 300; i++) {
        assert(buf[i] == 0);
    }
    /* Red proof: the same frame decoded without its key flag keeps the stale pixels. */
    memset(buf, 0x55, sizeof buf);
    assert(run_tokens(SKIP_ALL, sizeof SKIP_ALL, false, buf, 300) && buf[0] == 0x55 && buf[299] == 0x55);
    memset(buf, 0x55, sizeof buf);
    static const uint8_t DELTA[] = {0x00, 0x80, 9, 0x7E, 0xC0, 4, 0x7F, 0, 0, 0xE7, 6};
    assert(run_tokens(DELTA, sizeof DELTA, false, buf, 300));
    assert(buf[0] == 0x55 && buf[1] == 9);
    for (int i = 2; i < 129; i++) {
        assert(buf[i] == 0x55);
    }
    assert(buf[129] == 4 && buf[130] == 4 && buf[131] == 0x55);
    for (int i = 131; i < 259; i++) {
        assert(buf[i] == 0x55);
    }
    for (int i = 259; i < 300; i++) {
        assert(buf[i] == 6);
    }
    memset(buf, 0x55, sizeof buf);
    uint8_t lit[65] = {0xBF};
    for (int i = 1; i < 65; i++) {
        lit[i] = (uint8_t)i;
    }
    assert(run_tokens(lit, sizeof lit, false, buf, 300));
    assert(buf[0] == 1 && buf[63] == 64 && buf[64] == 0x55);
    static const uint8_t FILL65_THEN_LONG_MAX[] = {0xFF, 3, 0x7F, 0xFF, 0xFF};
    assert(!run_tokens(FILL65_THEN_LONG_MAX, sizeof FILL65_THEN_LONG_MAX, false, buf, 300));
    assert(buf[0] == 3 && buf[64] == 3 && buf[65] == 0x55);
    static const uint8_t NONE[1] = {0};
    memset(buf, 0x55, sizeof buf);
    assert(run_tokens(NONE, 0, false, buf, 300) && buf[0] == 0x55);
    assert(run_tokens(NONE, 0, true, buf, 300) && buf[0] == 0 && buf[299] == 0);
}

static void test_layout_boundaries(void)
{
    /* Canvas 414 centres on 207: the full-canvas crop has edge distance 207, so reach 207 passes and 208 breaks the cap. */
    assert(ap_layout_ok(414, 414, 1, 0, 0, 414, 414, 207));
    assert(!ap_layout_ok(414, 414, 1, 0, 0, 414, 414, 208));
    assert(!ap_layout_ok(414, 414, 1, 0, 0, 414, 414, 206));
    /* Canvas 16, crop 3,4 10x6: edge distance 5, farthest corner hypot(5, 4) = 6.4, so reach 5 to 7 passes. */
    assert(ap_layout_ok(16, 16, 1, 3, 4, 10, 6, 5) && ap_layout_ok(16, 16, 1, 3, 4, 10, 6, 7));
    assert(!ap_layout_ok(16, 16, 1, 3, 4, 10, 6, 4) && !ap_layout_ok(16, 16, 1, 3, 4, 10, 6, 8) && !ap_layout_ok(16, 16, 1, 3, 4, 10, 6, 0));
    /* Canvas 250 is 500 px at scale 2: reach 104 is 208 px and fails, and passes at scale 1; reach 103 is below the edge distance. */
    assert(ap_layout_ok(250, 250, 1, 21, 21, 208, 208, 104) && !ap_layout_ok(250, 250, 2, 21, 21, 208, 208, 104));
    assert(!ap_layout_ok(250, 250, 1, 21, 21, 208, 208, 103));
    assert(ap_layout_ok(512, 512, 1, 250, 250, 12, 12, 9));
    assert(!ap_layout_ok(513, 200, 1, 250, 100, 1, 1, 130) && !ap_layout_ok(200, 513, 1, 100, 250, 1, 1, 130));
    assert(!ap_layout_ok(257, 100, 2, 0, 50, 1, 1, 100));
    assert(!ap_layout_ok(16, 16, 1, 10, 0, 7, 1, 8) && !ap_layout_ok(16, 16, 1, 0, 10, 1, 7, 8));
    assert(!ap_layout_ok(16, 16, 0, 0, 0, 1, 1, 8) && !ap_layout_ok(16, 16, 3, 0, 0, 1, 1, 8) && !ap_layout_ok(0, 16, 1, 0, 0, 1, 1, 8));
    assert(!ap_layout_ok(16, 16, 1, 0, 0, 0, 1, 8) && !ap_layout_ok(16, 16, 1, 0, 0, 1, 0, 8));
    assert(!ap_layout_ok(16, 16, 1, 0xFFFF, 0, 2, 1, 8) && !ap_layout_ok(16, 16, 1, 0, 0xFFFF, 1, 2, 8));
}

/* Largest distance from the scene centre of a point reach px from the canvas centre in 360 directions, over the 48 orbit points. */
static double scene_radius(int reach, bool recentre)
{
    double worst = 0;
    for (int i = 0; i < ORBIT_COLS * ORBIT_ROWS; i++) {
        int gx, gy;
        dp_orbit(i, &gx, &gy);
        const double cx = recentre ? gx + ORBIT_RECENTRE_X_PX : gx, cy = recentre ? gy + ORBIT_RECENTRE_Y_PX : gy;
        for (int deg = 0; deg < 360; deg++) {
            const double a = deg * M_PI / 180.0, r = hypot(cx + reach * cos(a), cy + reach * sin(a));
            worst = r > worst ? r : worst;
        }
    }
    return worst;
}

static void test_l1_art_reach_in_scene(void)
{
    assert(scene_radius(AP_ART_REACH_MAX_PX, true) <= SCENE_MAX_RADIUS_PX);
    /* Red proofs: reach 208 leaves the 225 px glass bound at 225.2, and the raw lattice without the recentre at 233.8. */
    assert(fabs(scene_radius(AP_ART_REACH_MAX_PX + 1, true) - 225.2) < 0.05 && scene_radius(AP_ART_REACH_MAX_PX + 1, true) > SCENE_MAX_RADIUS_PX);
    assert(fabs(scene_radius(AP_ART_REACH_MAX_PX, false) - 233.8) < 0.05 && scene_radius(AP_ART_REACH_MAX_PX, false) > SCENE_MAX_RADIUS_PX);
}

_Static_assert(AP_ART_REACH_MAX_PX == 207 && AP_CANVAS_MAX_PX == 512, "the caps the layout tests pin");

#define BIG_LEN 70000u

/* A frame longer than 16 bits: 70,000 one-pixel SKIPs over a 280 x 250 key frame. */
static void test_p7_long_frame(void)
{
    static uint8_t tok[BIG_LEN];
    memset(tok, 0x00, sizeof tok);
    const uint32_t cap = 4096 + BIG_LEN;
    uint8_t *b = malloc(cap), *buf = malloc(BIG_LEN);
    assert(b && buf);
    const tclip_t c[1] = {{10, 25, 280, 250, 1, 1, 1, {{tok, BIG_LEN, AP_FRAME_KEY | 50}}, 0}};
    const uint32_t n = build_cap(b, cap, 300, 1, c, 1, STATES_ONE, AP_NO_CLIP);
    ap_pack_t p;
    assert(ap_parse(b, n, n, &p) == AP_OK && p.max_area == BIG_LEN);
    ap_frame_t f;
    assert(ap_frame(&p, 0, 0, &f) && f.len == BIG_LEN && f.data == b + f.off && ap_frame_crc_ok(&f));
    memset(buf, 0x55, BIG_LEN);
    assert(ap_decode(&f, buf, BIG_LEN) && buf[0] == 0 && buf[BIG_LEN - 1] == 0);
    /* Red proof: a 16-bit length reads 4,464 bytes, which neither covers the frame nor matches its CRC. */
    ap_frame_t cut = f;
    cut.len = (uint16_t)f.len;
    assert(cut.len == 4464 && !ap_frame_crc_ok(&cut));
    free(b);
    free(buf);
}

static void test_f1_frame_crc(void)
{
    ap_pack_t p;
    assert(ap_parse(APACK_GOLDEN_PACK, sizeof APACK_GOLDEN_PACK, sizeof APACK_GOLDEN_PACK, &p) == AP_OK && bad_frames(&p) == 0);
    uint8_t *m = dup(APACK_GOLDEN_PACK, sizeof APACK_GOLDEN_PACK);
    assert(ap_parse(m, sizeof APACK_GOLDEN_PACK, sizeof APACK_GOLDEN_PACK, &p) == AP_OK);
    ap_frame_t f;
    assert(ap_frame(&p, 0, 1, &f));
    m[f.off] ^= 0x01;
    /* Red proof: only frame 1 of clip 0 fails; the other two frames still match. */
    assert(!ap_frame_crc_ok(&f) && bad_frames(&p) == 1);
    assert(ap_frame(&p, 0, 0, &f) && ap_frame_crc_ok(&f) && ap_frame(&p, 1, 0, &f) && ap_frame_crc_ok(&f));
    free(m);
}

static void test_f2_parse_without_payload(void)
{
    uint8_t b[CAP];
    const uint32_t n = base_pack(b), payload_off = AP_HEADER_BYTES + get32(b + 20);
    ap_pack_t full, head;
    assert(ap_parse(b, n, n, &full) == AP_OK && ap_parse(b, payload_off, n, &head) == AP_OK);
    for (int c = 0; c < full.clip_count; c++) {
        for (int k = 0; k < full.clip[c].frames; k++) {
            ap_frame_t a, h;
            assert(ap_frame(&full, c, k, &a) && ap_frame(&head, c, k, &h));
            assert(a.data == b + a.off && h.data == NULL && h.off == a.off && h.len == a.len && h.crc == a.crc && h.ms == a.ms && h.key == a.key);
            uint8_t buf[256];
            assert(!ap_decode(&h, buf, sizeof buf) && !ap_frame_crc_ok(&h));
        }
    }
    /* Red proofs: a limit below the total, and an index cut short, are both refused. */
    assert(ap_parse(b, n, n - 1, &head) == AP_BAD_HEADER);
    assert(ap_parse(b, payload_off - 1, n, &head) == AP_BAD_HEADER);
}

static void test_f3_frames_contiguous(void)
{
    /* Red proof: with the contiguity rule dropped, frame 1 at +1 still lies inside the payload and parses. */
    assert(parse_mutated(m_frame1_off_plus1, true) == AP_BAD_HEADER);
    assert(parse_mutated(m_frame1_off_minus1, true) == AP_BAD_HEADER);
}

int main(void)
{
    test_crc_check_value();
    test_p1_golden();
    test_built_pack_parses();
    test_p2_empty_and_foreign();
    test_p3_index_rules();
    test_s2_rejected_view_gives_clawd();
    test_p3_colour_limit();
    test_p3_every_index_byte();
    test_p4_payload();
    test_p5_decoder_bounds();
    test_p6_token_semantics();
    test_layout_boundaries();
    test_l1_art_reach_in_scene();
    test_p7_long_frame();
    test_f1_frame_crc();
    test_f2_parse_without_payload();
    test_f3_frames_contiguous();
    puts("apack_test ok");
    return 0;
}
