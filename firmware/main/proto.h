#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FRAME_SYNC0 0xC1
#define FRAME_SYNC1 0xAD
#define FRAME_OVERHEAD 7

enum {
    T_HELLO = 0x01, T_LOG = 0x02, T_MIC_AUDIO = 0x10, T_INPUT = 0x20, T_SPK_STATUS = 0x21, T_VOLUME = 0x22,
    T_PONG = 0x30, T_SELFTEST_RESULT = 0x31, T_SNAP_BEGIN = 0x32, T_SNAP_DATA = 0x33, T_SNAP_END = 0x34, T_PAIR_RESULT = 0x35, T_WIFI_STATUS = 0x36, T_FORGET_RESULT = 0x37,
    T_HOST_HELLO = 0x81, T_SET_STATE = 0x82, T_SET_VOLUME = 0x83, T_PAIR = 0x84, T_FORGET = 0x85, T_SET_REMOTE = 0x86,
    T_SPK_AUDIO = 0x90, T_SPK_FLUSH = 0x91, T_SPK_END = 0x92,
    T_PING = 0xA0, T_SELFTEST = 0xA2, T_SNAPSHOT = 0xA3, T_DEBUG = 0xF0,
};

uint16_t proto_crc16(const uint8_t *data, size_t len, uint16_t crc);
/* Writes a complete frame to out (capacity >= len + FRAME_OVERHEAD); returns its size. */
size_t proto_encode(uint8_t *out, uint8_t type, const void *payload, size_t len);

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t fill;
    uint32_t wait_since_ms;
    uint32_t crc_errors;
    uint32_t frames_ok;
    uint32_t bytes_skipped;
    bool stream; /* a stream link: partial frames never time out */
} proto_parser_t;

typedef struct {
    uint8_t type;
    uint16_t len;
    const uint8_t *payload;
} proto_frame_t;

void proto_parser_init(proto_parser_t *p, uint8_t *storage, size_t cap);
/* For TCP and the tunnel: a frame is never cut there, so the partial-frame timeout is off. */
void proto_parser_set_stream(proto_parser_t *p, bool stream);
/* Copies up to the free space of bytes into the parser; returns the count consumed. */
size_t proto_parser_feed(proto_parser_t *p, const uint8_t *data, size_t len);
/* Returns 1 and fills f when a frame is ready (valid until proto_parser_release), 0 when more bytes are needed. */
int proto_parser_next(proto_parser_t *p, uint32_t now_ms, proto_frame_t *f);
void proto_parser_release(proto_parser_t *p, const proto_frame_t *f);
