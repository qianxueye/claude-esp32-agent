#include "netwrite.h"
#include "app_config.h"
#include "proto.h"

/* A byte that is not FRAME_SYNC0 at a frame start marks unused space up to the end of the storage. */
#define PAD_MARK 0x00

bool nw_init(netwrite_t *q, uint8_t *storage, uint32_t cap)
{
    if (cap < 16 || (cap & (cap - 1)) != 0) {
        return false;
    }
    q->buf = storage;
    q->cap = cap;
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    q->off = 0;
    q->dropped = 0;
    return true;
}

bool nw_put(netwrite_t *q, uint8_t type, const void *payload, size_t len)
{
    if (len > PROTO_MAX_PAYLOAD) {
        q->dropped++;
        return false;
    }
    uint32_t total = (uint32_t)len + FRAME_OVERHEAD;
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
    uint32_t idx = t & (q->cap - 1);
    uint32_t pad = q->cap - idx < total ? q->cap - idx : 0;
    if (total > q->cap || pad + total > q->cap - (t - h)) {
        q->dropped++;
        return false;
    }
    if (pad) {
        q->buf[idx] = PAD_MARK;
        t += pad;
        idx = 0;
    }
    proto_encode(q->buf + idx, type, payload, len);
    atomic_store_explicit(&q->tail, t + total, memory_order_release);
    return true;
}

/* Points at the unwritten rest of the front frame; returns its length, 0 when none is queued before end. */
static uint32_t front(netwrite_t *q, uint32_t end, const uint8_t **p)
{
    uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (h == end) {
        return 0;
    }
    uint32_t idx = h & (q->cap - 1);
    if (q->buf[idx] != FRAME_SYNC0) {
        h += q->cap - idx;
        atomic_store_explicit(&q->head, h, memory_order_release);
        if (h == end) {
            return 0;
        }
        idx = 0;
    }
    uint32_t len = FRAME_OVERHEAD + (uint32_t)(q->buf[idx + 3] | q->buf[idx + 4] << 8);
    *p = q->buf + idx + q->off;
    return len - q->off;
}

int nw_pump(netwrite_t *q, nw_write_fn write, void *ctx)
{
    uint32_t end = atomic_load_explicit(&q->tail, memory_order_acquire);
    for (;;) {
        const uint8_t *p;
        uint32_t n = front(q, end, &p);
        if (!n) {
            return NW_IDLE;
        }
        int r = write(ctx, p, n);
        if (r == NW_WANT_WRITE || r == 0) {
            return NW_PENDING;
        }
        if (r < 0) {
            return NW_FAILED;
        }
        if ((uint32_t)r < n) {
            q->off += (uint32_t)r;
            continue;
        }
        uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
        uint32_t len = q->off + n;
        q->off = 0;
        atomic_store_explicit(&q->head, h + len, memory_order_release);
    }
}

void nw_discard(netwrite_t *q)
{
    q->off = 0;
    atomic_store_explicit(&q->head, atomic_load_explicit(&q->tail, memory_order_acquire), memory_order_release);
}
