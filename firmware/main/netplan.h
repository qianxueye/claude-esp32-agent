#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "netcfg.h"

/* Network pick, relay try order, retry backoff and the WIFI_STATUS state (docs/network.md). */

typedef struct {
    uint8_t ssid[NET_SSID_MAX];
    uint8_t ssid_len;
    int rssi;
} np_ap_t;

enum { NP_TRY_NAME, NP_TRY_ADDR, NP_TRY_REMOTE };

typedef struct {
    uint8_t kind;
    uint8_t index; /* into the relay names or addresses */
    uint32_t timeout_ms;
} np_try_t;

#define NP_TRIES_MAX (NET_RELAY_NAMES_MAX + NET_RELAY_ADDRS_MAX + 1)

/* Where an attempt failed. */
enum { NP_FAIL_NAME, NP_FAIL_TCP, NP_FAIL_CERT, NP_FAIL_OUTER, NP_FAIL_UPGRADE, NP_FAIL_AUTH, NP_FAIL_INNER };

typedef struct {
    net_status_t st;
    bool paired;
    bool session; /* an authenticated relay session is open */
    bool session_active; /* its HOST_HELLO made it the active link */
    int round_fail; /* last failure of the current round, NET_RELAY_NONE when none */
    uint32_t relay_backoff_ms;
    uint32_t assoc_backoff_ms;
} netplan_t;

/* Adds one scan record to kept if its SSID is saved, keeping the strongest record per SSID; returns the new count. */
size_t np_scan_keep(const netcfg_t *cfg, np_ap_t kept[WIFI_NETWORKS_MAX], size_t n_kept, const np_ap_t *ap);
/* Picks the saved network to join from a scan; returns its index or -1. */
int np_pick(const netcfg_t *cfg, const np_ap_t *aps, size_t n);
/* Fills the relay attempts for saved network net; returns how many. */
size_t np_tries(const netcfg_t *cfg, int net, bool has_remote, np_try_t out[NP_TRIES_MAX]);
/* The WIFI_STATUS relay value for a failed attempt. */
int np_fail_status(int try_kind, int stage);
/* True when WIFI_STATUS must be sent again: wifi, ip or relay changed. */
bool np_status_differs(const net_status_t *a, const net_status_t *b);

/* Boot, PAIR and FORGET: status and both backoffs start over. */
void np_reset(netplan_t *np, bool paired);
/* A scan or an association attempt starts. */
void np_assoc_start(netplan_t *np);
/* A scan found no saved network; returns the wait before the next scan. */
uint32_t np_scan_none(netplan_t *np);
/* An association attempt failed; returns the wait before the next scan. */
uint32_t np_assoc_failed(netplan_t *np);
void np_wifi_up(netplan_t *np, const uint8_t ip[4], int rssi, size_t n_tries);
void np_wifi_rssi(netplan_t *np, int rssi);
/* A new DHCP address while associated; the relay state and any session are kept. */
void np_wifi_ip(netplan_t *np, const uint8_t ip[4], int rssi);
void np_wifi_down(netplan_t *np);
/* One attempt of the round failed with a relay value from np_fail_status. */
void np_try_failed(netplan_t *np, int relay);
/* Every attempt of the round failed; returns the wait before the next round. */
uint32_t np_round_failed(netplan_t *np);
/* The inner TLS session authenticated. */
void np_session_up(netplan_t *np);
/* The relay's HOST_HELLO made the session the active link. */
void np_session_active(netplan_t *np);
/* The authenticated session ended; returns the wait before the next round. */
uint32_t np_session_down(netplan_t *np);
/* SET_REMOTE changed the remote URL; n_tries is the new attempt count on the joined network. */
void np_remote_changed(netplan_t *np, size_t n_tries);
/* USB stopped being connected while it was the active link. */
void np_usb_lost(netplan_t *np);
