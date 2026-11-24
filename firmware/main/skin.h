#pragma once
#include <stdint.h>
#include "apack.h"

/* Finds, maps and parses the animation pack partition, then checks its payload CRC on a one-shot task; never aborts. */
void skin_init(void);
/* AP_VERIFYING until the payload check ends; any status but AP_OK means Clawd. */
ap_status_t skin_status(void);
/* The parsed pack while the status is AP_OK, else NULL. */
const ap_pack_t *skin_pack(void);
/* Marks a verified pack bad, for a frame that fails to decode. */
void skin_fail(ap_status_t st);
/* Milliseconds the payload check took; 0 until it ends. */
uint32_t skin_verify_ms(void);
