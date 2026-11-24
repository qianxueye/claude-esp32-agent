#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "proto.h"

/* live: the frame came on the active link (or USB while none is) and counts for host liveness. */
typedef void (*frame_handler_t)(const proto_frame_t *f, bool live);
/* Refuses a frame before the active-link rules see it. */
typedef bool (*frame_filter_t)(const proto_frame_t *f);

typedef struct {
    uint32_t tx_frames;
    uint32_t tx_dropped;
    uint32_t rx_frames;
    uint32_t rx_crc_errors;
    uint32_t rx_bytes_skipped;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
} transport_stats_t;

void transport_start(frame_handler_t on_frame, frame_filter_t accept);
/* Queues one frame on the active link; drops it when the queue is full or no host is attached. */
bool transport_send(uint8_t type, const void *payload, size_t len);
/* Queues one frame on a route read earlier with transport_route. */
bool transport_send_to(int route, uint8_t type, const void *payload, size_t len);
bool transport_send_usb(uint8_t type, const void *payload, size_t len);
/* Queues one frame that must reach the host; waits for queue room and USB room. */
bool transport_send_reliable(uint8_t type, const void *payload, size_t len);
/* Dispatches one frame from the open network session; kind is LINK_WIFI or LINK_TUNNEL, gen its configuration generation. */
void transport_net_frame(int kind, uint32_t gen, const proto_frame_t *f);
/* The network session ended; call before the next session can start. */
void transport_net_ended(void);
/* The ROUTE_ value for device-to-host frames, readable without the dispatch lock. */
int transport_route(void);
/* The LINK_ value of the active link. */
int transport_link_active(void);
uint32_t transport_rx_stack_hwm(void);
void transport_get_stats(transport_stats_t *out);
uint32_t now_ms(void);
