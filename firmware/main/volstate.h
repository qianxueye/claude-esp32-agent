#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

/* Volume bookkeeping without hardware: user volume, transient override, saved value. Rules in docs/protocol.md "Volume". */

enum { VOL_MODE_TRANSIENT = 0, VOL_MODE_PERSIST = 1, VOL_MODE_SEED = 2 };

typedef struct {
    int user; /* the volume the user chose */
    int stored; /* value in NVS, -1 when none */
    int ovr; /* transient override, -1 when none */
    int base; /* user volume when the knob armed */
    bool set; /* a user volume exists */
    bool dirty; /* user differs from stored */
    bool knob;
} volst_t;

int vs_effective(const volst_t *s);
/* Applies the stored value; returns true when it was valid. */
bool vs_load(volst_t *s, bool found, uint32_t raw);
/* Applies a SET_VOLUME payload; returns true when the user volume was set. */
bool vs_host_set(volst_t *s, const uint8_t *payload, int len);
/* A valid HOST_HELLO or entry to OFFLINE ends a transient override. */
void vs_session_reset(volst_t *s);
void vs_knob_arm(volst_t *s);
void vs_knob_set(volst_t *s, int v);
/* Ends the knob; returns true when a commit changed the user volume. */
bool vs_knob_end(volst_t *s, bool cancel);
void vs_saved(volst_t *s, int v);
bool vs_save_due(const volst_t *s, uint32_t now, uint32_t last_write, bool quiet);
