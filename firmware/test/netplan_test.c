#include <stdio.h>
#include <string.h>
#include "linkstate.h"
#include "netplan.h"
#include "proto.h"

static int s_fail, s_checks;
#define CHECK(name, cond) check((name), (cond), __LINE__)

static void check(const char *name, int ok, int line)
{
    s_checks++;
    if (!ok) {
        s_fail++;
        printf("FAIL netplan_test.c:%d %s\n", line, name);
    }
}

static const uint8_t IP[4] = {10, 0, 0, 7};

static np_ap_t ap(const char *ssid, int rssi)
{
    np_ap_t a = {.ssid_len = (uint8_t)strlen(ssid), .rssi = rssi};
    memcpy(a.ssid, ssid, a.ssid_len);
    return a;
}

static void cfg_two(netcfg_t *c)
{
    memset(c, 0, sizeof *c);
    strcpy(c->nets[0].ssid, "HomeNet");
    c->nets[0].lan = true;
    strcpy(c->nets[1].ssid, "Phone");
    strcpy(c->nets[2].ssid, "Office");
    c->n_nets = 3;
    strcpy(c->names[0], "relay.local");
    strcpy(c->names[1], "relay-2.local");
    c->n_names = 2;
    c->n_addrs = 2;
}

static void test_pick(void)
{
    netcfg_t c;
    cfg_two(&c);
    np_ap_t scan[] = {ap("Phone", -40), ap("Stranger", -30), ap("HomeNet", -80)};
    CHECK("lan network at -80 preferred over a stronger non-lan one at -40", np_pick(&c, scan, 3) == 0);
    np_ap_t two[] = {ap("Phone", -70), ap("Office", -50), ap("Stranger", -20)};
    CHECK("strongest of two non-lan networks: Office -50 over Phone -70", np_pick(&c, two, 3) == 2);
    np_ap_t dup[] = {ap("Phone", -70), ap("Office", -60), ap("Phone", -55)};
    CHECK("second access point of Phone at -55 beats Office at -60", np_pick(&c, dup, 3) == 1);
    np_ap_t none[] = {ap("Stranger", -20), ap("HomeNe", -30), ap("HomeNetX", -30)};
    CHECK("no saved network visible gives -1", np_pick(&c, none, 3) == -1);
    CHECK("empty scan gives -1", np_pick(&c, NULL, 0) == -1);
}

static void test_tries(void)
{
    netcfg_t c;
    cfg_two(&c);
    np_try_t t[NP_TRIES_MAX];
    size_t n = np_tries(&c, 0, true, t);
    CHECK("lan network: 2 names, 2 addresses, then the remote URL", n == 5);
    CHECK("lan order: names in order",
          t[0].kind == NP_TRY_NAME && t[0].index == 0 && t[1].kind == NP_TRY_NAME && t[1].index == 1);
    CHECK("lan order: then addresses in order",
          t[2].kind == NP_TRY_ADDR && t[2].index == 0 && t[3].kind == NP_TRY_ADDR && t[3].index == 1);
    CHECK("lan order: remote URL last", t[4].kind == NP_TRY_REMOTE);
    CHECK("LAN attempts use 1500 ms, the remote 15000 ms",
          t[0].timeout_ms == 1500 && t[3].timeout_ms == 1500 && t[4].timeout_ms == 15000);
    CHECK("lan network without a remote URL: names and addresses only", np_tries(&c, 0, false, t) == 4 && t[3].kind == NP_TRY_ADDR);
    n = np_tries(&c, 1, true, t);
    CHECK("non-lan network: the remote URL only", n == 1 && t[0].kind == NP_TRY_REMOTE);
    CHECK("non-lan network without a remote URL: empty", np_tries(&c, 1, false, t) == 0);
    c.n_names = NET_RELAY_NAMES_MAX;
    c.n_addrs = NET_RELAY_ADDRS_MAX;
    CHECK("full lists fit NP_TRIES_MAX", np_tries(&c, 0, true, t) == NP_TRIES_MAX);
}

static void test_fail_status(void)
{
    CHECK("LAN name failure is unreachable", np_fail_status(NP_TRY_NAME, NP_FAIL_NAME) == NET_RELAY_UNREACHABLE);
    CHECK("LAN TCP failure is unreachable", np_fail_status(NP_TRY_ADDR, NP_FAIL_TCP) == NET_RELAY_UNREACHABLE);
    CHECK("LAN fatal alert is auth_failed", np_fail_status(NP_TRY_ADDR, NP_FAIL_AUTH) == NET_RELAY_AUTH_FAILED);
    CHECK("LAN handshake timeout is unreachable", np_fail_status(NP_TRY_NAME, NP_FAIL_INNER) == NET_RELAY_UNREACHABLE);
    CHECK("remote name failure is captive", np_fail_status(NP_TRY_REMOTE, NP_FAIL_NAME) == NET_RELAY_CAPTIVE);
    CHECK("remote non-101 answer is captive", np_fail_status(NP_TRY_REMOTE, NP_FAIL_UPGRADE) == NET_RELAY_CAPTIVE);
    CHECK("remote certificate failure is tls_failed", np_fail_status(NP_TRY_REMOTE, NP_FAIL_CERT) == NET_RELAY_TLS_FAILED);
    CHECK("remote TCP failure is unreachable", np_fail_status(NP_TRY_REMOTE, NP_FAIL_TCP) == NET_RELAY_UNREACHABLE);
    CHECK("remote other outer failure is unreachable", np_fail_status(NP_TRY_REMOTE, NP_FAIL_OUTER) == NET_RELAY_UNREACHABLE);
    CHECK("remote inner fatal alert is auth_failed", np_fail_status(NP_TRY_REMOTE, NP_FAIL_AUTH) == NET_RELAY_AUTH_FAILED);
}

static int ladder_ok(uint32_t (*fail)(netplan_t *), netplan_t *np)
{
    static const uint32_t want[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        uint32_t got = fail(np);
        if (got != want[i]) {
            printf("  step %zu: got %u want %u\n", i, (unsigned)got, (unsigned)want[i]);
            return 0;
        }
    }
    return 1;
}

static netplan_t joined(void)
{
    netplan_t np;
    np_reset(&np, true);
    np_wifi_up(&np, IP, -50, 3);
    return np;
}

static void test_backoff(void)
{
    netplan_t np = joined();
    CHECK("relay backoff 1, 2, 4, 8, 16, 30, 30 s", ladder_ok(np_round_failed, &np));
    np_session_up(&np);
    np_session_active(&np);
    CHECK("session whose HOST_HELLO made it active resets the backoff and waits 1 s", np_session_down(&np) == 1000);
    CHECK("next failed round after an active session waits 1 s", np_round_failed(&np) == 1000);
    CHECK("then 2 s", np_round_failed(&np) == 2000);

    np = joined();
    np_round_failed(&np);
    np_round_failed(&np);
    np_session_up(&np);
    CHECK("session ended without HOST_HELLO counts as failed: waits 4 s", np_session_down(&np) == 4000);
    CHECK("and the next round waits 8 s", np_round_failed(&np) == 8000);

    np = joined();
    np_round_failed(&np);
    np_round_failed(&np);
    np_reset(&np, true);
    np_wifi_up(&np, IP, -50, 3);
    CHECK("config change resets the relay backoff to 1 s", np_round_failed(&np) == 1000);

    np = joined();
    np_round_failed(&np);
    np_round_failed(&np);
    np_usb_lost(&np);
    CHECK("USB disconnect while USB is active resets the relay backoff to 1 s", np_round_failed(&np) == 1000);

    np = joined();
    np_round_failed(&np);
    np_round_failed(&np);
    np_remote_changed(&np, 3);
    CHECK("changed SET_REMOTE resets the relay backoff to 1 s", np_round_failed(&np) == 1000);

    np_reset(&np, true);
    CHECK("association backoff 1, 2, 4, 8, 16, 30, 30 s", ladder_ok(np_assoc_failed, &np));
    np_wifi_up(&np, IP, -50, 3);
    CHECK("association success resets the association backoff", np_assoc_failed(&np) == 1000);
    np_reset(&np, true);
    CHECK("scan with no saved network uses the association ladder", ladder_ok(np_scan_none, &np));
}

static void test_ignored_hello_no_reset(void)
{
    linkstate_t ls;
    link_init(&ls);
    netplan_t np = joined();
    np_round_failed(&np);
    np_round_failed(&np);
    np_session_up(&np);
    link_on_frame(&ls, LINK_SRC_USB, LINK_NONE, T_HOST_HELLO, 10000);
    link_verdict_t v = link_on_frame(&ls, LINK_SRC_NET, LINK_WIFI, T_HOST_HELLO, 11000);
    if (v.activated) {
        np_session_active(&np);
    }
    CHECK("HOST_HELLO ignored while USB is recent does not reset the backoff", !v.activated && np_session_down(&np) == 4000);
    np_session_up(&np);
    v = link_on_frame(&ls, LINK_SRC_NET, LINK_WIFI, T_HOST_HELLO, 15000);
    if (v.activated) {
        np_session_active(&np);
    }
    CHECK("HOST_HELLO that made the session active resets the backoff", v.activated && np_session_down(&np) == 1000);
}

static void test_status(void)
{
    netplan_t np;
    np_reset(&np, false);
    CHECK("unpaired: wifi off, relay none", np.st.wifi == NET_WIFI_OFF && np.st.relay == NET_RELAY_NONE);
    np_assoc_start(&np);
    np_assoc_failed(&np);
    np_wifi_up(&np, IP, -50, 3);
    CHECK("unpaired: events leave off and none", np.st.wifi == NET_WIFI_OFF && np.st.relay == NET_RELAY_NONE);

    np_reset(&np, true);
    CHECK("paired, scanning: wifi connecting, relay none", np.st.wifi == NET_WIFI_CONNECTING && np.st.relay == NET_RELAY_NONE);
    np_assoc_start(&np);
    np_scan_none(&np);
    CHECK("scan without a saved network: wifi failed", np.st.wifi == NET_WIFI_FAILED);
    np_assoc_start(&np);
    CHECK("wifi failed stays through the next scan", np.st.wifi == NET_WIFI_FAILED);
    np_assoc_failed(&np);
    CHECK("failed association: wifi failed", np.st.wifi == NET_WIFI_FAILED);
    np_wifi_down(&np);
    CHECK("wifi failed stays through a disconnect event", np.st.wifi == NET_WIFI_FAILED);
    np_wifi_up(&np, IP, -50, 3);
    CHECK("association with an IP: wifi connected, relay connecting in the first round",
          np.st.wifi == NET_WIFI_CONNECTED && np.st.relay == NET_RELAY_CONNECTING && np.st.ip[3] == 7 && np.st.rssi == -50);
    np_try_failed(&np, NET_RELAY_UNREACHABLE);
    CHECK("relay connecting during the first round", np.st.relay == NET_RELAY_CONNECTING);
    np_try_failed(&np, NET_RELAY_CAPTIVE);
    np_round_failed(&np);
    CHECK("relay shows the last failure of the round", np.st.relay == NET_RELAY_CAPTIVE);
    np_try_failed(&np, NET_RELAY_UNREACHABLE);
    CHECK("failure value stays during the next round", np.st.relay == NET_RELAY_CAPTIVE);
    np_round_failed(&np);
    CHECK("next round's last failure replaces it", np.st.relay == NET_RELAY_UNREACHABLE);
    np_session_up(&np);
    CHECK("authenticated session: relay connected", np.st.relay == NET_RELAY_CONNECTED);
    np_session_down(&np);
    CHECK("session end after authentication: relay connecting", np.st.relay == NET_RELAY_CONNECTING);
    np_try_failed(&np, NET_RELAY_UNREACHABLE);
    np_round_failed(&np);
    np_wifi_down(&np);
    CHECK("Wi-Fi disconnect: wifi connecting, relay none", np.st.wifi == NET_WIFI_CONNECTING && np.st.relay == NET_RELAY_NONE);
    np_wifi_up(&np, IP, -50, 3);
    CHECK("re-association clears the old failure: relay connecting", np.st.relay == NET_RELAY_CONNECTING);
    np_try_failed(&np, NET_RELAY_AUTH_FAILED);
    np_round_failed(&np);
    np_reset(&np, true);
    CHECK("config change clears failures", np.st.wifi == NET_WIFI_CONNECTING && np.st.relay == NET_RELAY_NONE);
    np_wifi_up(&np, IP, -50, 0);
    CHECK("joined with an empty attempt list: relay none", np.st.relay == NET_RELAY_NONE);
    np_remote_changed(&np, 1);
    CHECK("SET_REMOTE gives the empty list a URL: relay connecting", np.st.relay == NET_RELAY_CONNECTING);
    np_try_failed(&np, NET_RELAY_TLS_FAILED);
    np_round_failed(&np);
    np_remote_changed(&np, 1);
    CHECK("changed SET_REMOTE clears a failure: relay connecting", np.st.relay == NET_RELAY_CONNECTING);
    np_session_up(&np);
    np_remote_changed(&np, 1);
    CHECK("changed SET_REMOTE keeps an open session connected", np.st.relay == NET_RELAY_CONNECTED);
    const uint8_t ip2[4] = {10, 0, 0, 9};
    np_wifi_up(&np, ip2, -60, 1);
    CHECK("new IP during an open session keeps relay connected", np.st.relay == NET_RELAY_CONNECTED && np.st.ip[3] == 9);
}

static void test_refused_after_handshake(void)
{
    netplan_t np = joined();
    uint32_t waits[4];
    int seq_ok = 1;
    for (int i = 0; i < 4; i++) {
        np_session_up(&np);
        seq_ok &= np.st.relay == NET_RELAY_CONNECTED;
        waits[i] = np_session_down(&np);
        seq_ok &= np.st.relay == NET_RELAY_CONNECTING;
    }
    CHECK("refused right after the handshake: connected, then connecting each time", seq_ok);
    CHECK("refused right after the handshake: waits grow 1, 2, 4, 8 s",
          waits[0] == 1000 && waits[1] == 2000 && waits[2] == 4000 && waits[3] == 8000);
}

static void test_status_differs(void)
{
    net_status_t a = {.wifi = NET_WIFI_CONNECTED, .relay = NET_RELAY_CONNECTED, .ip = {10, 0, 0, 7}, .rssi = -50};
    net_status_t b = a;
    b.rssi = -70;
    CHECK("rssi change alone sends no WIFI_STATUS", !np_status_differs(&a, &b));
    b.ip[3] = 8;
    CHECK("ip change sends WIFI_STATUS", np_status_differs(&a, &b));
    b = a;
    b.relay = NET_RELAY_CONNECTING;
    CHECK("relay change sends WIFI_STATUS", np_status_differs(&a, &b));
    b = a;
    b.wifi = NET_WIFI_CONNECTING;
    CHECK("wifi change sends WIFI_STATUS", np_status_differs(&a, &b));
}

int main(void)
{
    test_pick();
    test_tries();
    test_fail_status();
    test_backoff();
    test_ignored_hello_no_reset();
    test_status();
    test_refused_after_handshake();
    test_status_differs();
    printf("netplan: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
