#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Which link is active and which received frames count (docs/protocol.md "Active link"). */

enum { LINK_SRC_USB, LINK_SRC_NET };
enum { LINK_NONE, LINK_USB, LINK_WIFI, LINK_TUNNEL };
/* Where device-to-host frames go; one scalar so it can be read without the dispatch lock. */
enum { ROUTE_USB, ROUTE_WIFI, ROUTE_TUNNEL };

typedef struct {
    int active;
    bool usb_heard; /* a valid USB frame has arrived since boot */
    uint32_t usb_last_ms;
} linkstate_t;

typedef struct {
    bool handle; /* run the frame's handler */
    bool live; /* the frame counts for host liveness */
    bool activated; /* this HOST_HELLO made its network session the active link */
} link_verdict_t;

void link_init(linkstate_t *s);
/* Judges one valid frame; net_kind is LINK_WIFI or LINK_TUNNEL for the session that sent a network frame. */
link_verdict_t link_on_frame(linkstate_t *s, int src, int net_kind, uint8_t type, uint32_t now_ms);
/* The network session ended; an active network link becomes none. */
void link_net_ended(linkstate_t *s);
int link_route(const linkstate_t *s);
/* The HELLO "link" value for a route. */
const char *link_name(int route);
