#include <stdio.h>
#include <string.h>
#include "netwrite.h"
#include "proto.h"

static int s_fail, s_checks;
#define CHECK(name, cond) check((name), (cond), __LINE__)

static void check(const char *name, int ok, int line)
{
    s_checks++;
    if (!ok) {
        s_fail++;
        printf("FAIL netwrite_test.c:%d %s\n", line, name);
    }
}

/* A writer that follows a script of results: a positive value takes up to that many bytes. */
typedef struct {
    const int *script;
    int steps, step;
    uint8_t out[4096];
    size_t n_out;
    const uint8_t *last_p[16];
    size_t last_len[16];
    int calls;
} sink_t;

static int sink_write(void *ctx, const uint8_t *buf, size_t len)
{
    sink_t *s = ctx;
    if (s->calls < 16) {
        s->last_p[s->calls] = buf;
        s->last_len[s->calls] = len;
    }
    s->calls++;
    int r = s->step < s->steps ? s->script[s->step++] : 1 << 20;
    if (r <= 0) {
        return r;
    }
    size_t k = (size_t)r < len ? (size_t)r : len;
    memcpy(s->out + s->n_out, buf, k);
    s->n_out += k;
    return (int)k;
}

static size_t frame(uint8_t *out, uint8_t type, uint8_t fill, size_t len)
{
    uint8_t pl[256];
    memset(pl, fill, len);
    return proto_encode(out, type, pl, len);
}

static bool put(netwrite_t *q, uint8_t type, uint8_t fill, size_t len)
{
    uint8_t pl[256];
    memset(pl, fill, len);
    return nw_put(q, type, pl, len);
}

static void test_partial_writes(void)
{
    static uint8_t store[256];
    netwrite_t q;
    CHECK("init with 256 bytes", nw_init(&q, store, sizeof store));
    put(&q, T_LOG, 'a', 40);
    put(&q, T_PONG, 'b', 4);
    const int script[] = {5, 10, 1000, 1000};
    sink_t s = {.script = script, .steps = 4};
    int r = nw_pump(&q, sink_write, &s);
    uint8_t want[128];
    size_t n = frame(want, T_LOG, 'a', 40);
    n += frame(want + n, T_PONG, 'b', 4);
    CHECK("frame written in three partial writes arrives whole and in order",
          r == NW_IDLE && s.n_out == n && memcmp(s.out, want, n) == 0);
    CHECK("partial writes continue at the next offset",
          s.calls == 4 && s.last_len[0] == 47 && s.last_len[1] == 42 && s.last_len[2] == 32 && s.last_p[1] == s.last_p[0] + 5
              && s.last_p[2] == s.last_p[0] + 15);
}

static void test_want_write(void)
{
    static uint8_t store[256];
    netwrite_t q;
    nw_init(&q, store, sizeof store);
    put(&q, T_LOG, 'a', 40);
    const int script[] = {7, NW_WANT_WRITE};
    sink_t s = {.script = script, .steps = 2};
    CHECK("WANT_WRITE leaves the pump pending", nw_pump(&q, sink_write, &s) == NW_PENDING);
    put(&q, T_PONG, 'b', 4);
    sink_t s2 = {0};
    int r = nw_pump(&q, sink_write, &s2);
    CHECK("WANT_WRITE retries the same pointer and length after another frame was queued",
          s2.last_p[0] == s.last_p[1] && s2.last_len[0] == s.last_len[1] && s.last_len[1] == 40);
    uint8_t want[128];
    size_t n = frame(want, T_LOG, 'a', 40);
    CHECK("the retried frame completes, then the next frame follows",
          r == NW_IDLE && s2.n_out == n - 7 + 11 && memcmp(s.out, want, 7) == 0 && memcmp(s2.out, want + 7, n - 7) == 0);
}

static void test_full_queue(void)
{
    static uint8_t store[64];
    netwrite_t q;
    nw_init(&q, store, sizeof store);
    CHECK("27-byte frame fits", put(&q, T_LOG, 'a', 20));
    CHECK("second 27-byte frame fits", put(&q, T_LOG, 'b', 20));
    CHECK("third 27-byte frame dropped: 10 bytes free", !put(&q, T_LOG, 'c', 20) && q.dropped == 1);
    CHECK("frame larger than the storage dropped", !put(&q, T_LOG, 'd', 60) && q.dropped == 2);
    sink_t s = {0};
    nw_pump(&q, sink_write, &s);
    int seen_c = memchr(s.out, 'c', s.n_out) != NULL;
    CHECK("a full queue drops before the first byte: nothing of the dropped frame is written", s.n_out == 54 && !seen_c);
    CHECK("space returns after the writer drained", put(&q, T_LOG, 'e', 20));
}

static void test_wrap(void)
{
    static uint8_t store[64];
    netwrite_t q;
    nw_init(&q, store, sizeof store);
    put(&q, T_LOG, 'a', 33);
    sink_t s = {0};
    nw_pump(&q, sink_write, &s);
    CHECK("frame that does not fit before the end goes to the start", put(&q, T_LOG, 'b', 33));
    CHECK("but not when the start is still in use", !put(&q, T_LOG, 'c', 20));
    sink_t s2 = {0};
    nw_pump(&q, sink_write, &s2);
    uint8_t want[64];
    size_t n = frame(want, T_LOG, 'b', 33);
    CHECK("frame across the storage end arrives whole", s2.n_out == n && memcmp(s2.out, want, n) == 0 && s2.last_p[0] == store);
    for (int i = 0; i < 200; i++) {
        put(&q, T_PONG, (uint8_t)i, 4 + (size_t)(i % 13));
        sink_t s3 = {0};
        nw_pump(&q, sink_write, &s3);
        n = frame(want, T_PONG, (uint8_t)i, 4 + (size_t)(i % 13));
        if (s3.n_out != n || memcmp(s3.out, want, n) != 0) {
            CHECK("200 frames of varying size across the wrap arrive whole", 0);
            return;
        }
    }
    CHECK("200 frames of varying size across the wrap arrive whole", 1);
}

static void test_session_end(void)
{
    static uint8_t store[256];
    netwrite_t q;
    nw_init(&q, store, sizeof store);
    put(&q, T_LOG, 'a', 40);
    put(&q, T_LOG, 'b', 40);
    const int script[] = {3, -1};
    sink_t s = {.script = script, .steps = 2};
    CHECK("writer error fails the pump", nw_pump(&q, sink_write, &s) == NW_FAILED && s.n_out == 3);
    nw_discard(&q);
    put(&q, T_PONG, 'c', 4);
    sink_t s2 = {0};
    int r = nw_pump(&q, sink_write, &s2);
    uint8_t want[32];
    size_t n = frame(want, T_PONG, 'c', 4);
    CHECK("ending a session mid-frame discards the rest: next session's first byte is SYNC",
          r == NW_IDLE && s2.n_out == n && s2.out[0] == FRAME_SYNC0 && s2.out[1] == FRAME_SYNC1 && memcmp(s2.out, want, n) == 0);
}

static void test_init(void)
{
    static uint8_t store[100];
    netwrite_t q;
    CHECK("storage that is not a power of two refused", !nw_init(&q, store, 100));
    CHECK("storage of 8 bytes refused", !nw_init(&q, store, 8));
}

int main(void)
{
    test_partial_writes();
    test_want_write();
    test_full_queue();
    test_wrap();
    test_session_end();
    test_init();
    printf("netwrite: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
