#include "audio.h"
#include <math.h>
#include <string.h>
#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "proto.h"
#include "transport.h"

static const char *TAG = "audio";

static esp_codec_dev_handle_t s_spk, s_mic;
static uint8_t *s_q;
static size_t s_q_head, s_q_count;
static SemaphoreHandle_t s_q_mtx;
static bool s_end_pending, s_writing;
static volatile uint32_t s_bytes_played, s_overflow;
static uint32_t s_consumed, s_flush_gen;
static volatile uint32_t s_spk_underruns, s_spk_slow, s_mic_gaps, s_mic_dropped;
static bool s_in_segment;
static volatile uint8_t s_volume = DEFAULT_VOLUME;
static volatile uint16_t s_spk_level, s_mic_level;
static volatile bool s_streaming;
static bool s_pa_on = true;
static volatile int s_hold;
static portMUX_TYPE s_win_mux = portMUX_INITIALIZER_UNLOCKED;
static double s_win_mono, s_win_l, s_win_r;
static uint32_t s_win_n;

static void send_spk_status(uint32_t queued, uint8_t flags, uint32_t consumed)
{
    uint8_t p[9] = {queued & 0xFF, (queued >> 8) & 0xFF, (queued >> 16) & 0xFF, queued >> 24, flags,
                    consumed & 0xFF, (consumed >> 8) & 0xFF, (consumed >> 16) & 0xFF, consumed >> 24};
    transport_send(T_SPK_STATUS, p, sizeof(p));
}

void audio_set_volume(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
    if (s_spk) {
        esp_codec_dev_set_out_vol(s_spk, percent);
    }
}

uint8_t audio_get_volume(void)
{
    return s_volume;
}

void audio_spk_push(const uint8_t *pcm, size_t len)
{
    xSemaphoreTake(s_q_mtx, portMAX_DELAY);
    if (len > SPK_QUEUE_BYTES - s_q_count) {
        s_overflow++;
    } else {
        size_t tail = (s_q_head + s_q_count) % SPK_QUEUE_BYTES;
        size_t first = SPK_QUEUE_BYTES - tail;
        if (first > len) {
            first = len;
        }
        memcpy(s_q + tail, pcm, first);
        memcpy(s_q, pcm + first, len - first);
        s_q_count += len;
        s_end_pending = false;
    }
    xSemaphoreGive(s_q_mtx);
}

void audio_spk_flush(void)
{
    xSemaphoreTake(s_q_mtx, portMAX_DELAY);
    s_consumed += (uint32_t)s_q_count;
    s_q_head = 0;
    s_q_count = 0;
    s_end_pending = false;
    s_flush_gen++;
    s_in_segment = false;
    uint32_t consumed = s_consumed;
    send_spk_status(0, 0, consumed);
    xSemaphoreGive(s_q_mtx);
}

void audio_spk_end(void)
{
    xSemaphoreTake(s_q_mtx, portMAX_DELAY);
    if (s_q_count == 0 && !s_writing) {
        s_end_pending = false;
        uint32_t consumed = s_consumed;
        xSemaphoreGive(s_q_mtx);
        send_spk_status(0, 2, consumed);
        return;
    }
    s_end_pending = true;
    xSemaphoreGive(s_q_mtx);
}

uint32_t audio_spk_queued(void)
{
    return (uint32_t)s_q_count;
}

uint32_t audio_spk_bytes_played(void)
{
    return s_bytes_played;
}

uint32_t audio_spk_overflow_frames(void)
{
    return s_overflow;
}

audio_counters_t audio_counters(void)
{
    return (audio_counters_t){s_spk_underruns, s_spk_slow, s_mic_gaps, s_mic_dropped};
}

void audio_mic_set_streaming(bool on)
{
    s_streaming = on;
}

void audio_mic_hold(bool on)
{
    s_hold += on ? 1 : -1;
}

void audio_mic_window_reset(void)
{
    portENTER_CRITICAL(&s_win_mux);
    s_win_mono = s_win_l = s_win_r = 0;
    s_win_n = 0;
    portEXIT_CRITICAL(&s_win_mux);
}

mic_rms_t audio_mic_window_rms(void)
{
    mic_rms_t r = {0};
    portENTER_CRITICAL(&s_win_mux);
    if (s_win_n) {
        r.mono = sqrtf((float)(s_win_mono / s_win_n));
        r.left = sqrtf((float)(s_win_l / s_win_n));
        r.right = sqrtf((float)(s_win_r / s_win_n));
    }
    portEXIT_CRITICAL(&s_win_mux);
    return r;
}

uint16_t audio_mic_level(void)
{
    return s_mic_level;
}

uint16_t audio_spk_level(void)
{
    return s_spk_level;
}

static uint16_t to_level(float rms, float full_scale)
{
    float v = rms / full_scale * 1000.0f;
    return v > 1000.0f ? 1000 : (uint16_t)v;
}

static void pa_set(bool on)
{
    if (on != s_pa_on) {
        gpio_set_level(BSP_POWER_AMP_IO, on ? 1 : 0);
        s_pa_on = on;
    }
}

int audio_pa_is_on(void)
{
    return s_pa_on * 2 + gpio_get_level(BSP_POWER_AMP_IO);
}

static void spk_task(void *arg)
{
    uint32_t idle_since = now_ms();
    static int16_t mono[AUDIO_FRAME_SAMPLES];
    static int16_t stereo[AUDIO_FRAME_SAMPLES * 2];
    uint32_t last_status = 0;
    for (;;) {
        xSemaphoreTake(s_q_mtx, portMAX_DELAY);
        size_t n = s_q_count < AUDIO_FRAME_BYTES ? s_q_count : AUDIO_FRAME_BYTES;
        size_t first = SPK_QUEUE_BYTES - s_q_head;
        if (first > n) {
            first = n;
        }
        memcpy(mono, s_q + s_q_head, first);
        memcpy((uint8_t *)mono + first, s_q, n - first);
        s_q_head = (s_q_head + n) % SPK_QUEUE_BYTES;
        s_q_count -= n;
        s_consumed += (uint32_t)n;
        s_writing = n > 0;
        uint32_t gen = s_flush_gen;
        xSemaphoreGive(s_q_mtx);

        uint32_t t = now_ms();
        if (n == 0) {
            if (s_in_segment && !s_end_pending && t - idle_since >= SPK_UNDERRUN_GRACE_MS) {
                s_spk_underruns++;
                s_in_segment = false;
            }
            s_spk_level = 0;
            if (t - idle_since >= PA_IDLE_OFF_MS) {
                pa_set(false);
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        idle_since = t;
        if (!s_pa_on) {
            pa_set(true);
            vTaskDelay(pdMS_TO_TICKS(PA_SETTLE_MS));
        }
        size_t samples = n / 2;
        double acc = 0;
        for (size_t i = 0; i < samples; i++) {
            stereo[2 * i] = stereo[2 * i + 1] = mono[i];
            acc += (double)mono[i] * mono[i];
        }
        s_spk_level = to_level(sqrtf((float)(acc / samples)), SPK_LEVEL_FULL_SCALE);
        uint32_t w0 = now_ms();
        esp_codec_dev_write(s_spk, stereo, samples * 4);
        if (now_ms() - w0 > AUDIO_FRAME_MS + SPK_SLOW_WRITE_MARGIN_MS) {
            s_spk_slow++;
        }
        s_in_segment = true;
        s_bytes_played += n;

        xSemaphoreTake(s_q_mtx, portMAX_DELAY);
        s_writing = false;
        bool drained = s_end_pending && s_q_count == 0;
        if (drained) {
            s_end_pending = false;
            s_in_segment = false;
        }
        uint32_t queued = (uint32_t)s_q_count, consumed = s_consumed;
        bool flushed = gen != s_flush_gen;
        xSemaphoreGive(s_q_mtx);
        if (flushed) {
            continue;
        }
        if (drained) {
            send_spk_status(0, 2, consumed);
            last_status = t;
        } else if (t - last_status >= PROTO_SPK_STATUS_PERIOD_MS) {
            send_spk_status(queued, 1, consumed);
            last_status = t;
        }
    }
}

static void mic_task(void *arg)
{
    static int16_t raw[AUDIO_FRAME_SAMPLES * 2];
    static int16_t mono[AUDIO_FRAME_SAMPLES];
    bool was_on = false;
    uint32_t discard_until = 0, last_read = 0;
    for (;;) {
        bool want = s_streaming || s_hold > 0;
        if (!want) {
            was_on = false;
            last_read = 0;
            s_mic_level = 0;
            vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
            continue;
        }
        if (!was_on) {
            was_on = true;
            discard_until = now_ms() + MIC_DISCARD_MS;
        }
        if (esp_codec_dev_read(s_mic, raw, sizeof(raw)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS));
            continue;
        }
        uint32_t rt = now_ms();
        if (last_read && rt - last_read > AUDIO_FRAME_MS + MIC_GAP_MARGIN_MS) {
            s_mic_gaps++;
        }
        last_read = rt;
        double sl = 0, sr = 0, sm = 0;
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            int l = raw[2 * i], r = raw[2 * i + 1];
            int m = MIC_CHANNEL_MODE == MIC_CH_LEFT ? l : MIC_CHANNEL_MODE == MIC_CH_RIGHT ? r : (l + r) / 2;
            mono[i] = (int16_t)m;
            sl += (double)l * l;
            sr += (double)r * r;
            sm += (double)m * m;
        }
        if ((int32_t)(now_ms() - discard_until) < 0) {
            continue;
        }
        s_mic_level = to_level(sqrtf((float)(sm / AUDIO_FRAME_SAMPLES)), MIC_LEVEL_FULL_SCALE);
        portENTER_CRITICAL(&s_win_mux);
        s_win_mono += sm / AUDIO_FRAME_SAMPLES;
        s_win_l += sl / AUDIO_FRAME_SAMPLES;
        s_win_r += sr / AUDIO_FRAME_SAMPLES;
        s_win_n++;
        portEXIT_CRITICAL(&s_win_mux);
        if (s_streaming) {
            if (!transport_send(T_MIC_AUDIO, mono, AUDIO_FRAME_BYTES)) {
                s_mic_dropped++;
            }
        }
    }
}

void audio_start(void)
{
    s_q = heap_caps_malloc(SPK_QUEUE_BYTES, MALLOC_CAP_SPIRAM);
    s_q_mtx = xSemaphoreCreateMutex();
    assert(s_q && s_q_mtx);
    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    assert(s_spk && s_mic);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS,
        .channel = AUDIO_CHANNELS,
        .sample_rate = AUDIO_SAMPLE_RATE,
    };
    int rs = esp_codec_dev_open(s_spk, &fs);
    int rm = esp_codec_dev_open(s_mic, &fs);
    ESP_LOGI(TAG, "codec open spk=%d mic=%d", rs, rm);
    esp_codec_dev_set_out_vol(s_spk, s_volume);
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    gpio_input_enable(BSP_POWER_AMP_IO);
    pa_set(false);
    xTaskCreate(spk_task, "spk", STACK_SPK, NULL, PRIO_SPK, NULL);
    xTaskCreate(mic_task, "mic", STACK_MIC, NULL, PRIO_MIC, NULL);
}
