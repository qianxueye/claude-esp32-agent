#include "netplan.h"
#include <string.h>

int np_pick(const netcfg_t *cfg, const np_ap_t *aps, size_t n)
{
    int best_lan = -1, best_any = -1, rssi_lan = 0, rssi_any = 0;
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < cfg->n_nets; k++) {
            const netcfg_net_t *net = &cfg->nets[k];
            size_t len = strlen(net->ssid);
            if (len != aps[i].ssid_len || memcmp(net->ssid, aps[i].ssid, len) != 0) {
                continue;
            }
            int r = aps[i].rssi;
            if (best_any < 0 || r > rssi_any || (r == rssi_any && k < best_any)) {
                best_any = k;
                rssi_any = r;
            }
            if (net->lan && (best_lan < 0 || r > rssi_lan || (r == rssi_lan && k < best_lan))) {
                best_lan = k;
                rssi_lan = r;
            }
        }
    }
    return best_lan >= 0 ? best_lan : best_any;
}

size_t np_tries(const netcfg_t *cfg, int net, bool has_remote, np_try_t out[NP_TRIES_MAX])
{
    size_t n = 0;
    if (net < 0 || net >= cfg->n_nets) {
        return 0;
    }
    if (cfg->nets[net].lan) {
        for (uint8_t i = 0; i < cfg->n_names; i++) {
            out[n++] = (np_try_t){NP_TRY_NAME, i, LAN_CONNECT_TIMEOUT_MS};
        }
        for (uint8_t i = 0; i < cfg->n_addrs; i++) {
            out[n++] = (np_try_t){NP_TRY_ADDR, i, LAN_CONNECT_TIMEOUT_MS};
        }
    }
    if (has_remote) {
        out[n++] = (np_try_t){NP_TRY_REMOTE, 0, REMOTE_CONNECT_TIMEOUT_MS};
    }
    return n;
}

int np_fail_status(int try_kind, int stage)
{
    if (stage == NP_FAIL_AUTH) {
        return NET_RELAY_AUTH_FAILED;
    }
    if (try_kind == NP_TRY_REMOTE) {
        if (stage == NP_FAIL_NAME || stage == NP_FAIL_UPGRADE) {
            return NET_RELAY_CAPTIVE;
        }
        if (stage == NP_FAIL_CERT) {
            return NET_RELAY_TLS_FAILED;
        }
    }
    return NET_RELAY_UNREACHABLE;
}

bool np_status_differs(const net_status_t *a, const net_status_t *b)
{
    if (a->wifi != b->wifi || a->relay != b->relay) {
        return true;
    }
    return a->wifi == NET_WIFI_CONNECTED && memcmp(a->ip, b->ip, 4) != 0;
}

static uint32_t backoff_step(uint32_t *b)
{
    uint32_t d = *b;
    *b = d >= NET_BACKOFF_MAX_MS / 2 ? NET_BACKOFF_MAX_MS : d * 2;
    return d;
}

void np_reset(netplan_t *np, bool paired)
{
    memset(np, 0, sizeof *np);
    np->paired = paired;
    np->st.wifi = paired ? NET_WIFI_CONNECTING : NET_WIFI_OFF;
    np->st.relay = NET_RELAY_NONE;
    np->round_fail = NET_RELAY_NONE;
    np->relay_backoff_ms = NET_BACKOFF_MIN_MS;
    np->assoc_backoff_ms = NET_BACKOFF_MIN_MS;
}

static uint32_t wifi_failed(netplan_t *np)
{
    if (!np->paired) {
        return NET_BACKOFF_MAX_MS;
    }
    np->st.wifi = NET_WIFI_FAILED;
    np->st.relay = NET_RELAY_NONE;
    return backoff_step(&np->assoc_backoff_ms);
}

void np_assoc_start(netplan_t *np)
{
    if (np->paired && np->st.wifi != NET_WIFI_FAILED) {
        np->st.wifi = NET_WIFI_CONNECTING;
    }
}

uint32_t np_scan_none(netplan_t *np)
{
    return wifi_failed(np);
}

uint32_t np_assoc_failed(netplan_t *np)
{
    return wifi_failed(np);
}

void np_wifi_up(netplan_t *np, const uint8_t ip[4], int rssi, size_t n_tries)
{
    if (!np->paired) {
        return;
    }
    np->st.wifi = NET_WIFI_CONNECTED;
    memcpy(np->st.ip, ip, 4);
    np->st.rssi = rssi;
    if (!np->session) {
        np->st.relay = n_tries ? NET_RELAY_CONNECTING : NET_RELAY_NONE;
        np->round_fail = NET_RELAY_NONE;
    }
    np->assoc_backoff_ms = NET_BACKOFF_MIN_MS;
}

void np_wifi_rssi(netplan_t *np, int rssi)
{
    np->st.rssi = rssi;
}

void np_wifi_ip(netplan_t *np, const uint8_t ip[4], int rssi)
{
    if (np->st.wifi == NET_WIFI_CONNECTED) {
        memcpy(np->st.ip, ip, 4);
        np->st.rssi = rssi;
    }
}

void np_wifi_down(netplan_t *np)
{
    if (!np->paired) {
        return;
    }
    if (np->st.wifi == NET_WIFI_CONNECTED) {
        np->st.wifi = NET_WIFI_CONNECTING;
    }
    np->st.relay = NET_RELAY_NONE;
    np->round_fail = NET_RELAY_NONE;
    np->session = false;
    np->session_active = false;
}

void np_try_failed(netplan_t *np, int relay)
{
    np->round_fail = relay;
}

uint32_t np_round_failed(netplan_t *np)
{
    if (np->st.wifi == NET_WIFI_CONNECTED && np->round_fail != NET_RELAY_NONE) {
        np->st.relay = np->round_fail;
    }
    np->round_fail = NET_RELAY_NONE;
    return backoff_step(&np->relay_backoff_ms);
}

void np_session_up(netplan_t *np)
{
    np->session = true;
    np->session_active = false;
    np->round_fail = NET_RELAY_NONE;
    if (np->st.wifi == NET_WIFI_CONNECTED) {
        np->st.relay = NET_RELAY_CONNECTED;
    }
}

void np_session_active(netplan_t *np)
{
    np->session_active = true;
    np->relay_backoff_ms = NET_BACKOFF_MIN_MS;
}

uint32_t np_session_down(netplan_t *np)
{
    bool active = np->session_active;
    np->session = false;
    np->session_active = false;
    if (np->st.wifi == NET_WIFI_CONNECTED) {
        np->st.relay = NET_RELAY_CONNECTING;
    }
    return active ? np->relay_backoff_ms : backoff_step(&np->relay_backoff_ms);
}

void np_remote_changed(netplan_t *np, size_t n_tries)
{
    np->relay_backoff_ms = NET_BACKOFF_MIN_MS;
    if (np->st.wifi == NET_WIFI_CONNECTED && !np->session) {
        np->st.relay = n_tries ? NET_RELAY_CONNECTING : NET_RELAY_NONE;
    }
}

void np_usb_lost(netplan_t *np)
{
    np->relay_backoff_ms = NET_BACKOFF_MIN_MS;
}
