#include "volume.h"
#include "app_config.h"
#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "proto.h"
#include "transport.h"
#include "volstate.h"

static const char *TAG = "volume";

static SemaphoreHandle_t s_mtx;
static volst_t s_st;
static bool s_nvs_ok;
static uint32_t s_last_write, s_arms, s_cancels, s_edge, s_saves;

static void apply_locked(void)
{
    audio_set_volume((uint8_t)vs_effective(&s_st));
}

static void report(int v)
{
    uint8_t b = (uint8_t)v;
    transport_send(T_VOLUME, &b, 1);
}

static bool nvs_start(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs init %s: erasing the partition once", esp_err_to_name(e));
        e = nvs_flash_erase();
        if (e == ESP_OK) {
            e = nvs_flash_init();
        }
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "nvs unusable (%s): volume kept in RAM", esp_err_to_name(e));
        return false;
    }
    return true;
}

void volume_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_nvs_ok = nvs_start();
    bool found = false;
    uint8_t raw = 0;
    if (s_nvs_ok) {
        nvs_handle_t h;
        esp_err_t e = nvs_open(VOL_NVS_NAMESPACE, NVS_READONLY, &h);
        if (e == ESP_OK) {
            e = nvs_get_u8(h, VOL_NVS_KEY, &raw);
            nvs_close(h);
        }
        found = e == ESP_OK;
        if (!found && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs read failed: %s", esp_err_to_name(e));
        }
    }
    bool ok = vs_load(&s_st, found, raw);
    s_last_write = now_ms() - VOL_SAVE_MIN_MS;
    ESP_LOGI(TAG, "volume %d %s", s_st.user, ok ? "loaded" : "default, none stored");
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

void volume_knob_arm(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    vs_knob_arm(&s_st);
    apply_locked();
    s_arms++;
    xSemaphoreGive(s_mtx);
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

bool volume_poll(uint32_t now_ms, bool quiet)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool due = vs_save_due(&s_st, now_ms, s_last_write, quiet);
    int v = s_st.user;
    xSemaphoreGive(s_mtx);
    if (!due) {
        return false;
    }
    bool wrote = false;
    if (s_nvs_ok) {
        nvs_handle_t h;
        esp_err_t e = nvs_open(VOL_NVS_NAMESPACE, NVS_READWRITE, &h);
        if (e == ESP_OK) {
            e = nvs_set_u8(h, VOL_NVS_KEY, (uint8_t)v);
            if (e == ESP_OK) {
                e = nvs_commit(h);
            }
            nvs_close(h);
        }
        wrote = e == ESP_OK;
        if (!wrote) {
            ESP_LOGW(TAG, "nvs write failed: %s", esp_err_to_name(e));
        }
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (wrote || !s_nvs_ok) {
        vs_saved(&s_st, v);
    }
    xSemaphoreGive(s_mtx);
    s_last_write = now_ms;
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
}
