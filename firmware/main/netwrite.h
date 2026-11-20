#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Frame queue and writer for a network session: whole frames in, never a cut frame out (docs/network.md "TX"). */

/* A write callback returns the bytes it took (at least 1), NW_WANT_WRITE, or another negative error. */
#define NW_WANT_WRITE (-2)
typedef int (*nw_write_fn)(void *ctx, const uint8_t *buf, size_t len);

enum { NW_IDLE, NW_PENDING, NW_FAILED };

typedef struct {
    uint8_t *buf;
    uint32_t cap; /* a power of two */
    _Atomic uint32_t head; /* consumer position, counts bytes mod 2^32 */
    _Atomic uint32_t tail; /* producer position */
    uint32_t off; /* bytes of the front frame already taken by the writer */
    uint32_t dropped;
} netwrite_t;

/* Uses storage of cap bytes; returns false unless cap is a power of two of at least 16. */
bool nw_init(netwrite_t *q, uint8_t *storage, uint32_t cap);
/* Encodes and queues one frame, or drops it whole when it does not fit; callers serialise among themselves. */
bool nw_put(netwrite_t *q, uint8_t type, const void *payload, size_t len);
/* Writes queued frames until the queue holds only frames added during the call, the writer
 * wants to wait (call again later: the same bytes are offered) or the writer fails. */
int nw_pump(netwrite_t *q, nw_write_fn write, void *ctx);
/* Session end: drops every queued frame and the rest of a frame being written; consumer side only. */
void nw_discard(netwrite_t *q);
