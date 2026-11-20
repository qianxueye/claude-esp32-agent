#include "netstore.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "pairrec.h"

static const char *TAG = "netstore";

static SemaphoreHandle_t s_mtx;
static uint32_t s_cur_id; /* id of the stored PAIR record, 0 for none */
static uint32_t s_max_id; /* largest id still in storage */
static uint8_t s_blob[PR_PAIR_BLOB_MAX];

static bool lock(void)
{
    return s_mtx && xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE;
}

static void unlock(void)
{
    mbedtls_platform_zeroize(s_blob, sizeof s_blob);
    xSemaphoreGive(s_mtx);
}

static esp_err_t open_rw(nvs_open_mode_t mode, nvs_handle_t *h)
{
    return nvs_open_from_partition(NET_NVS_PARTITION, NET_NVS_NAMESPACE, mode, h);
}

bool netstore_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    return s_mtx != NULL;
}

/* Reads one record into s_blob; returns its decoded data or NULL. */
static const uint8_t *read_record(nvs_handle_t h, const char *key, size_t max, uint32_t *id, size_t *len)
{
    size_t n = sizeof s_blob;
    esp_err_t e = nvs_get_blob(h, key, s_blob, &n);
    const uint8_t *data = NULL;
    if (e == ESP_OK && !pr_decode(s_blob, n, max, id, &data, len)) {
        ESP_LOGW(TAG, "saved %s record malformed", key);
        data = NULL;
    } else if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "saved %s record unreadable (%s)", key, esp_err_to_name(e));
    }
    return data;
}

size_t netstore_load(uint8_t *text, size_t cap, uint32_t *pair_id, char url[NET_URL_MAX + 1], uint32_t *remote_id)
{
    size_t out = 0;
    *pair_id = 0;
    *remote_id = 0;
    url[0] = 0;
    if (!lock()) {
        return 0;
    }
    nvs_handle_t h;
    if (open_rw(NVS_READONLY, &h) == ESP_OK) {
        uint32_t id = 0;
        size_t len = 0;
        const uint8_t *d = read_record(h, "remote", NET_URL_MAX, &id, &len);
        if (d) {
            s_max_id = id;
        }
        if (d && memchr(d, 0, len) == NULL) {
            memcpy(url, d, len);
            url[len] = 0;
            *remote_id = id;
        }
        d = read_record(h, "pair", NET_PAIR_MAX_BYTES, &id, &len);
        if (d && len <= cap) {
            memcpy(text, d, len);
            out = len;
            *pair_id = id;
            s_cur_id = id;
            s_max_id = id > s_max_id ? id : s_max_id;
        }
        nvs_close(h);
    }
    unlock();
    return out;
}

bool netstore_save_pair(const uint8_t *text, size_t len, uint32_t *pair_id)
{
    if (!lock()) {
        return false;
    }
    uint32_t id = pr_next_id(s_max_id, 0);
    size_t n = pr_encode(id, text, len, NET_PAIR_MAX_BYTES, s_blob, sizeof s_blob);
    nvs_handle_t h;
    esp_err_t e = n ? open_rw(NVS_READWRITE, &h) : ESP_ERR_INVALID_SIZE;
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "pair", s_blob, n);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e == ESP_OK) {
        s_cur_id = id;
        s_max_id = id;
        *pair_id = id;
    } else {
        ESP_LOGW(TAG, "pairing not saved (%s)", esp_err_to_name(e));
    }
    unlock();
    return e == ESP_OK;
}

int netstore_save_remote(uint32_t pair_id, const _Atomic uint32_t *gen, uint32_t want_gen, const char *url)
{
    if (!lock()) {
        return NS_FAILED;
    }
    if (!pr_save_current(s_cur_id, pair_id, atomic_load(gen), want_gen)) {
        unlock();
        return NS_STALE;
    }
    size_t n = pr_encode(pair_id, url, strlen(url), NET_URL_MAX, s_blob, sizeof s_blob);
    nvs_handle_t h;
    esp_err_t e = n ? open_rw(NVS_READWRITE, &h) : ESP_ERR_INVALID_SIZE;
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "remote", s_blob, n);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    unlock();
    return e == ESP_OK ? NS_SAVED : NS_FAILED;
}

bool netstore_forget(void)
{
    if (!lock()) {
        return false;
    }
    nvs_handle_t h;
    esp_err_t e = open_rw(NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_erase_all(h);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    s_cur_id = 0;
    if (e == ESP_OK) {
        s_max_id = 0;
    } else {
        ESP_LOGW(TAG, "pairing not erased (%s)", esp_err_to_name(e));
    }
    unlock();
    return e == ESP_OK;
}
