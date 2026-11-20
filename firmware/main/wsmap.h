#pragma once

/* Maps WebSocket transport results to inner-TLS BIO results for the tunnel (docs/network.md "Tunnel BIO"). */

/* Opcodes as esp_transport_ws_get_read_opcode reports them; NONE when no header was read by the call. */
enum { WS_OP_CONT = 0x0, WS_OP_TEXT = 0x1, WS_OP_BINARY = 0x2, WS_OP_CLOSE = 0x8, WS_OP_PING = 0x9, WS_OP_PONG = 0xA, WS_OP_NONE = 0x100 };

enum { WSR_DATA, WSR_WANT_READ, WSR_EOS, WSR_TEXT, WSR_FAIL };
enum { WSW_SENT, WSW_WANT_WRITE, WSW_FAIL };

/* Judges one esp_transport_read result n with the opcode and payload length read after it.
 * WSR_DATA means n bytes of inner TLS; WSR_TEXT, WSR_EOS and WSR_FAIL end the session. */
int wsmap_read(int n, int opcode, int payload_len);
/* Judges one write: poll is the BIO's own esp_transport_poll_write, sent the send_raw result for len > 0 bytes.
 * WANT_WRITE only when the poll timed out, so no byte of the message has left. */
int wsmap_write(int poll, int sent, int len);
