#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_config.h"

/* Pairing storage in its own NVS partition: key "pair" holds a record of the PAIR text, "remote" one of the SET_REMOTE URL (docs/network.md "Storage").
 * Every function takes the store's own lock and no other. */

/* NS_FAILED: nothing written, the previous record stays; NS_UNKNOWN: either record may load at the next boot. */
enum { NS_SAVED, NS_FAILED, NS_STALE, NS_UNKNOWN };

typedef struct {
    size_t len; /* PAIR text length, 0 unless the pair record was found */
    uint32_t pair_id, remote_id; /* 0 when the record is not found */
    int pair_class, remote_class; /* PR_READ_* */
} netstore_loaded_t;

/* Prepares the store; false when it cannot be used this boot. */
bool netstore_init(void);
/* Reads both records once at boot; text gets the PAIR text and url the SET_REMOTE URL, or "". */
void netstore_load(uint8_t *text, size_t cap, char url[NET_URL_MAX + 1], netstore_loaded_t *out);
/* The store's content is known and writes are allowed (docs/network.md "Storage"). */
bool netstore_known(void);
/* Replaces the pairing with a validated PAIR text under a new pairing id. */
int netstore_save_pair(const uint8_t *text, size_t len, uint32_t *pair_id);
/* Stores a SET_REMOTE URL for pairing_id; NS_STALE when the pairing or *gen changed since the plan. */
int netstore_save_remote(uint32_t pair_id, const _Atomic uint32_t *gen, uint32_t want_gen, const char *url);
/* Erases the pairing; false on a storage error. */
bool netstore_forget(void);
