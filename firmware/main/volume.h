#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Device-owned volume: NVS storage, codec level and the VOLUME report. Rules in docs/protocol.md "Volume". */

typedef struct {
    uint32_t arms, cancels, edge, saves, over[3];
    int user, effective;
    bool set;
} volume_stats_t;

/* The 2 s log line; over holds the counters capped at ARC_OV_LOG_MAX. */
#define VOLUME_LINE_FMT "knob vol=%d eff=%d set=%d arms=%u cancels=%u edge=%u saves=%u ov=%u/%u/%u"

/* Loads the stored volume and applies it; call after nvstore_init. When NVS is unusable or the read fails, the volume stays in RAM this boot. */
void volume_init(void);
void volume_hello_fields(int *vol, bool *set);
/* SET_VOLUME payload of one or two bytes. */
void volume_host_set(const uint8_t *payload, int len);
/* A valid HOST_HELLO or entry to OFFLINE ends a transient override. */
void volume_session_reset(void);
/* Arms the knob; returns the user volume it armed with, read under the same lock. */
int volume_knob_arm(void);
void volume_knob_set(int v);
void volume_knob_end(bool cancel);
void volume_count_edge(void);
/* A stretch of cells (1 to ARC_OVER_CELLS) was shown. */
void volume_count_over(int cells);
/* A change is due for writing under the shared clock. */
bool volume_save_due(uint32_t now_ms, bool quiet);
/* Writes a due change; call from the life task when vs_save_pick picks the volume. Returns true when a write happened. */
bool volume_poll(uint32_t now_ms, bool quiet);
void volume_stats(volume_stats_t *out);
