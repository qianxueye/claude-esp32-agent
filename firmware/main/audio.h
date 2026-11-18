#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    float mono;
    float left;
    float right;
} mic_rms_t;

void audio_start(void);
void audio_set_volume(uint8_t percent);
uint8_t audio_get_volume(void);

/* Speaker queue */
void audio_spk_push(const uint8_t *pcm, size_t len);
void audio_spk_flush(void);
void audio_spk_end(void);
uint32_t audio_spk_queued(void);
uint32_t audio_spk_bytes_played(void);
uint32_t audio_spk_overflow_frames(void);
/* True while the amplifier is switched on. */
bool audio_pa_active(void);
/* bit1 = requested state, bit0 = pin level read back */
int audio_pa_is_on(void);

/* Microphone */
void audio_mic_set_streaming(bool on);
void audio_mic_hold(bool on);
void audio_mic_window_reset(void);
mic_rms_t audio_mic_window_rms(void);

typedef struct {
    uint32_t spk_underruns; /* queue ran dry mid-segment */
    uint32_t spk_slow_writes;
    uint32_t mic_gaps; /* interval between mic frames beyond one frame plus margin */
    uint32_t mic_dropped; /* MIC_AUDIO frames the transport refused */
} audio_counters_t;
audio_counters_t audio_counters(void);

/* Levels for the UI, 0..1000 */
uint16_t audio_mic_level(void);
uint16_t audio_spk_level(void);
