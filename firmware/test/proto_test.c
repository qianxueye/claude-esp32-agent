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
    /* stream mode: a partial frame waits for its bytes however long they take */
    proto_parser_init(&p, store, sizeof store);
    proto_parser_set_stream(&p, true);
    fl = proto_encode(fr, T_PING, pl, 4);
    proto_parser_feed(&p, fr, 5);
    assert(!proto_parser_next(&p, 1000, &f));
    assert(!proto_parser_next(&p, 60000, &f));
    assert(p.fill == 5 && p.bytes_skipped == 0);
    proto_parser_feed(&p, fr + 5, fl - 5);
    assert(proto_parser_next(&p, 120000, &f) == 1 && f.type == T_PING && f.len == 4);
    proto_parser_release(&p, &f);
    assert(T_SET_REMOTE == 0x86);
    /* a PAIR frame is wiped on release; the frames after it in the same read still parse */
    static const char secret[] = "{\"psk\":\"SECRET-PSK-TEXT\"}";
    proto_parser_init(&p, store, sizeof store);
    memset(store, 0xEE, sizeof store);
    n = 0;
    memcpy(stream + n, "zz", 2); n += 2;
    n += proto_encode(stream + n, T_PAIR, secret, sizeof secret - 1);
    n += proto_encode(stream + n, T_PING, pl, 4);
    fl = proto_encode(fr, T_SET_STATE, pl, 1);
    memcpy(stream + n, fr, 3); n += 3;
    assert(proto_parser_feed(&p, stream, n) == n);
    assert(proto_parser_next(&p, 1000, &f) == 1 && f.type == T_PAIR);
    proto_parser_release_wipe(&p, &f);
    static const char value[] = "SECRET-PSK-TEXT";
    for (size_t k = 0; k + 4 < sizeof value; k++) {
        for (size_t i = 0; i + 4 <= sizeof store; i++) {
            assert(memcmp(store + i, value + k, 4) != 0); /* no 4-byte piece of the secret is left */
        }
    }
    assert(proto_parser_next(&p, 1000, &f) == 1 && f.type == T_PING && f.len == 4 && memcmp(f.payload, pl, 4) == 0);
    proto_parser_release(&p, &f);
    assert(p.fill == 3 && memcmp(store, fr, 3) == 0);
    proto_parser_feed(&p, fr + 3, fl - 3);
    assert(proto_parser_next(&p, 1000, &f) == 1 && f.type == T_SET_STATE && f.len == 1);
    proto_parser_release(&p, &f);
    puts("ok");
    return 0;
}
