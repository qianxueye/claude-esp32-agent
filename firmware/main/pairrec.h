#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_config.h"
#include "netcfg.h"

/* Stored pairing records and the rules that pick the remote URL and its saves (docs/network.md "Storage"). */

#define PR_VERSION 1
#define PR_HEADER_BYTES 7 /* version, pairing id (u32 LE), data length (u16 LE) */
#define PR_PAIR_BLOB_MAX (PR_HEADER_BYTES + NET_PAIR_MAX_BYTES)
#define PR_REMOTE_BLOB_MAX (PR_HEADER_BYTES + NET_URL_MAX)

/* Writes one record of at most max data bytes; returns its size, 0 when id is 0 or it does not fit. */
size_t pr_encode(uint32_t id, const void *data, size_t len, size_t max, uint8_t *out, size_t cap);
/* Checks version, id and exact length; *data points into blob. */
bool pr_decode(const uint8_t *blob, size_t n, size_t max, uint32_t *id, const uint8_t **data, size_t *len);
/* The id for a new PAIR: one more than every id still in storage, never 0. */
uint32_t pr_next_id(uint32_t pair_id, uint32_t remote_id);
/* The remote URL in effect: the SET_REMOTE record of the same pairing, else the PAIR's own, else NULL. */
const char *pr_effective_remote(uint32_t pair_id, const netcfg_t *cfg, uint32_t remote_id, const char *remote_url);

typedef struct {
    bool save; /* differs from the stored URL */
    bool apply; /* differs from the URL in use */
} pr_remote_plan_t;

/* Plans a valid SET_REMOTE URL against the stored URL and the one in use (NULL for none). */
pr_remote_plan_t pr_remote_plan(const char *stored, const char *in_use, const char *url);
/* NVS init results that call for erasing the partition; netstore.c checks them against nvs.h. */
#define PR_ERR_NVS_NO_FREE_PAGES 0x110d
#define PR_ERR_NVS_NEW_VERSION_FOUND 0x1110

enum { PR_BOOT_OK, PR_BOOT_REFORMAT, PR_BOOT_UNUSABLE };

/* What boot does with the pairing partition after its first init returned init_err. */
int pr_boot_action(int32_t init_err);
/* A SET_REMOTE save may proceed only for the pairing and generation it was planned under. */
bool pr_save_current(uint32_t stored_id, uint32_t want_id, uint32_t gen, uint32_t want_gen);
