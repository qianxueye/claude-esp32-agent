#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "audio.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "imu.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "proto.h"
#include "transport.h"
#include "ui.h"

static const char *TAG = "app";

static volatile dev_state_t s_state = ST_IDLE;
static volatile uint32_t s_last_host_ms;
static volatile bool s_ready;
static volatile bool s_selftest_busy;
static QueueHandle_t s_snap_queue;
static SemaphoreHandle_t s_state_mtx;

static void send_hello(const char *reason)
{
    char json[HELLO_JSON_BYTES];
    int n = snprintf(json, sizeof(json),
                     "{\"proto\":%d,\"fw\":\"%s\",\"rate\":%d,\"reason\":\"%s\",\"link\":\"usb\",\"paired\":false}",
                     PROTO_VERSION, FW_VERSION, AUDIO_SAMPLE_RATE, reason);
    transport_send(T_HELLO, json, n);
}

static void send_json(uint8_t type, const char *json)
{
    transport_send(type, json, strlen(json));
}

/* Finds "key": in a small JSON payload; returns the position after the colon or NULL. */
static const char *json_value(const char *json, const char *key)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return NULL;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return NULL;
    }
    while (*++p == ' ') {
    }
    return p;
}

static bool copy_json(const proto_frame_t *f, char *out, size_t cap)
{
    if (f->len == 0 || f->len >= cap) {
        return false;
    }
    memcpy(out, f->payload, f->len);
    out[f->len] = 0;
    return true;
}

static bool host_hello_proto_ok(const proto_frame_t *f)
{
    char json[HOST_JSON_BYTES];
    if (!copy_json(f, json, sizeof(json))) {
        return false;
    }
    const char *v = json_value(json, "proto");
    return v && atoi(v) == PROTO_VERSION;
}

static void handle_debug(const proto_frame_t *f)
{
    char json[HOST_JSON_BYTES];
    if (!copy_json(f, json, sizeof(json))) {
        return;
    }
    const char *v = json_value(json, "angle_override");
    if (v) {
        if (strncmp(v, "null", 4) == 0) {
            ui_set_angle_override(false, 0);
        } else {
            ui_set_angle_override(true, strtof(v, NULL));
        }
    }
    v = json_value(json, "bench");
    if (v) {
        ui_set_bench(atoi(v));
    }
}

static void set_state(dev_state_t s)
{
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    s_state = s;
    ui_set_state(s);
    audio_mic_set_streaming(s == ST_LISTENING);
    xSemaphoreGive(s_state_mtx);
    ESP_LOGI(TAG, "state %d", (int)s);
}

static void selftest_task(void *arg)
{
    audio_mic_hold(true);
    vTaskDelay(pdMS_TO_TICKS(MIC_DISCARD_MS + 100));
    audio_mic_window_reset();
    vTaskDelay(pdMS_TO_TICKS(SELFTEST_BEFORE_MS));
    mic_rms_t before = audio_mic_window_rms();

    int n = AUDIO_SAMPLE_RATE * SELFTEST_TONE_MS / 1000;
    int16_t *tone = heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
    uint32_t played0 = audio_spk_bytes_played();
    audio_mic_window_reset();
    if (tone) {
        for (int i = 0; i < n; i++) {
            tone[i] = (int16_t)(SELFTEST_TONE_AMPLITUDE * sinf(2.0f * (float)M_PI * SELFTEST_TONE_HZ * i / AUDIO_SAMPLE_RATE));
        }
        for (size_t off = 0; off < (size_t)n * 2; off += 4096) {
            size_t chunk = (size_t)n * 2 - off < 4096 ? (size_t)n * 2 - off : 4096;
            audio_spk_push((uint8_t *)tone + off, chunk);
        }
        free(tone);
    }
    vTaskDelay(pdMS_TO_TICKS(SELFTEST_TONE_MS + 150));
    mic_rms_t during = audio_mic_window_rms();
    uint32_t played = audio_spk_bytes_played() - played0;
    audio_mic_hold(false);

    char json[128];
    int len = snprintf(json, sizeof(json), "{\"mic_rms_before\":%.1f,\"mic_rms_during\":%.1f,\"spk_bytes_played\":%u}",
                       before.mono, during.mono, (unsigned)played);
    transport_send(T_SELFTEST_RESULT, json, len);
    ESP_LOGI(TAG, "selftest L before/during %.1f/%.1f R %.1f/%.1f", before.left, during.left, before.right,
             during.right);
    s_selftest_busy = false;
    vTaskDelete(NULL);
}

static void send_snapshot(lv_draw_buf_t *buf)
{
    const uint32_t w = buf->header.w, h = buf->header.h, stride = buf->header.stride;
    const uint32_t row_bytes = w * 2, total = row_bytes * h;
    char json[64];
    int n = snprintf(json, sizeof(json), "{\"w\":%u,\"h\":%u,\"fmt\":\"RGB565\"}", (unsigned)w, (unsigned)h);
    bool ok = transport_send_reliable(T_SNAP_BEGIN, json, n);
    static uint8_t chunk[PROTO_MAX_PAYLOAD];
    uint32_t off = 0;
    while (ok && off < total) {
        uint32_t len = total - off < sizeof(chunk) ? total - off : sizeof(chunk);
        for (uint32_t done = 0; done < len;) {
            uint32_t pos = off + done, row = pos / row_bytes, col = pos % row_bytes;
            uint32_t part = row_bytes - col < len - done ? row_bytes - col : len - done;
            memcpy(chunk + done, (const uint8_t *)buf->data + row * stride + col, part);
            done += part;
        }
        ok = transport_send_reliable(T_SNAP_DATA, chunk, len);
        off += len;
    }
    if (ok) {
        uint8_t p[4] = {total & 0xFF, (total >> 8) & 0xFF, (total >> 16) & 0xFF, total >> 24};
        transport_send_reliable(T_SNAP_END, p, 4);
    } else {
        ESP_LOGW(TAG, "snapshot aborted at %u", (unsigned)off);
    }
    ui_snapshot_free(buf);
}

static void snapshot_task(void *arg)
{
    for (;;) {
        lv_draw_buf_t *buf;
        if (xQueueReceive(s_snap_queue, &buf, portMAX_DELAY) == pdTRUE) {
            send_snapshot(buf);
        }
    }
}

static void on_snapshot_request(lv_draw_buf_t *buf)
{
    if (buf && xQueueSend(s_snap_queue, &buf, 0) != pdTRUE) {
        ui_snapshot_free(buf);
    }
}

static void on_frame(const proto_frame_t *f)
{
    if (!s_ready) {
        return;
    }
    if (f->type == T_HOST_HELLO && !host_hello_proto_ok(f)) {
        ESP_LOGW(TAG, "HOST_HELLO ignored: proto mismatch (device speaks %d)", PROTO_VERSION);
        return;
    }
    s_last_host_ms = now_ms();
    lv_draw_buf_t *snap = f->type == T_SNAPSHOT ? ui_snapshot_take() : NULL;
    const char *reply = "reply";
    if (s_state == ST_OFFLINE && f->type != T_SNAPSHOT) {
        set_state(ST_IDLE);
        reply = "resume";
        if (f->type != T_HOST_HELLO) {
            send_hello("resume");
        }
    }
    switch (f->type) {
    case T_HOST_HELLO:
        send_hello(reply);
        send_json(T_WIFI_STATUS, "{\"wifi\":\"off\",\"relay\":\"none\"}");
        break;
    case T_SET_STATE:
        if (f->len == 1 && f->payload[0] < ST_COUNT) {
            set_state((dev_state_t)f->payload[0]);
        }
        break;
    case T_SET_VOLUME:
        if (f->len == 1) {
            audio_set_volume(f->payload[0]);
        }
        break;
    case T_PAIR:
        send_json(T_PAIR_RESULT, "{\"ok\":false,\"error\":\"not implemented\"}");
        break;
    case T_FORGET:
        send_json(T_FORGET_RESULT, "{\"ok\":false,\"error\":\"not implemented\"}");
        break;
    case T_SPK_AUDIO:
        if (f->len > 0 && (f->len & 1) == 0) {
            audio_spk_push(f->payload, f->len);
        }
        break;
    case T_SPK_FLUSH:
        audio_spk_flush();
        break;
    case T_SPK_END:
        audio_spk_end();
        break;
    case T_PING:
        if (f->len == 4) {
            transport_send(T_PONG, f->payload, 4);
        }
        break;
    case T_SELFTEST:
        if (!s_selftest_busy) {
            s_selftest_busy = true;
            xTaskCreate(selftest_task, "selftest", STACK_WORKER, NULL, PRIO_WORKER, NULL);
        }
        break;
    case T_SNAPSHOT:
        on_snapshot_request(snap);
        break;
    case T_DEBUG:
        handle_debug(f);
        break;
    default:
        break;
    }
}

static void life_task(void *arg)
{
    uint32_t last_stats = now_ms();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LIFE_PERIOD_MS));
        uint32_t last_host = s_last_host_ms;
        uint32_t t = now_ms();
        if (s_ready && s_state != ST_OFFLINE && t - last_host >= PROTO_HOST_SILENCE_TIMEOUT_MS) {
            set_state(ST_OFFLINE);
        }
        if (t - last_stats >= STATS_LOG_PERIOD_MS) {
            last_stats = t;
            transport_stats_t st;
            transport_get_stats(&st);
            audio_counters_t ac = audio_counters();
            ESP_LOGI(TAG, "stats rx=%u/%uB crc=%u skip=%u tx=%u/%uB drop=%u spkq=%u spkovf=%u pa=%d under=%u slow=%u micgap=%u micdrop=%u", (unsigned)st.rx_frames,
                     (unsigned)st.rx_bytes, (unsigned)st.rx_crc_errors, (unsigned)st.rx_bytes_skipped,
                     (unsigned)st.tx_frames, (unsigned)st.tx_bytes, (unsigned)st.tx_dropped,
                     (unsigned)audio_spk_queued(), (unsigned)audio_spk_overflow_frames(), (int)audio_pa_is_on(),
                     (unsigned)ac.spk_underruns, (unsigned)ac.spk_slow_writes, (unsigned)ac.mic_gaps, (unsigned)ac.mic_dropped);
        }
    }
}

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int low = 0;
    bool sent = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_BUTTON_POLL_MS));
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            if (++low >= BOOT_BUTTON_DEBOUNCE_POLLS && !sent) {
                sent = true;
                uint8_t code = 3;
                transport_send(T_INPUT, &code, 1);
            }
        } else {
            low = 0;
            sent = false;
        }
    }
}

void app_main(void)
{
    s_state_mtx = xSemaphoreCreateMutex();
    s_snap_queue = xQueueCreate(SNAPSHOT_QUEUE_DEPTH, sizeof(lv_draw_buf_t *));
    xTaskCreate(snapshot_task, "snap", STACK_WORKER, NULL, PRIO_WORKER, NULL);
    s_last_host_ms = now_ms();
    transport_start(on_frame);
    ESP_LOGI(TAG, "boot fw=%s reset_reason=%d", FW_VERSION, (int)esp_reset_reason());
    ui_start();
    ESP_LOGI(TAG, "display up; free internal %u largest %u psram %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    audio_start();
    imu_start();
    ESP_LOGI(TAG, "audio up");
    xTaskCreate(button_task, "button", STACK_BUTTON, NULL, PRIO_BUTTON, NULL);
    s_last_host_ms = now_ms();
    s_ready = true;
    xTaskCreate(life_task, "life", STACK_LIFE, NULL, PRIO_LIFE, NULL);
    send_hello("boot");
}
