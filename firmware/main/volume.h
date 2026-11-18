#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Device-owned volume: NVS storage, codec level and the VOLUME report. Rules in docs/protocol.md "Volume". */

typedef struct {
    uint32_t arms, cancels, edge, saves;
    int user, effective;
    bool set;
} volume_stats_t;

/* Loads the stored volume and applies it; when NVS is unusable or the read fails, the volume stays in RAM this boot. */
void volume_init(void);
void volume_hello_fields(int *vol, bool *set);
/* SET_VOLUME payload of one or two bytes. */
void volume_host_set(const uint8_t *payload, int len);
/* A valid HOST_HELLO or entry to OFFLINE ends a transient override. */
void volume_session_reset(void);
void volume_knob_arm(void);
void volume_knob_set(int v);
void volume_knob_end(bool cancel);
void volume_count_edge(void);
/* Writes a due change; call from the life task. Returns true when a write happened. */
bool volume_poll(uint32_t now_ms, bool quiet);
void volume_stats(volume_stats_t *out);
