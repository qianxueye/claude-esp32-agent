#include "wsmap.h"

int wsmap_read(int n, int opcode, int payload_len)
{
    if (n < 0) {
        return WSR_FAIL;
    }
    switch (opcode) {
    case WS_OP_TEXT:
        return WSR_TEXT;
    case WS_OP_CLOSE:
        return WSR_EOS;
    case WS_OP_PING:
    case WS_OP_PONG:
        return WSR_WANT_READ;
    case WS_OP_BINARY:
    case WS_OP_CONT:
        if (n > 0) {
            return WSR_DATA;
        }
        /* 0 inside a non-empty payload: the transport has dropped its frame position */
        return payload_len == 0 ? WSR_WANT_READ : WSR_FAIL;
    case WS_OP_NONE:
        return n == 0 ? WSR_WANT_READ : WSR_FAIL;
    default:
        return WSR_FAIL;
    }
}

int wsmap_write(int poll, int sent, int len)
{
    if (poll == 0) {
        return WSW_WANT_WRITE;
    }
    if (poll < 0 || len <= 0) {
        return WSW_FAIL;
    }
    return sent == len ? WSW_SENT : WSW_FAIL;
}
