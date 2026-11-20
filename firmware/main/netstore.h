#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_config.h"

/* Pairing storage in its own NVS partition: key "pair" holds a record of the PAIR text, "remote" one of the SET_REMOTE URL (docs/network.md "Storage").
 * Every function takes the store's own lock and no other. */

enum { NS_SAVED, NS_FAILED, NS_STALE };

/* Prepares the store; false when it cannot be used this boot. */
bool netstore_init(void);
/* Reads the PAIR text and its pairing id; returns the text length, 0 when none or malformed.
 * url and *remote_id hold the SET_REMOTE record, or "" and 0. */
size_t netstore_load(uint8_t *text, size_t cap, uint32_t *pair_id, char url[NET_URL_MAX + 1], uint32_t *remote_id);
/* Replaces the pairing with a validated PAIR text under a new pairing id; false on a storage error. */
bool netstore_save_pair(const uint8_t *text, size_t len, uint32_t *pair_id);
/* Stores a SET_REMOTE URL for pairing_id; NS_STALE when the pairing or *gen changed since the plan. */
int netstore_save_remote(uint32_t pair_id, const _Atomic uint32_t *gen, uint32_t want_gen, const char *url);
/* Erases the pairing; false on a storage error. */
bool netstore_forget(void);
