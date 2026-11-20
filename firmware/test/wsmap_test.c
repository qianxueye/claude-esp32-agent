#include <stdio.h>
#include "wsmap.h"

static int s_fail, s_checks;
#define CHECK(name, cond) check((name), (cond), __LINE__)

static void check(const char *name, int ok, int line)
{
    s_checks++;
    if (!ok) {
        s_fail++;
        printf("FAIL wsmap_test.c:%d %s\n", line, name);
    }
}

/* Inputs follow what transport_ws reports with control frames handled inside the transport (docs/network.md). */
static void test_read(void)
{
    CHECK("binary frame, 640 bytes read: data", wsmap_read(640, WS_OP_BINARY, 640) == WSR_DATA);
    CHECK("continuation frame, 100 bytes read: data", wsmap_read(100, WS_OP_CONT, 100) == WSR_DATA);
    CHECK("0 after an answered PING (opcode cleared, payload 4): want read, not end of stream",
          wsmap_read(0, WS_OP_NONE, 4) == WSR_WANT_READ);
    CHECK("0 after a PONG (opcode cleared): want read, not end of stream", wsmap_read(0, WS_OP_NONE, 0) == WSR_WANT_READ);
    CHECK("0 with opcode PING reported: want read", wsmap_read(0, WS_OP_PING, 0) == WSR_WANT_READ);
    CHECK("0 with opcode PONG reported: want read", wsmap_read(0, WS_OP_PONG, 0) == WSR_WANT_READ);
    CHECK("CLOSE reported: end of stream", wsmap_read(0, WS_OP_CLOSE, 2) == WSR_EOS);
    CHECK("CLOSE handled inside the transport, then the next read fails: session ends",
          wsmap_read(0, WS_OP_NONE, 2) == WSR_WANT_READ && wsmap_read(-1, WS_OP_NONE, 2) == WSR_FAIL);
    CHECK("text message with 5 bytes: session close", wsmap_read(5, WS_OP_TEXT, 5) == WSR_TEXT);
    CHECK("empty text message: session close", wsmap_read(0, WS_OP_TEXT, 0) == WSR_TEXT);
    CHECK("read timeout before a header (opcode cleared, stale payload 640): want read",
          wsmap_read(0, WS_OP_NONE, 640) == WSR_WANT_READ);
    CHECK("0 inside a 640-byte binary payload: session ends", wsmap_read(0, WS_OP_BINARY, 640) == WSR_FAIL);
    CHECK("0 inside a continuation payload: session ends", wsmap_read(0, WS_OP_CONT, 100) == WSR_FAIL);
    CHECK("empty binary message: want read", wsmap_read(0, WS_OP_BINARY, 0) == WSR_WANT_READ);
    CHECK("read -1 (closed by FIN): session ends", wsmap_read(-1, WS_OP_BINARY, 640) == WSR_FAIL);
    CHECK("read -2 (connection failed): session ends", wsmap_read(-2, WS_OP_NONE, 0) == WSR_FAIL);
    CHECK("bytes without a header: session ends", wsmap_read(5, WS_OP_NONE, 0) == WSR_FAIL);
    CHECK("reserved opcode 3: session ends", wsmap_read(5, 0x3, 5) == WSR_FAIL);
}

static void test_write(void)
{
    CHECK("own poll timed out: want write, nothing sent", wsmap_write(0, 0, 100) == WSW_WANT_WRITE);
    CHECK("whole 100-byte message sent", wsmap_write(1, 100, 100) == WSW_SENT);
    CHECK("0 after the poll (header out, payload timed out): session ends", wsmap_write(1, 0, 100) == WSW_FAIL);
    CHECK("60 of 100 bytes sent: session ends", wsmap_write(1, 60, 100) == WSW_FAIL);
    CHECK("send -1 after the poll: session ends", wsmap_write(1, -1, 100) == WSW_FAIL);
    CHECK("own poll failed: session ends", wsmap_write(-1, 0, 100) == WSW_FAIL);
}

int main(void)
{
    test_read();
    test_write();
    printf("wsmap: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
