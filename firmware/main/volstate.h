#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

/* Volume bookkeeping without hardware: user volume, transient override, saved value. Rules in docs/protocol.md "Volume". */

enum { VOL_MODE_TRANSIENT = 0, VOL_MODE_PERSIST = 1, VOL_MODE_SEED = 2 };
/* Outcome of reading the stored volume at boot; VS_LOAD_ERROR means the stored value is unknown. */
enum { VS_LOAD_ABSENT, VS_LOAD_FOUND, VS_LOAD_ERROR };
enum { VS_STREAK_SAME, VS_STREAK_START, VS_STREAK_END };

typedef struct {
    int user; /* the volume the user chose; the tentative value while the knob is armed */
    int stored; /* last value known to be in NVS, -1 when none */
    int ovr; /* transient override, -1 when none */
    int base; /* user volume when the knob armed */
    bool set; /* a user volume exists */
    bool ram; /* storage is off for this boot: never write */
    bool knob;
    bool failing; /* the last write failed */
} volst_t;

int vs_effective(const volst_t *s);
/* Applies the load outcome; returns true when a valid stored value was found. */
bool vs_load(volst_t *s, int result, uint32_t raw);
/* Applies a SET_VOLUME payload; returns true when the user volume was set. */
bool vs_host_set(volst_t *s, const uint8_t *payload, int len);
/* A valid HOST_HELLO or entry to OFFLINE ends a transient override. */
void vs_session_reset(volst_t *s);
/* Arms the knob; returns the user volume it armed with. */
int vs_knob_arm(volst_t *s);
void vs_knob_set(volst_t *s, int v);
/* Ends the knob; returns true when a commit changed the user volume. */
bool vs_knob_end(volst_t *s, bool cancel);
/* Records that v was written to NVS; call only after a successful write. */
void vs_saved(volst_t *s, int v);
/* The committed user volume differs from the stored one and storage is on. */
bool vs_needs_save(const volst_t *s);
/* Records a write outcome; returns VS_STREAK_START or VS_STREAK_END when a run of failures starts or ends. */
int vs_write_streak(volst_t *s, bool wrote);
bool vs_save_due(const volst_t *s, uint32_t now, uint32_t last_write, bool quiet);
