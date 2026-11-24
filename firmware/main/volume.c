#include "volume.h"
#include "app_config.h"
#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvstore.h"
#include "proto.h"
#include "transport.h"
#include "volstate.h"

static const char *TAG = "volume";

static SemaphoreHandle_t s_mtx;
static volst_t s_st;
static uint32_t s_arms, s_cancels, s_edge, s_saves, s_over[3];

static void apply_locked(void)
{
    audio_set_volume((uint8_t)vs_effective(&s_st));
}

static void report(int v)
{
    uint8_t b = (uint8_t)v;
    transport_send(T_VOLUME, &b, 1);
}

void volume_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    uint8_t raw = 0;
    int result = nvstore_load_u8(VOL_NVS_KEY, &raw);
    bool ok = vs_load(&s_st, result, raw);
    ESP_LOGI(TAG, "volume %d %s", s_st.user, ok ? "loaded" : s_st.ram ? "default, storage off" : "default, none stored");
    apply_locked();
}

void volume_hello_fields(int *vol, bool *set)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *vol = s_st.user;
    *set = s_st.set;
    xSemaphoreGive(s_mtx);
}

void volume_host_set(const uint8_t *payload, int len)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool user_set = vs_host_set(&s_st, payload, len);
    apply_locked();
    int user = s_st.user;
    xSemaphoreGive(s_mtx);
    if (user_set) {
        report(user);
    }
}

void volume_session_reset(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    vs_session_reset(&s_st);
    apply_locked();
    xSemaphoreGive(s_mtx);
}

int volume_knob_arm(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int base = vs_knob_arm(&s_st);
    apply_locked();
    s_arms++;
    xSemaphoreGive(s_mtx);
    return base;
}

void volume_knob_set(int v)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    vs_knob_set(&s_st, v);
    apply_locked();
    xSemaphoreGive(s_mtx);
}

void volume_knob_end(bool cancel)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool changed = vs_knob_end(&s_st, cancel);
    apply_locked();
    s_cancels += cancel;
    int user = s_st.user;
    xSemaphoreGive(s_mtx);
    if (changed) {
        report(user);
    }
}

void volume_count_edge(void)
{
    s_edge++;
}

void volume_count_over(int cells)
{
    if (cells >= 1 && cells <= 3) {
        s_over[cells - 1]++;
    }
}

bool volume_save_due(uint32_t now_ms, bool quiet)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool due = vs_save_due(&s_st, now_ms, nvstore_last_write(), quiet);
    xSemaphoreGive(s_mtx);
    return due;
}

bool volume_poll(uint32_t now_ms, bool quiet)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool due = vs_save_due(&s_st, now_ms, nvstore_last_write(), quiet);
    int v = s_st.user;
    xSemaphoreGive(s_mtx);
    if (!due) {
        return false;
    }
    esp_err_t e = nvstore_save_u8(VOL_NVS_KEY, (uint8_t)v, now_ms);
    bool wrote = e == ESP_OK;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (wrote) {
        vs_saved(&s_st, v);
    }
    int streak = vs_write_streak(&s_st, wrote);
    xSemaphoreGive(s_mtx);
    if (streak == VS_STREAK_START) {
        ESP_LOGW(TAG, "nvs write failed (%s): retrying every %d ms", esp_err_to_name(e), VOL_SAVE_MIN_MS);
    } else if (streak == VS_STREAK_END) {
        ESP_LOGI(TAG, "nvs write succeeded again");
    }
    s_saves += wrote;
    return wrote;
}

void volume_stats(volume_stats_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    out->user = s_st.user;
    out->effective = vs_effective(&s_st);
    out->set = s_st.set;
    xSemaphoreGive(s_mtx);
    out->arms = s_arms;
    out->cancels = s_cancels;
    out->edge = s_edge;
    out->saves = s_saves;
    for (int i = 0; i < 3; i++) {
        out->over[i] = s_over[i];
    }
}
