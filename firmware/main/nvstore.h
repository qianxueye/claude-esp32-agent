#pragma once
#include <stdint.h>
#include "esp_err.h"

/* NVS storage shared by every stored setting: one init, one erase rule and one write clock. Rules in docs/protocol.md "Volume". */

/* Initialises NVS, erasing the partition once when it is full or from a newer version; any other failure turns storage off for this boot. */
void nvstore_init(void);
/* Reads key from VOL_NVS_NAMESPACE; returns VS_LOAD_FOUND, VS_LOAD_ABSENT or VS_LOAD_ERROR, also when storage is off. */
int nvstore_load_u8(const char *key, uint8_t *raw);
/* Writes and commits key; now_ms and key become the last attempt whatever the outcome. */
esp_err_t nvstore_save_u8(const char *key, uint8_t v, uint32_t now_ms);
/* The one clock behind VOL_SAVE_MIN_MS for every key. */
uint32_t nvstore_last_write(void);
/* The key of the last write attempt, NULL before the first. */
const char *nvstore_last_key(void);
