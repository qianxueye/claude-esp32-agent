#include "netstore.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "netstore";

size_t netstore_load_pair(uint8_t *buf, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(NET_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    size_t len = cap;
    esp_err_t e = nvs_get_blob(h, "pair", buf, &len);
    nvs_close(h);
    if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "saved pairing unreadable (%s)", esp_err_to_name(e));
    }
    return e == ESP_OK ? len : 0;
}

bool netstore_load_remote(char url[NET_URL_MAX + 1])
{
    nvs_handle_t h;
    url[0] = 0;
    if (nvs_open(NET_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = NET_URL_MAX + 1;
    esp_err_t e = nvs_get_str(h, "remote", url, &len);
    nvs_close(h);
    if (e != ESP_OK) {
        url[0] = 0;
    }
    return e == ESP_OK;
}

bool netstore_save_pair(const uint8_t *text, size_t len, const char *remote)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_erase_key(h, "remote");
        if (e == ESP_ERR_NVS_NOT_FOUND) {
            e = ESP_OK;
        }
        if (e == ESP_OK) {
            e = nvs_set_blob(h, "pair", text, len);
        }
        if (e == ESP_OK && remote) {
            e = nvs_set_str(h, "remote", remote);
        }
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "pairing not saved (%s)", esp_err_to_name(e));
    }
    return e == ESP_OK;
}

bool netstore_save_remote(const char *url)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_str(h, "remote", url);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "remote URL not saved (%s)", esp_err_to_name(e));
    }
    return e == ESP_OK;
}

bool netstore_forget(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_erase_all(h);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "pairing not erased (%s)", esp_err_to_name(e));
    }
    return e == ESP_OK;
}
