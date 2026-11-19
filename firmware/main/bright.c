#include "bright.h"
#include "app_config.h"
#include "brtmap.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvstore.h"
#include "volstate.h"

static const char *TAG = "bright";

/* Leaf lock: taken after the display lock, never before it. */
static SemaphoreHandle_t s_mtx;
static volst_t s_st;
static uint32_t s_arms, s_rim, s_cancels, s_slides, s_stops, s_saves;

void bright_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    uint8_t raw = 0;
    int result = nvstore_load_u8(BRT_NVS_KEY, &raw);
    bool ok = vs_load_range(&s_st, result, raw, BRT_MIN_PERCENT, 100, DISP_ACTIVE_PERCENT);
    s_st.user = brt_snap(s_st.user);
    ESP_LOGI(TAG, "bright %d %s", s_st.user, ok ? "loaded" : s_st.ram ? "default, storage off" : "default, none stored");
}

int bright_effective(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int v = vs_effective(&s_st);
    xSemaphoreGive(s_mtx);
    return v;
}

int bright_swipe_arm(bool rim)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int base = vs_knob_arm(&s_st);
    s_arms++;
    s_rim += rim;
    xSemaphoreGive(s_mtx);
    return base;
}

void bright_swipe_set(int pct)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    vs_knob_set(&s_st, brt_snap(pct));
    xSemaphoreGive(s_mtx);
}

void bright_swipe_end(bool cancel)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    vs_swipe_end(&s_st, cancel);
    s_cancels += cancel;
    xSemaphoreGive(s_mtx);
}

void bright_test_pin(int pct)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (pct < 0) {
        vs_session_reset(&s_st);
    } else {
        uint8_t b = (uint8_t)brt_snap(pct);
        vs_host_set(&s_st, &b, 1);
    }
    xSemaphoreGive(s_mtx);
}

void bright_set(int pct)
{
    const uint8_t p[2] = {(uint8_t)brt_snap(pct), VOL_MODE_PERSIST};
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    vs_host_set(&s_st, p, 2);
    xSemaphoreGive(s_mtx);
}

void bright_count_slide(void)
{
    s_slides++;
}

void bright_count_stop(void)
{
    s_stops++;
}

bool bright_save_due(uint32_t now_ms, bool quiet)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool due = vs_save_due(&s_st, now_ms, nvstore_last_write(), quiet);
    xSemaphoreGive(s_mtx);
    return due;
}

bool bright_poll(uint32_t now_ms, bool quiet)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool due = vs_save_due(&s_st, now_ms, nvstore_last_write(), quiet);
    int v = s_st.user;
    xSemaphoreGive(s_mtx);
    if (!due) {
        return false;
    }
    esp_err_t e = nvstore_save_u8(BRT_NVS_KEY, (uint8_t)v, now_ms);
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

void bright_stats(bright_stats_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    out->user = s_st.user;
    out->effective = vs_effective(&s_st);
    out->set = s_st.set;
    xSemaphoreGive(s_mtx);
    out->arms = s_arms;
    out->rim = s_rim;
    out->cancels = s_cancels;
    out->slides = s_slides;
    out->stops = s_stops;
    out->saves = s_saves;
}
