#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_config.h"

/* Pairing storage in NVS namespace NET_NVS_NAMESPACE: key "pair" holds the PAIR text, "remote" the remote URL. */

/* Reads the saved PAIR text into buf; returns its length, 0 when there is none or it cannot be read. */
size_t netstore_load_pair(uint8_t *buf, size_t cap);
/* Reads the saved remote URL; returns false when there is none. */
bool netstore_load_remote(char url[NET_URL_MAX + 1]);
/* Replaces the pairing with a validated PAIR text and its remote URL (NULL for none); false on a storage error. */
bool netstore_save_pair(const uint8_t *text, size_t len, const char *remote);
bool netstore_save_remote(const char *url);
/* Erases the namespace; false on a storage error. */
bool netstore_forget(void);
