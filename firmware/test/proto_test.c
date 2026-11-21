#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "proto.h"

/* True when any 4-byte piece of the secret values in the PAIR test text is still in buf. */
static int secret_left(const uint8_t *buf, size_t len)
{
    static const char *const values[] = {"SECRET-PSK-TEXT", "PASSWORD-TEXT"};
    for (int v = 0; v < 2; v++) {
        for (size_t k = 0; k + 4 <= strlen(values[v]); k++) {
            for (size_t i = 0; i + 4 <= len; i++) {
                if (memcmp(buf + i, values[v] + k, 4) == 0) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

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
    /* no byte the parser discards or releases stays in its buffer, whatever the path */
    static const char secret[] = "{\"psk\":\"SECRET-PSK-TEXT\",\"password\":\"PASSWORD-TEXT\"}";
    uint8_t pair[128];
    size_t pairlen = proto_encode(pair, T_PAIR, secret, sizeof secret - 1);
    /* 1: bad CRC, then a valid frame in the same read */
    proto_parser_init(&p, store, sizeof store);
    n = 0;
    memcpy(stream + n, "zz", 2); n += 2;
    memcpy(stream + n, pair, pairlen); stream[n + pairlen - 1] ^= 0xFF; n += pairlen;
    n += proto_encode(stream + n, T_PING, pl, 4);
    assert(proto_parser_feed(&p, stream, n) == n);
    assert(proto_parser_next(&p, 1000, &f) == 1 && f.type == T_PING && f.len == 4 && memcmp(f.payload, pl, 4) == 0);
    assert(p.crc_errors == 1);
    proto_parser_release(&p, &f);
    assert(p.fill == 0 && !secret_left(store, sizeof store));
    /* 2: a valid PAIR released, a partial frame still pending */
    n = 0;
    memcpy(stream + n, pair, pairlen); n += pairlen;
    fl = proto_encode(fr, T_SET_STATE, pl, 1);
    memcpy(stream + n, fr, 3); n += 3;
    proto_parser_feed(&p, stream, n);
    assert(proto_parser_next(&p, 1000, &f) == 1 && f.type == T_PAIR);
    proto_parser_release(&p, &f);
    assert(!secret_left(store, sizeof store) && p.fill == 3 && memcmp(store, fr, 3) == 0);
    proto_parser_feed(&p, fr + 3, fl - 3);
    assert(proto_parser_next(&p, 1000, &f) == 1 && f.type == T_SET_STATE);
    proto_parser_release(&p, &f);
    /* 3: a partial PAIR that times out on USB */
    proto_parser_feed(&p, pair, pairlen - 10);
    assert(!proto_parser_next(&p, 2000, &f));
    assert(!proto_parser_next(&p, 2300, &f));
    assert(p.fill == 0 && !secret_left(store, sizeof store));
    /* 4: a partial PAIR when the parser is reset */
    proto_parser_feed(&p, pair, pairlen - 10);
    proto_parser_init(&p, store, sizeof store);
    assert(!secret_left(store, sizeof store));
    proto_parser_feed(&p, fr, fl);
    assert(proto_parser_next(&p, 3000, &f) == 1 && f.type == T_SET_STATE);
    puts("ok");
    return 0;
}
