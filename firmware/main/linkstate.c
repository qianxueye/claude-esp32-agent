#include "linkstate.h"
#include "app_config.h"
#include "proto.h"

void link_init(linkstate_t *s)
{
    s->active = LINK_NONE;
    s->usb_heard = false;
    s->usb_last_ms = 0;
}

static bool usb_silent(const linkstate_t *s, uint32_t now_ms)
{
    return !s->usb_heard || (uint32_t)(now_ms - s->usb_last_ms) >= PROTO_HOST_SILENCE_TIMEOUT_MS;
}

link_verdict_t link_on_frame(linkstate_t *s, int src, int net_kind, uint8_t type, uint32_t now_ms)
{
    link_verdict_t v = {false, false, false};
    bool pairing = type == T_PAIR || type == T_FORGET;
    if (src == LINK_SRC_USB) {
        s->usb_heard = true;
        s->usb_last_ms = now_ms;
        if (type == T_HOST_HELLO) {
            s->active = LINK_USB;
        }
        bool on_active = s->active == LINK_NONE || s->active == LINK_USB;
        v.handle = on_active || pairing;
        v.live = on_active;
        return v;
    }
    if (pairing) {
        return v;
    }
    if (type == T_HOST_HELLO) {
        if (!usb_silent(s, now_ms)) {
            return v;
        }
        v.activated = s->active != net_kind;
        s->active = net_kind;
    }
    if (s->active == net_kind) {
        v.handle = true;
        v.live = true;
    }
    return v;
}

link_verdict_t link_on_net_frame(linkstate_t *s, int net_kind, uint32_t session_gen, uint32_t gen, uint8_t type, uint32_t now_ms)
{
    if (session_gen != gen) {
        link_verdict_t none = {false, false, false};
        return none;
    }
    return link_on_frame(s, LINK_SRC_NET, net_kind, type, now_ms);
}

void link_net_ended(linkstate_t *s)
{
    if (s->active == LINK_WIFI || s->active == LINK_TUNNEL) {
        s->active = LINK_NONE;
    }
}

int link_route(const linkstate_t *s)
{
    return s->active == LINK_WIFI ? ROUTE_WIFI : s->active == LINK_TUNNEL ? ROUTE_TUNNEL : ROUTE_USB;
}

const char *link_name(int route)
{
    return route == ROUTE_WIFI ? "wifi" : route == ROUTE_TUNNEL ? "tunnel" : "usb";
}
