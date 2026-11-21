#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Wi-Fi station, relay sessions over the LAN and the tunnel, and the pairing they use (docs/network.md). */

/* Loads the pairing; call after NVS is initialised and before the first HELLO. */
void net_init(void);
/* Starts the network task; its first WIFI_STATUS follows the boot HELLO. */
void net_start(void);
bool net_paired(void);
/* Validates and stores a PAIR payload; returns NULL or the PAIR_RESULT error code. */
const char *net_pair(const uint8_t *payload, size_t len);
/* Erases the pairing and drops it from RAM; returns NULL or the FORGET_RESULT error code. */
const char *net_forget(void);
/* Applies a SET_REMOTE payload while paired; an invalid or unchanged URL is ignored. */
void net_set_remote(const uint8_t *payload, size_t len);
/* Writes the current WIFI_STATUS JSON; returns its length. */
size_t net_status_json(char *buf, size_t cap);
/* Queues one frame for the open network session; reliable waits for room as on USB. */
bool net_send(uint8_t type, const void *payload, size_t len, bool reliable);
/* The relay's HOST_HELLO made the open session the active link. */
void net_session_activated(void);
/* The configuration generation; PAIR and FORGET advance it. */
uint32_t net_generation(void);
uint32_t net_stack_hwm(void);
uint32_t net_tx_dropped(void);
