#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "proto.h"

typedef void (*frame_handler_t)(const proto_frame_t *f);

typedef struct {
    uint32_t tx_frames;
    uint32_t tx_dropped;
    uint32_t rx_frames;
    uint32_t rx_crc_errors;
    uint32_t rx_bytes_skipped;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
} transport_stats_t;

void transport_start(frame_handler_t on_frame);
/* Queues one frame; drops it when the queue is full or no host is attached. */
bool transport_send(uint8_t type, const void *payload, size_t len);
/* Queues one frame that must reach the host; waits for queue room and USB room. */
bool transport_send_reliable(uint8_t type, const void *payload, size_t len);
void transport_get_stats(transport_stats_t *out);
uint32_t now_ms(void);
