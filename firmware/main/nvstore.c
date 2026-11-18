#include "nvstore.h"
#include <stdbool.h>
#include "app_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "transport.h"
#include "volstate.h"

static const char *TAG = "nvstore";

static bool s_on;
static uint32_t s_last_write;

void nvstore_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs init %s: erasing the partition once", esp_err_to_name(e));
        e = nvs_flash_erase();
        if (e == ESP_OK) {
            e = nvs_flash_init();
        }
    }
    s_on = e == ESP_OK;
    if (!s_on) {
        ESP_LOGW(TAG, "nvs unusable (%s): storage off this boot", esp_err_to_name(e));
    }
    s_last_write = now_ms() - VOL_SAVE_MIN_MS;
}

int nvstore_load_u8(const char *key, uint8_t *raw)
{
    if (!s_on) {
        return VS_LOAD_ERROR;
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open(VOL_NVS_NAMESPACE, NVS_READONLY, &h);
    if (e == ESP_OK) {
        e = nvs_get_u8(h, key, raw);
        nvs_close(h);
    }
    int result = e == ESP_OK ? VS_LOAD_FOUND : e == ESP_ERR_NVS_NOT_FOUND ? VS_LOAD_ABSENT : VS_LOAD_ERROR;
    if (result == VS_LOAD_ERROR) {
        ESP_LOGW(TAG, "nvs read %s failed (%s): kept in RAM", key, esp_err_to_name(e));
    }
    return result;
}

esp_err_t nvstore_save_u8(const char *key, uint8_t v, uint32_t now_ms)
{
    s_last_write = now_ms;
    if (!s_on) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open(VOL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_u8(h, key, v);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    return e;
}

uint32_t nvstore_last_write(void)
{
    return s_last_write;
}
