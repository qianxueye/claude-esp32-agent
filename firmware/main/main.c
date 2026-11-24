#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "audio.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "imu.h"
#include "keydebounce.h"
#include "battstate.h"
#include "bright.h"
#include "nvstore.h"
#include "volknob.h"
#include "volstate.h"
#include "linkstate.h"
#include "net.h"
#include "netcfg.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "proto.h"
#include "transport.h"
#include "ui.h"
#include "volume.h"

static const char *TAG = "app";

static volatile dev_state_t s_state = ST_IDLE;
static volatile uint32_t s_last_host_ms;
static volatile bool s_ready;
static volatile bool s_selftest_busy;
static QueueHandle_t s_snap_queue;
static SemaphoreHandle_t s_state_mtx;
static volatile uint32_t s_pwr_read_errors;

static void send_hello(const char *reason)
{
    char json[HELLO_JSON_BYTES];
    int vol;
    bool vol_set;
    volume_hello_fields(&vol, &vol_set);
    int route = transport_route();
    int n = snprintf(json, sizeof(json),
                     "{\"proto\":%d,\"fw\":\"%s\",\"rate\":%d,\"reason\":\"%s\",\"link\":\"%s\",\"paired\":%s,\"store\":\"%s\",\"vol\":%d,"
                     "\"vol_set\":%s}",
                     PROTO_VERSION, FW_VERSION, AUDIO_SAMPLE_RATE, reason, link_name(route), net_paired() ? "true" : "false",
                     net_store_known() ? "ok" : "error", vol, vol_set ? "true" : "false");
    transport_send_to(route, T_HELLO, json, n);
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
    v = json_value(json, "imu_dump");
    if (v) {
        imu_set_dump(atoi(v));
    }
    v = json_value(json, "disp_scale");
    if (v) {
        ui_set_disp_scale(atoi(v));
    }
    v = json_value(json, "disp_force");
    if (v) {
        ui_set_disp_force(strncmp(v, "null", 4) == 0 ? -1 : atoi(v));
    }
    v = json_value(json, "orbit");
    if (v) {
        ui_set_orbit(strncmp(v, "null", 4) == 0 ? -1 : atoi(v));
    }
    v = json_value(json, "arc");
    if (v) {
        ui_set_arc(strncmp(v, "null", 4) == 0 ? -1 : atoi(v));
    }
    v = json_value(json, "brt_arc");
    if (v) {
        ui_set_brt_arc(strncmp(v, "null", 4) == 0 ? -1 : atoi(v));
    }
    v = json_value(json, "arc_over");
    if (v) {
        ui_set_arc_over(strncmp(v, "null", 4) == 0 ? 0 : atoi(v));
    }
    v = json_value(json, "fill");
    if (v) {
        ui_set_fill(strncmp(v, "null", 4) == 0 ? -1 : atoi(v));
    }
    v = json_value(json, "brt");
    if (v) {
        ui_set_brightness_pin(strncmp(v, "null", 4) == 0 ? -1 : atoi(v));
    }
    v = json_value(json, "brt_set");
    if (v) {
        ui_set_brightness(atoi(v));
    }
}

static void set_state(dev_state_t s)
{
    if (s == ST_OFFLINE && s_state != ST_OFFLINE) {
        volume_session_reset();
    }
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

static unsigned task_free_bytes(const char *name)
{
    TaskHandle_t t = xTaskGetHandle(name);
    return t ? (unsigned)uxTaskGetStackHighWaterMark(t) : 0;
}

static bool accept_frame(const proto_frame_t *f)
{
    if (f->type == T_HOST_HELLO && !host_hello_proto_ok(f)) {
        ESP_LOGW(TAG, "HOST_HELLO ignored: proto mismatch (device speaks %d)", PROTO_VERSION);
        return false;
    }
    return true;
}

static void send_pairing_result(uint8_t type, const char *err)
{
    char json[64];
    size_t n = netcfg_result_json(err, json, sizeof json);
    transport_send_usb(type, json, n);
}

static void on_frame(const proto_frame_t *f, bool live)
{
    if (!s_ready) {
        return;
    }
    if (live) {
        s_last_host_ms = now_ms();
    }
    lv_draw_buf_t *snap = f->type == T_SNAPSHOT ? ui_snapshot_take() : NULL;
    const char *reply = "reply";
    if (live && s_state == ST_OFFLINE && f->type != T_SNAPSHOT) {
        set_state(ST_IDLE);
        reply = "resume";
        if (f->type != T_HOST_HELLO) {
            send_hello("resume");
        }
    }
    switch (f->type) {
    case T_HOST_HELLO:
        volume_session_reset();
        send_hello(reply);
        {
            char json[NET_STATUS_JSON_BYTES];
            size_t n = net_status_json(json, sizeof json);
            transport_send(T_WIFI_STATUS, json, n);
        }
        break;
    case T_SET_STATE:
        if (f->len == 1 && f->payload[0] < ST_COUNT) {
            set_state((dev_state_t)f->payload[0]);
        }
        break;
    case T_SET_VOLUME:
        if (f->len == 1 || f->len == 2) {
            volume_host_set(f->payload, f->len);
        }
        break;
    case T_PAIR:
        send_pairing_result(T_PAIR_RESULT, net_pair(f->payload, f->len));
        break;
    case T_FORGET:
        send_pairing_result(T_FORGET_RESULT, net_forget());
        break;
    case T_SET_REMOTE:
        net_set_remote(f->payload, f->len);
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

static i2c_master_dev_handle_t s_pmu;
static batt_state_t s_batt;

static void pmu_open(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = PMU_I2C_ADDR, .scl_speed_hz = PMU_I2C_HZ};
    if (bus == NULL || i2c_master_bus_add_device(bus, &cfg, &s_pmu) != ESP_OK) {
        s_pmu = NULL;
        ESP_LOGW(TAG, "pmu: no device");
    }
}

/* Read-only: the firmware never writes a PMU register. */
static esp_err_t pmu_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return s_pmu == NULL ? ESP_ERR_INVALID_STATE : i2c_master_transmit_receive(s_pmu, &reg, 1, buf, n, PMU_I2C_TIMEOUT_MS);
}

static void batt_poll(uint32_t t)
{
    uint8_t r00 = 0, r01 = 0, pct = 0, vh = 0, vl = 0;
    esp_err_t err = pmu_read(0x00, &r00, 1);
    const bool r00_ok = err == ESP_OK;
    if (err == ESP_OK) {
        err = pmu_read(0x01, &r01, 1);
    }
    if (err == ESP_OK) {
        err = pmu_read(0xA4, &pct, 1);
    }
    int mv = -1;
    if (err == ESP_OK && batt_present(r00) && pmu_read(0x34, &vh, 1) == ESP_OK && pmu_read(0x35, &vl, 1) == ESP_OK) {
        mv = ((vh & 0x1F) << 8) | vl;
    }
    batt_in_t in = {.ok = err == ESP_OK, .reg00_ok = r00_ok, .reg00 = r00, .reg01 = r01, .pct = pct, .now_ms = t};
    batt_out_t o = batt_step(&s_batt, &in);
    ui_set_battery(BATT_PACK(o.look, o.bars));
    if (o.wake) {
        ui_wake(DP_CAUSE_POWER);
    }
    if (o.log_stale) {
        ESP_LOGW(TAG, "batt: stale");
    }
    if (o.log_fail) {
        ESP_LOGW(TAG, "batt: read failed err=%d", (int)err);
    }
    if (o.log) {
        ESP_LOGI(TAG, BATT_LINE_FMT, (int)pct, (int)batt_present(r00), (int)batt_vbus(r00), batt_dir(r01), batt_chg(r01), mv, o.bars, batt_look_name(o.look));
    }
}

static void life_task(void *arg)
{
    uint32_t last_stats = now_ms(), last_batt = last_stats - BATT_POLL_MS;
    bool saved_logged = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LIFE_PERIOD_MS));
        uint32_t last_host = s_last_host_ms;
        uint32_t t = now_ms();
        if (s_ready && s_state != ST_OFFLINE && t - last_host >= PROTO_HOST_SILENCE_TIMEOUT_MS) {
            set_state(ST_OFFLINE);
        }
        bool quiet = !audio_pa_active() && s_state != ST_LISTENING;
        const char *last_key = nvstore_last_key();
        int last = !last_key ? VS_PICK_NONE : strcmp(last_key, VOL_NVS_KEY) == 0 ? VS_PICK_A : VS_PICK_B;
        int pick = vs_save_pick(volume_save_due(t, quiet), bright_save_due(t, quiet), last);
        if (pick == VS_PICK_B) {
            bright_poll(t, quiet);
        } else if (pick == VS_PICK_A && volume_poll(t, quiet) && !saved_logged) {
            saved_logged = true;
            ESP_LOGI(TAG, "volume saved; life stack high-water %u words", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        if (t - last_batt >= BATT_POLL_MS) {
            last_batt = t;
            batt_poll(t);
        }
        if (t - last_stats >= STATS_LOG_PERIOD_MS) {
            last_stats = t;
            transport_stats_t st;
            transport_get_stats(&st);
            audio_counters_t ac = audio_counters();
            uint32_t wk[DP_CAUSE_N];
            ui_wake_counts(wk);
            ESP_LOGI(TAG, "stats rx=%u/%uB crc=%u skip=%u tx=%u/%uB drop=%u spkq=%u spkovf=%u pa=%d under=%u slow=%u micgap=%u micdrop=%u", (unsigned)st.rx_frames,
                     (unsigned)st.rx_bytes, (unsigned)st.rx_crc_errors, (unsigned)st.rx_bytes_skipped,
                     (unsigned)st.tx_frames, (unsigned)st.tx_bytes, (unsigned)st.tx_dropped,
                     (unsigned)audio_spk_queued(), (unsigned)audio_spk_overflow_frames(), (int)audio_pa_is_on(),
                     (unsigned)ac.spk_underruns, (unsigned)ac.spk_slow_writes, (unsigned)ac.mic_gaps, (unsigned)ac.mic_dropped);
            ESP_LOGI(TAG, "stack free B: rx=%u net=%u sys_evt=%u wifi=%u tiT=%u; net drop=%u", (unsigned)transport_rx_stack_hwm(),
                     (unsigned)net_stack_hwm(), task_free_bytes("sys_evt"), task_free_bytes("wifi"), task_free_bytes("tiT"),
                     (unsigned)net_tx_dropped());
            ESP_LOGI(TAG, "wakes touch=%u button=%u state=%u motion=%u power=%u pwr_err=%u", (unsigned)wk[DP_CAUSE_TOUCH], (unsigned)wk[DP_CAUSE_BUTTON],
                     (unsigned)wk[DP_CAUSE_STATE], (unsigned)wk[DP_CAUSE_MOTION], (unsigned)wk[DP_CAUSE_POWER], (unsigned)s_pwr_read_errors);
            volume_stats_t vs;
            volume_stats(&vs);
            ESP_LOGI(TAG, VOLUME_LINE_FMT, vs.user, vs.effective, (int)vs.set, (unsigned)vs.arms, (unsigned)vs.cancels, (unsigned)vs.edge, (unsigned)vs.saves,
                     vk_ov_log(vs.over[0]), vk_ov_log(vs.over[1]), vk_ov_log(vs.over[2]));
            bright_stats_t bs;
            bright_stats(&bs);
            ESP_LOGI(TAG, BRIGHT_LINE_FMT, bs.user, bs.effective, ui_panel_level(), (int)bs.set, (unsigned)bs.arms, (unsigned)bs.rim, (unsigned)bs.cancels,
                     (unsigned)bs.slides, (unsigned)bs.stops, (unsigned)bs.saves, vk_ov_log(bs.over[0]), vk_ov_log(bs.over[1]), vk_ov_log(bs.over[2]));
        }
    }
}

static void key_press(const char *name)
{
    ESP_LOGI(TAG, "button: press key=%s", name);
    ui_wake(DP_CAUSE_BUTTON);
    uint8_t code = 3;
    transport_send(T_INPUT, &code, 1);
}

static void log_pmu_regs(void)
{
    uint8_t r22 = 0, r27 = 0;
    esp_err_t e1 = pmu_read(0x22, &r22, 1);
    esp_err_t e2 = pmu_read(0x27, &r27, 1);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGW(TAG, "pmu: read failed %d %d", (int)e1, (int)e2);
        return;
    }
    ESP_LOGI(TAG, "pmu: reg22=0x%02x reg27=0x%02x", r22, r27);
}

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    log_pmu_regs();
    esp_io_expander_handle_t exp = bsp_io_expander_init();
    keydeb_t boot, pwr;
    keydeb_init(&boot);
    keydeb_init(&pwr);
    bool logged = false;
    uint32_t logged_byte = 0, logged_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_BUTTON_POLL_MS));
        if (keydeb_poll(&boot, gpio_get_level(BOOT_BUTTON_GPIO) == 0, BOOT_BUTTON_DEBOUNCE_POLLS)) {
            key_press("boot");
        }
        uint32_t in = 0;
        bool pwr_down = false;
        bool pwr_ok = exp != NULL && esp_io_expander_get_level(exp, 0xFF, &in) == ESP_OK;
        if (pwr_ok) {
            pwr_down = (in >> PWR_KEY_EXIO) & 1;
            uint32_t t = now_ms();
            if (!logged || (in != logged_byte && t - logged_ms >= PWR_LOG_MIN_MS)) {
                ESP_LOGI(TAG, "tca9554 in=0x%02x", (unsigned)in);
                logged = true;
                logged_byte = in;
                logged_ms = t;
            }
        } else {
            s_pwr_read_errors++;
        }
        if (keydeb_poll_read(&pwr, pwr_ok, pwr_down, BOOT_BUTTON_DEBOUNCE_POLLS)) {
            key_press("pwr");
        }
    }
}

void app_main(void)
{
    s_state_mtx = xSemaphoreCreateMutex();
    s_snap_queue = xQueueCreate(SNAPSHOT_QUEUE_DEPTH, sizeof(lv_draw_buf_t *));
    xTaskCreate(snapshot_task, "snap", STACK_WORKER, NULL, PRIO_WORKER, NULL);
    s_last_host_ms = now_ms();
    transport_start(on_frame, accept_frame);
    ESP_LOGI(TAG, "boot fw=%s reset_reason=%d", FW_VERSION, (int)esp_reset_reason());
    nvstore_init();
    volume_init();
    bright_init();
    ui_start();
    ESP_LOGI(TAG, "display up; free internal %u largest %u psram %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    audio_start();
    imu_start();
    ESP_LOGI(TAG, "audio up");
    net_init();
    pmu_open();
    xTaskCreate(button_task, "button", STACK_BUTTON, NULL, PRIO_BUTTON, NULL);
    s_last_host_ms = now_ms();
    s_ready = true;
    xTaskCreate(life_task, "life", STACK_LIFE, NULL, PRIO_LIFE, NULL);
    send_hello("boot");
    net_start();
}
