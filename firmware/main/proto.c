#include "proto.h"
#include <string.h>
#include "app_config.h"

uint16_t proto_crc16(const uint8_t *data, size_t len, uint16_t crc)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t proto_encode(uint8_t *out, uint8_t type, const void *payload, size_t len)
{
    out[0] = FRAME_SYNC0;
    out[1] = FRAME_SYNC1;
    out[2] = type;
    out[3] = len & 0xFF;
    out[4] = (len >> 8) & 0xFF;
    if (len) {
        memcpy(out + 5, payload, len);
    }
    uint16_t crc = proto_crc16(out + 2, 3 + len, 0xFFFF);
    out[5 + len] = crc & 0xFF;
    out[6 + len] = crc >> 8;
    return len + FRAME_OVERHEAD;
}

void proto_parser_init(proto_parser_t *p, uint8_t *storage, size_t cap)
{
    memset(p, 0, sizeof(*p));
    p->buf = storage;
    p->cap = cap;
}

size_t proto_parser_feed(proto_parser_t *p, const uint8_t *data, size_t len)
{
    size_t room = p->cap - p->fill;
    size_t n = len < room ? len : room;
    memcpy(p->buf + p->fill, data, n);
    p->fill += n;
    return n;
}

static void drop(proto_parser_t *p, size_t n)
{
    p->fill -= n;
    memmove(p->buf, p->buf + n, p->fill);
    p->wait_since_ms = 0;
}

int proto_parser_next(proto_parser_t *p, uint32_t now_ms, proto_frame_t *f)
{
    for (;;) {
        size_t skip = 0;
        while (skip < p->fill && p->buf[skip] != FRAME_SYNC0) {
            skip++;
        }
        if (skip) {
            p->bytes_skipped += skip;
            drop(p, skip);
        }
        if (p->fill < 2) {
            return 0;
        }
        if (p->buf[1] != FRAME_SYNC1) {
            p->bytes_skipped++;
            drop(p, 1);
            continue;
        }
        if (p->fill < 5) {
            goto incomplete;
        }
        uint16_t len = p->buf[3] | (p->buf[4] << 8);
        if (len > PROTO_MAX_PAYLOAD) {
            p->bytes_skipped++;
            drop(p, 1);
            continue;
        }
        size_t total = (size_t)len + FRAME_OVERHEAD;
        if (p->fill < total) {
            goto incomplete;
        }
        uint16_t want = proto_crc16(p->buf + 2, 3 + len, 0xFFFF);
        uint16_t got = p->buf[5 + len] | (p->buf[6 + len] << 8);
        if (want != got) {
            p->crc_errors++;
            p->bytes_skipped++;
            drop(p, 1);
            continue;
        }
        f->type = p->buf[2];
        f->len = len;
        f->payload = p->buf + 5;
        p->frames_ok++;
        p->wait_since_ms = 0;
        return 1;

    incomplete:
        if (p->wait_since_ms == 0) {
            p->wait_since_ms = now_ms ? now_ms : 1;
        } else if (now_ms - p->wait_since_ms >= PROTO_PARTIAL_FRAME_TIMEOUT_MS) {
            p->bytes_skipped++;
            drop(p, 1);
            continue;
        }
        return 0;
    }
}

void proto_parser_release(proto_parser_t *p, const proto_frame_t *f)
{
    drop(p, (size_t)f->len + FRAME_OVERHEAD);
}
