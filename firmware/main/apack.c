#include "apack.h"
#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t ap_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ T[crc & 15];
        crc = (crc >> 4) ^ T[crc & 15];
    }
    return ~crc;
}

/* True when [off, off + len) lies inside [lo, hi); written so no sum can wrap. */
static bool inside(uint32_t off, uint32_t len, uint32_t lo, uint32_t hi)
{
    return off >= lo && off <= hi && len <= hi - off;
}

bool ap_layout_ok(uint16_t canvas_w, uint16_t canvas_h, uint8_t scale, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (scale < 1 || scale > 2 || canvas_w == 0 || canvas_h == 0 || w == 0 || h == 0) {
        return false;
    }
    const int32_t cw = (int32_t)canvas_w * scale, ch = (int32_t)canvas_h * scale;
    if (cw > AP_CANVAS_MAX_PX || ch > AP_CANVAS_MAX_PX) {
        return false;
    }
    if ((int32_t)x + w > canvas_w || (int32_t)y + h > canvas_h) {
        return false;
    }
    const int32_t top = (int32_t)y * scale - ch / 2, bottom = ((int32_t)y + h) * scale - ch / 2;
    return top >= -AP_SAFE_UP_PX && bottom <= AP_SAFE_DOWN_PX;
}

static bool clip_ok(uint8_t v, uint8_t clip_count)
{
    return v == AP_NO_CLIP || v < clip_count;
}

static ap_status_t parse_clip(const ap_pack_t *p, const uint8_t *e, uint32_t tbl_end, uint32_t idx_end, ap_clip_t *c)
{
    c->x = rd16(e);
    c->y = rd16(e + 2);
    c->w = rd16(e + 4);
    c->h = rd16(e + 6);
    c->frames = rd16(e + 8);
    c->colors = rd16(e + 10);
    const uint32_t pal_off = rd32(e + 12), frm_off = rd32(e + 16);
    c->loop = (e[20] & 1) != 0;
    if (e[21] != AP_CODEC_DELTA || c->colors < 1 || c->colors > AP_MAX_COLORS || c->frames == 0) {
        return AP_BAD_HEADER;
    }
    if (!inside(pal_off, (uint32_t)c->colors * 2, tbl_end, idx_end) ||
        !inside(frm_off, (uint32_t)c->frames * AP_FRAME_ENTRY_BYTES, tbl_end, idx_end)) {
        return AP_BAD_HEADER;
    }
    c->palette = p->base + pal_off;
    c->frame_tbl = p->base + frm_off;
    for (uint32_t i = 0; i < c->frames; i++) {
        const uint8_t *fe = c->frame_tbl + i * AP_FRAME_ENTRY_BYTES;
        const uint16_t meta = rd16(fe + 6);
        if (!inside(rd32(fe), rd16(fe + 4), p->payload_off, p->total_len) || (meta & AP_FRAME_MS_MASK) == 0) {
            return AP_BAD_HEADER;
        }
        if (i == 0 && !(meta & AP_FRAME_KEY)) {
            return AP_BAD_HEADER;
        }
    }
    if (!ap_layout_ok(p->canvas_w, p->canvas_h, p->scale, c->x, c->y, c->w, c->h)) {
        return AP_BAD_LAYOUT;
    }
    return AP_OK;
}

ap_status_t ap_parse(const uint8_t *base, uint32_t avail, ap_pack_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!base || avail < AP_HEADER_BYTES) {
        return AP_BAD_HEADER;
    }
    bool erased = true;
    for (int i = 0; i < AP_HEADER_BYTES; i++) {
        erased = erased && base[i] == 0xFF;
    }
    if (erased) {
        return AP_EMPTY;
    }
    if (memcmp(base, AP_MAGIC, 8) != 0 || rd16(base + 8) != AP_VERSION) {
        return AP_BAD_HEADER;
    }
    const uint32_t total = rd32(base + 16), index_len = rd32(base + 20);
    if (total > avail || total < AP_HEADER_BYTES || index_len > total - AP_HEADER_BYTES) {
        return AP_BAD_HEADER;
    }
    uint32_t crc = ap_crc32(0, base, 44);
    if (ap_crc32(crc, base + AP_HEADER_BYTES, index_len) != rd32(base + 44)) {
        return AP_BAD_HEADER;
    }
    out->base = base;
    out->total_len = total;
    out->payload_off = AP_HEADER_BYTES + index_len;
    out->payload_crc = rd32(base + 24);
    out->scale = base[10];
    out->clip_count = base[11];
    out->canvas_w = rd16(base + 12);
    out->canvas_h = rd16(base + 14);
    if (out->scale < 1 || out->scale > 2 || out->clip_count < 1 || out->clip_count > AP_MAX_CLIPS) {
        return AP_BAD_HEADER;
    }
    if (out->canvas_w == 0 || out->canvas_h == 0 || (uint32_t)out->canvas_w * out->scale > AP_CANVAS_MAX_PX ||
        (uint32_t)out->canvas_h * out->scale > AP_CANVAS_MAX_PX) {
        return AP_BAD_LAYOUT;
    }
    const uint32_t tbl_end = AP_HEADER_BYTES + (uint32_t)out->clip_count * AP_CLIP_ENTRY_BYTES;
    if (tbl_end > out->payload_off) {
        return AP_BAD_HEADER;
    }
    for (int i = 0; i < AP_STATE_SLOTS; i++) {
        out->state_clip[i] = base[28 + i];
        if (!clip_ok(out->state_clip[i], out->clip_count)) {
            return AP_BAD_HEADER;
        }
    }
    for (int i = 0; i < AP_EVENT_SLOTS; i++) {
        if (!clip_ok(base[36 + i], out->clip_count)) {
            return AP_BAD_HEADER;
        }
    }
    out->pickup_clip = base[36];
    for (int i = 0; i < out->clip_count; i++) {
        ap_clip_t *c = &out->clip[i];
        ap_status_t st = parse_clip(out, base + AP_HEADER_BYTES + i * AP_CLIP_ENTRY_BYTES, tbl_end, out->payload_off, c);
        if (st != AP_OK) {
            return st;
        }
        const uint32_t area = (uint32_t)c->w * c->h;
        out->max_area = area > out->max_area ? area : out->max_area;
    }
    return AP_OK;
}

bool ap_payload_ok(const ap_pack_t *p)
{
    return p && p->base && ap_crc32(0, p->base + p->payload_off, p->total_len - p->payload_off) == p->payload_crc;
}

bool ap_frame(const ap_pack_t *p, int clip, int frame, ap_frame_t *out)
{
    if (!p || !p->base || clip < 0 || clip >= p->clip_count || frame < 0 || frame >= p->clip[clip].frames) {
        return false;
    }
    const uint8_t *fe = p->clip[clip].frame_tbl + (uint32_t)frame * AP_FRAME_ENTRY_BYTES;
    const uint32_t off = rd32(fe);
    const uint16_t len = rd16(fe + 4), meta = rd16(fe + 6);
    if (!inside(off, len, p->payload_off, p->total_len)) {
        return false;
    }
    out->data = p->base + off;
    out->len = len;
    out->ms = meta & AP_FRAME_MS_MASK;
    out->key = (meta & AP_FRAME_KEY) != 0;
    return true;
}

bool ap_decode(const ap_frame_t *f, uint8_t *buf, uint32_t area)
{
    if (!f || !buf) {
        return false;
    }
    if (f->key) {
        memset(buf, 0, area);
    }
    const uint8_t *s = f->data;
    uint32_t left = f->len, p = 0;
    while (left > 0) {
        const uint8_t c = *s++;
        left--;
        uint32_t n;
        if (c < 0x7F) {
            n = (uint32_t)c + 1;
        } else if (c == 0x7F) {
            if (left < 2) {
                return false;
            }
            n = 128u + rd16(s);
            s += 2;
            left -= 2;
        } else if (c < 0xC0) {
            n = (uint32_t)c - 0x7F;
            if (n > left || n > area - p) {
                return false;
            }
            memcpy(buf + p, s, n);
            s += n;
            left -= n;
            p += n;
            continue;
        } else {
            if (left < 1) {
                return false;
            }
            n = (uint32_t)c - 0xBE;
            const uint8_t v = *s++;
            left--;
            if (n > area - p) {
                return false;
            }
            memset(buf + p, v, n);
            p += n;
            continue;
        }
        if (n > area - p) {
            return false;
        }
        p += n;
    }
    return true;
}
