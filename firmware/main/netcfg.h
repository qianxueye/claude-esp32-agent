#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_config.h"

/* PAIR and SET_REMOTE parsing and validation, and the WIFI_STATUS text (docs/protocol.md, docs/network.md). */

#define NET_SSID_MAX 32
#define NET_PASS_MAX 64
#define NET_PSK_BYTES 32
#define NET_IDENTITY_LEN 18 /* "clawd-" and 12 hex digits */

typedef struct {
    char ssid[NET_SSID_MAX + 1];
    char password[NET_PASS_MAX + 1];
    bool lan;
} netcfg_net_t;

typedef struct {
    netcfg_net_t nets[WIFI_NETWORKS_MAX];
    uint8_t n_nets;
    char identity[NET_IDENTITY_LEN + 1];
    uint8_t psk[NET_PSK_BYTES];
    char names[NET_RELAY_NAMES_MAX][NET_RELAY_NAME_MAX + 1];
    uint8_t n_names;
    uint8_t addrs[NET_RELAY_ADDRS_MAX][4];
    uint8_t n_addrs;
    uint16_t port;
    bool has_remote;
    char remote_url[NET_URL_MAX + 1];
} netcfg_t;

typedef struct {
    char host[NET_URL_MAX + 1];
    uint16_t port;
    const char *path; /* points into the URL that was split */
} netcfg_url_t;

enum { NET_WIFI_OFF, NET_WIFI_CONNECTING, NET_WIFI_CONNECTED, NET_WIFI_FAILED };
enum {
    NET_RELAY_NONE, NET_RELAY_CONNECTING, NET_RELAY_CONNECTED,
    NET_RELAY_AUTH_FAILED, NET_RELAY_UNREACHABLE, NET_RELAY_CAPTIVE, NET_RELAY_TLS_FAILED,
};

typedef struct {
    int wifi;
    int relay;
    uint8_t ip[4]; /* meaningful only while wifi is connected */
    int rssi;
} net_status_t;

/* Writes the PSK identity for a factory MAC. */
void netcfg_identity(const uint8_t mac[6], char out[NET_IDENTITY_LEN + 1]);
/* Parses a PAIR payload into out; returns NULL or the PAIR_RESULT error code.
 * out is wiped on failure; identity is the board's own PSK identity. */
const char *netcfg_parse_pair(const uint8_t *text, size_t len, const char *identity, netcfg_t *out);
/* Parses a SET_REMOTE payload; returns true and writes the URL when it is valid. */
bool netcfg_parse_remote(const uint8_t *text, size_t len, char url[NET_URL_MAX + 1]);
/* Checks a remote URL (docs/protocol.md "SET_REMOTE"); fills parts when it is valid and parts is not NULL. */
bool netcfg_url_split(const char *url, size_t len, netcfg_url_t *parts);
/* Writes PAIR_RESULT or FORGET_RESULT JSON for err (NULL is success); returns its length. */
size_t netcfg_result_json(const char *err, char *buf, size_t cap);
/* Writes WIFI_STATUS JSON; returns its length, 0 when cap is too small. */
size_t netcfg_status_json(const net_status_t *st, char *buf, size_t cap);
/* Zeroes memory in a way the compiler keeps. */
void netcfg_wipe(void *p, size_t n);
