#include <stdio.h>
#include <string.h>
#include "linkstate.h"
#include "proto.h"

static int s_fail, s_checks;
#define CHECK(name, cond) check((name), (cond), __LINE__)

static void check(const char *name, int ok, int line)
{
    s_checks++;
    if (!ok) {
        s_fail++;
        printf("FAIL linkstate_test.c:%d %s\n", line, name);
    }
}

static link_verdict_t usb(linkstate_t *s, uint8_t type, uint32_t t)
{
    return link_on_frame(s, LINK_SRC_USB, LINK_NONE, type, t);
}

static link_verdict_t wifi(linkstate_t *s, uint8_t type, uint32_t t)
{
    return link_on_frame(s, LINK_SRC_NET, LINK_WIFI, type, t);
}

static void test_usb_silence_window(void)
{
    linkstate_t s;
    link_init(&s);
    usb(&s, T_PING, 10000);
    link_verdict_t v = wifi(&s, T_HOST_HELLO, 14999);
    CHECK("NET HOST_HELLO 4999 ms after a USB frame is ignored", !v.handle && !v.live && !v.activated && s.active == LINK_NONE);
    v = wifi(&s, T_HOST_HELLO, 15000);
    CHECK("NET HOST_HELLO 5000 ms after a USB frame becomes active",
          v.handle && v.live && v.activated && s.active == LINK_WIFI && link_route(&s) == ROUTE_WIFI);

    link_init(&s);
    v = wifi(&s, T_HOST_HELLO, 100);
    CHECK("never-heard USB counts as silent at 100 ms uptime", v.handle && v.activated && s.active == LINK_WIFI);

    link_init(&s);
    usb(&s, T_PING, 0xFFFFF000u);
    v = wifi(&s, T_HOST_HELLO, 0xFFFFF000u + 4999);
    CHECK("4999 ms across the clock wrap is still recent", !v.handle && s.active == LINK_NONE);
    v = wifi(&s, T_HOST_HELLO, 0xFFFFF000u + 5000);
    CHECK("5000 ms across the clock wrap is silent", v.activated && s.active == LINK_WIFI);
}

static void test_usb_precedence(void)
{
    linkstate_t s;
    link_init(&s);
    wifi(&s, T_HOST_HELLO, 100);
    link_verdict_t v = wifi(&s, T_HOST_HELLO, 200);
    CHECK("second HOST_HELLO on the active session does not count as activation", v.handle && !v.activated);
    v = usb(&s, T_HOST_HELLO, 300);
    CHECK("USB HOST_HELLO while NET is active switches to USB", v.handle && v.live && s.active == LINK_USB && link_route(&s) == ROUTE_USB);

    link_init(&s);
    wifi(&s, T_HOST_HELLO, 100);
    v = usb(&s, T_PAIR, 200);
    CHECK("PAIR on USB handled while NET is active", v.handle && !v.live && s.active == LINK_WIFI);
    v = usb(&s, T_FORGET, 300);
    CHECK("FORGET on USB handled while NET is active", v.handle && !v.live && s.active == LINK_WIFI);
    v = usb(&s, T_PING, 400);
    CHECK("USB PING while NET is active is ignored and not live", !v.handle && !v.live && s.active == LINK_WIFI);
    v = wifi(&s, T_HOST_HELLO, 500);
    CHECK("NET HOST_HELLO on the active session ignored while USB was heard", !v.handle && !v.live && s.active == LINK_WIFI);
    v = wifi(&s, T_PAIR, 600);
    CHECK("PAIR on NET ignored while NET is active", !v.handle && !v.live);
    v = wifi(&s, T_FORGET, 700);
    CHECK("FORGET on NET ignored while NET is active", !v.handle && !v.live);
    v = wifi(&s, T_PING, 800);
    CHECK("NET PING while NET is active is handled and live", v.handle && v.live);

    link_init(&s);
    usb(&s, T_HOST_HELLO, 100);
    v = wifi(&s, T_PING, 200);
    CHECK("NET PING while USB is active is ignored and not live", !v.handle && !v.live && s.active == LINK_USB);
    v = usb(&s, T_PING, 300);
    CHECK("USB PING while USB is active is handled and live", v.handle && v.live);
}

static void test_none_and_end(void)
{
    linkstate_t s;
    link_init(&s);
    CHECK("route while NONE is USB", s.active == LINK_NONE && link_route(&s) == ROUTE_USB);
    link_verdict_t v = usb(&s, T_SET_STATE, 100);
    CHECK("USB frame while NONE is handled and live", v.handle && v.live && s.active == LINK_NONE);
    v = link_on_frame(&s, LINK_SRC_NET, LINK_TUNNEL, T_PING, 200);
    CHECK("NET PING while NONE is ignored", !v.handle && !v.live);

    link_init(&s);
    link_on_frame(&s, LINK_SRC_NET, LINK_TUNNEL, T_HOST_HELLO, 100);
    CHECK("route while a tunnel session is active is TUNNEL", link_route(&s) == ROUTE_TUNNEL);
    link_net_ended(&s);
    CHECK("NET session end while NET is active gives NONE and USB routing", s.active == LINK_NONE && link_route(&s) == ROUTE_USB);

    link_init(&s);
    usb(&s, T_HOST_HELLO, 100);
    link_net_ended(&s);
    CHECK("NET session end while USB is active keeps USB", s.active == LINK_USB && link_route(&s) == ROUTE_USB);
    CHECK("route while USB is active is USB", link_route(&s) == ROUTE_USB);
    CHECK("HELLO link names", strcmp(link_name(ROUTE_USB), "usb") == 0 && strcmp(link_name(ROUTE_WIFI), "wifi") == 0
                                  && strcmp(link_name(ROUTE_TUNNEL), "tunnel") == 0);
}

static void test_generation(void)
{
    linkstate_t s;
    link_init(&s);
    link_verdict_t v = link_on_net_frame(&s, LINK_TUNNEL, 4, 5, T_HOST_HELLO, 100);
    CHECK("HOST_HELLO from a session of generation 4 after PAIR made it 5 does not activate",
          !v.handle && !v.activated && s.active == LINK_NONE);
    v = link_on_net_frame(&s, LINK_TUNNEL, 5, 5, T_HOST_HELLO, 100);
    CHECK("HOST_HELLO from a current session activates", v.handle && v.activated && s.active == LINK_TUNNEL);
    v = link_on_net_frame(&s, LINK_TUNNEL, 5, 6, T_SET_REMOTE, 200);
    CHECK("SET_REMOTE read before a USB PAIR changed the generation is dropped", !v.handle && !v.live);
    v = link_on_net_frame(&s, LINK_TUNNEL, 6, 6, T_SET_REMOTE, 200);
    CHECK("SET_REMOTE of the current generation on the active session is handled", v.handle && v.live);
}

int main(void)
{
    test_generation();
    test_usb_silence_window();
    test_usb_precedence();
    test_none_and_end();
    printf("linkstate: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
