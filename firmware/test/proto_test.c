#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "proto.h"
int main(void)
{
    assert(proto_crc16((const uint8_t *)"123456789", 9, 0xFFFF) == 0x29B1);
    uint8_t store[8192], fr[64], stream[256];
    proto_parser_t p; proto_parser_init(&p, store, sizeof store);
    size_t n = 0;
    memcpy(stream + n, "junk\xC1\xC1", 6); n += 6;
    uint8_t pl[4] = {1, 2, 3, 4};
    size_t fl = proto_encode(fr, T_PING, pl, 4);
    memcpy(stream + n, fr, fl); n += fl;
    fr[fl - 1] ^= 0xFF; /* corrupt crc */
    memcpy(stream + n, fr, fl); n += fl;
    fl = proto_encode(fr, T_SET_STATE, pl, 1);
    memcpy(stream + n, fr, fl); n += fl;
    proto_parser_feed(&p, stream, n);
    proto_frame_t f; int got = 0;
    while (proto_parser_next(&p, 1000, &f)) { got++; printf("type %02x len %u\n", f.type, f.len); proto_parser_release(&p, &f); }
    assert(got == 2 && p.crc_errors == 1);
    /* partial frame times out and is skipped */
    proto_parser_init(&p, store, sizeof store);
    fl = proto_encode(fr, T_PING, pl, 4);
    proto_parser_feed(&p, fr, 5);
    assert(!proto_parser_next(&p, 1000, &f));
    assert(!proto_parser_next(&p, 1100, &f));
    proto_parser_feed(&p, fr + 5, fl - 5);
    assert(proto_parser_next(&p, 1150, &f) == 1);
    proto_parser_release(&p, &f);
    proto_parser_feed(&p, fr, 5);
    assert(!proto_parser_next(&p, 2000, &f));
    assert(!proto_parser_next(&p, 2300, &f));
    assert(p.fill < 5);
    char js[24];
    int jn = snprintf(js, sizeof js, "{\"reason\":\"%s\"}", "a-reason-longer-than-the-buffer");
    assert(jn > (int)sizeof js && proto_json_len(jn, sizeof js) == strlen(js));
    jn = snprintf(js, sizeof js, "{\"proto\":%d}", 3);
    assert(proto_json_len(jn, sizeof js) == strlen(js) && strlen(js) == 11);
    assert(proto_json_len(-1, sizeof js) == 0 && proto_json_len(5, 0) == 0 && proto_json_len(24, 24) == 23);
    /* Red proof: passing the snprintf result as is would send bytes past the buffer. */
    assert((size_t)snprintf(js, sizeof js, "%040d", 0) > sizeof js);
    puts("ok");
    return 0;
}
