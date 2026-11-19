#include "skin.h"
#include <stdbool.h>
#include <string.h>
#include "app_config.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "skin";

static ap_pack_t s_pack;
static int s_status = AP_NO_PARTITION;
static volatile uint32_t s_verify_ms;
static int64_t s_verify_t0;

static void set_status(ap_status_t st)
{
    __atomic_store_n(&s_status, (int)st, __ATOMIC_RELEASE);
}

ap_status_t skin_status(void)
{
    return (ap_status_t)__atomic_load_n(&s_status, __ATOMIC_ACQUIRE);
}

const ap_pack_t *skin_pack(void)
{
    return skin_status() == AP_OK ? &s_pack : NULL;
}

void skin_fail(ap_status_t st)
{
    set_status(st);
    ESP_LOGW(TAG, "pack=%d", (int)st);
}

uint32_t skin_verify_ms(void)
{
    return s_verify_ms;
}

static void verify(void)
{
    bool ok = true;
    for (int c = 0; ok && c < s_pack.clip_count; c++) {
        for (int k = 0; ok && k < s_pack.clip[c].frames; k++) {
            ap_frame_t f;
            ok = ap_frame(&s_pack, c, k, &f) && ap_frame_crc_ok(&f);
        }
    }
    const int64_t us = esp_timer_get_time() - s_verify_t0;
    s_verify_ms = us > 0 ? (uint32_t)(us / 1000) : 0;
    set_status(ok ? AP_OK : AP_BAD_PAYLOAD);
    ESP_LOGI(TAG, "pack=%d verify_ms=%u bytes=%u clips=%u", (int)skin_status(), (unsigned)s_verify_ms, (unsigned)s_pack.total_len, (unsigned)s_pack.clip_count);
}

static void verify_task(void *arg)
{
    verify();
    vTaskDelete(NULL);
}

void skin_init(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, ANIM_PARTITION_LABEL);
    if (!part) {
        set_status(AP_NO_PARTITION);
        ESP_LOGI(TAG, "pack=%d no partition", AP_NO_PARTITION);
        return;
    }
    uint8_t hdr[AP_HEADER_BYTES];
    esp_err_t err = esp_partition_read(part, 0, hdr, sizeof(hdr));
    if (err != ESP_OK) {
        set_status(AP_BAD_HEADER);
        ESP_LOGW(TAG, "pack=%d read err=%d", AP_BAD_HEADER, (int)err);
        return;
    }
    bool erased = true;
    for (size_t i = 0; i < sizeof(hdr); i++) {
        erased = erased && hdr[i] == 0xFF;
    }
    if (erased) {
        set_status(AP_EMPTY);
        ESP_LOGI(TAG, "pack=%d empty", AP_EMPTY);
        return;
    }
    const uint32_t total = (uint32_t)hdr[16] | ((uint32_t)hdr[17] << 8) | ((uint32_t)hdr[18] << 16) | ((uint32_t)hdr[19] << 24);
    if (total < AP_HEADER_BYTES || total > part->size) {
        set_status(AP_BAD_HEADER);
        ESP_LOGW(TAG, "pack=%d total_len=%u partition=%u", AP_BAD_HEADER, (unsigned)total, (unsigned)part->size);
        return;
    }
    const void *map = NULL;
    esp_partition_mmap_handle_t handle;
    err = esp_partition_mmap(part, 0, total, ESP_PARTITION_MMAP_DATA, &map, &handle);
    if (err != ESP_OK) {
        set_status(AP_BAD_HEADER);
        ESP_LOGW(TAG, "pack=%d mmap err=%d", AP_BAD_HEADER, (int)err);
        return;
    }
    const ap_status_t st = ap_parse((const uint8_t *)map, total, total, &s_pack);
    if (st != AP_OK) {
        esp_partition_munmap(handle);
        set_status(st);
        ESP_LOGW(TAG, "pack=%d parse", (int)st);
        return;
    }
    s_verify_t0 = esp_timer_get_time();
    set_status(AP_VERIFYING);
    if (xTaskCreate(verify_task, "skinverify", STACK_SKIN_VERIFY, NULL, PRIO_SKIN_VERIFY, NULL) != pdPASS) {
        verify();
    }
}
