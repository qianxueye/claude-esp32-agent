#include "netstore.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pairrec.h"

static const char *TAG = "netstore";

static SemaphoreHandle_t s_mtx;
static bool s_known; /* the content is known: writes are allowed this boot */
static uint32_t s_cur_id; /* id of the stored PAIR record, 0 for none */
static size_t s_pair_len;
static uint32_t s_remote_id; /* the stored SET_REMOTE record, id 0 for none */
static uint8_t s_remote_raw[NET_URL_MAX];
static size_t s_remote_len;
static uint32_t s_max_id; /* largest id still in storage */
static uint8_t s_blob[PR_PAIR_BLOB_MAX];
static uint8_t s_back[PR_PAIR_BLOB_MAX];

/* Warnings found under the lock, logged after it is released because logging can take other locks. */
typedef struct {
    const char *what, *key;
    esp_err_t err;
} note_t;
static note_t s_notes[2];
static int s_n_notes;

static void note(const char *what, const char *key, esp_err_t err)
{
    if (s_n_notes < 2) {
        s_notes[s_n_notes++] = (note_t){what, key, err};
    }
}

static bool lock(void)
{
    return s_mtx && xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE;
}

static void unlock(void)
{
    note_t notes[2];
    int n = s_n_notes;
    memcpy(notes, s_notes, sizeof notes);
    s_n_notes = 0;
    mbedtls_platform_zeroize(s_blob, sizeof s_blob);
    mbedtls_platform_zeroize(s_back, sizeof s_back);
    xSemaphoreGive(s_mtx);
    for (int i = 0; i < n; i++) {
        ESP_LOGW(TAG, "%s %s (%s)", notes[i].what, notes[i].key, esp_err_to_name(notes[i].err));
    }
}

static esp_err_t open_rw(nvs_open_mode_t mode, nvs_handle_t *h)
{
    if (!s_known) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    return nvs_open_from_partition(NET_NVS_PARTITION, NET_NVS_NAMESPACE, mode, h);
}

_Static_assert(PR_ERR_NVS_NO_FREE_PAGES == ESP_ERR_NVS_NO_FREE_PAGES, "nvs.h code");
_Static_assert(PR_ERR_NVS_NEW_VERSION_FOUND == ESP_ERR_NVS_NEW_VERSION_FOUND, "nvs.h code");
_Static_assert(PR_ERR_NVS_NOT_FOUND == ESP_ERR_NVS_NOT_FOUND, "nvs.h code");
_Static_assert(PR_ERR_NVS_REMOVE_FAILED == ESP_ERR_NVS_REMOVE_FAILED, "nvs.h code");

/* Initialises the partition; only a layout NVS cannot use is erased, any other error leaves it untouched. */
static esp_err_t start_partition(void)
{
    esp_err_t e = nvs_flash_init_partition(NET_NVS_PARTITION);
    if (pr_boot_action(e) == PR_BOOT_REFORMAT) {
        ESP_LOGW(TAG, "pairing partition unreadable (%s): erased", esp_err_to_name(e));
        e = nvs_flash_erase_partition(NET_NVS_PARTITION);
        if (e == ESP_OK) {
            e = nvs_flash_init_partition(NET_NVS_PARTITION);
        }
    }
    s_known = e == ESP_OK;
    if (!s_known) {
        ESP_LOGE(TAG, "pairing partition unusable (%s)", esp_err_to_name(e));
    }
    return e;
}

bool netstore_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    return s_mtx && start_partition() == ESP_OK;
}

/* Reads one record into s_blob and classifies it; v points into s_blob, and a failure sets *why. */
static int read_view(nvs_handle_t h, const char *key, size_t max, pr_view_t *v, esp_err_t *why)
{
    size_t n = sizeof s_blob;
    esp_err_t e = nvs_get_blob(h, key, s_blob, &n);
    bool decoded = e == ESP_OK && pr_decode(s_blob, n, max, &v->id, &v->data, &v->len);
    int c = pr_read_class(e, decoded);
    v->present = c == PR_READ_FOUND;
    if (c == PR_READ_FAILED) {
        *why = e == ESP_OK ? ESP_ERR_INVALID_SIZE : e;
        note(e == ESP_OK ? "malformed record" : "unreadable record", key, *why);
    }
    return c;
}

void netstore_load(uint8_t *text, size_t cap, char url[NET_URL_MAX + 1], netstore_loaded_t *out)
{
    *out = (netstore_loaded_t){0, 0, 0, PR_READ_FAILED, PR_READ_FAILED};
    url[0] = 0;
    if (!lock()) {
        return;
    }
    pr_view_t pv = {false, 0, NULL, 0}, rv = {false, 0, NULL, 0};
    nvs_handle_t h;
    esp_err_t e = s_known ? open_rw(NVS_READONLY, &h) : ESP_ERR_NVS_NOT_INITIALIZED;
    esp_err_t why = e;
    if (e == ESP_OK) {
        out->remote_class = read_view(h, "remote", NET_URL_MAX, &rv, &why);
        if (out->remote_class == PR_READ_FOUND) {
            memcpy(s_remote_raw, rv.data, rv.len);
        }
        out->pair_class = read_view(h, "pair", NET_PAIR_MAX_BYTES, &pv, &why);
        if (pv.present && pv.len > cap) {
            pv.present = false;
            out->pair_class = PR_READ_FAILED;
        }
        if (pv.present) {
            memcpy(text, pv.data, pv.len);
            out->len = pv.len;
            out->pair_id = pv.id;
        }
        nvs_close(h);
    } else if (s_known) {
        out->pair_class = out->remote_class = pr_read_class(e, false);
    }
    bool was_known = s_known;
    s_known = s_known && pr_load_result(out->pair_class, out->remote_class, true).known;
    if (was_known && !s_known) {
        note("store content unknown this boot:", "load", why == ESP_OK ? ESP_ERR_INVALID_SIZE : why);
    }
    s_remote_id = rv.present ? rv.id : 0;
    s_remote_len = rv.present ? rv.len : 0;
    s_cur_id = pv.present ? pv.id : 0;
    s_pair_len = pv.present ? pv.len : 0;
    s_max_id = s_cur_id > s_remote_id ? s_cur_id : s_remote_id;
    if (rv.present && memchr(s_remote_raw, 0, rv.len) == NULL) {
        memcpy(url, s_remote_raw, rv.len);
        url[rv.len] = 0;
        out->remote_id = rv.id;
    }
    unlock();
}

bool netstore_known(void)
{
    if (!lock()) {
        return false;
    }
    bool k = s_known;
    unlock();
    return k;
}

/* Reads key back after a fresh init of the partition, which is what the next boot loads. */
static bool read_back(const char *key, size_t max, pr_view_t *got)
{
    got->present = false;
    nvs_flash_deinit_partition(NET_NVS_PARTITION);
    if (nvs_flash_init_partition(NET_NVS_PARTITION) != ESP_OK) {
        s_known = false;
        return false;
    }
    nvs_handle_t h;
    esp_err_t e = open_rw(NVS_READONLY, &h);
    if (e != ESP_OK) {
        return e == ESP_ERR_NVS_NOT_FOUND;
    }
    size_t n = sizeof s_back;
    e = nvs_get_blob(h, key, s_back, &n);
    nvs_close(h);
    if (e != ESP_OK) {
        return e == ESP_ERR_NVS_NOT_FOUND;
    }
    got->present = true;
    return pr_decode(s_back, n, max, &got->id, &got->data, &got->len);
}

/* Writes the record in s_blob; after any error the outcome follows the error and what the store holds. */
static int write_record(const char *key, size_t n, size_t max, pr_view_t want, pr_view_t prev)
{
    nvs_handle_t h;
    esp_err_t e = n ? open_rw(NVS_READWRITE, &h) : ESP_ERR_INVALID_SIZE;
    if (e == ESP_OK) {
        e = nvs_set_blob(h, key, s_blob, n);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e == ESP_OK) {
        return PR_WRITE_DONE;
    }
    if (!n || !s_known) {
        note("not saved:", key, e);
        return PR_WRITE_KEPT;
    }
    pr_view_t got = {false, 0, NULL, 0};
    bool read_ok = read_back(key, max, &got);
    int out = pr_write_outcome(e, read_ok, got, want, prev);
    if (out == PR_WRITE_UNKNOWN) {
        s_known = false;
    }
    note(out == PR_WRITE_DONE ? "saved despite an error:" : out == PR_WRITE_KEPT ? "not saved:" : "store content unknown after a failed write:",
         key, e);
    return out;
}

int netstore_save_pair(const uint8_t *text, size_t len, uint32_t *pair_id)
{
    if (!lock()) {
        return NS_FAILED;
    }
    uint32_t id = pr_next_id(s_max_id, 0);
    size_t n = pr_encode(id, text, len, NET_PAIR_MAX_BYTES, s_blob, sizeof s_blob);
    pr_view_t want = {true, id, text, len}, prev = {s_cur_id != 0, s_cur_id, NULL, s_pair_len};
    int out = write_record("pair", n, NET_PAIR_MAX_BYTES, want, prev);
    if (out == PR_WRITE_DONE) {
        s_cur_id = id;
        s_pair_len = len;
        s_max_id = id;
        *pair_id = id;
    }
    unlock();
    return out == PR_WRITE_DONE ? NS_SAVED : out == PR_WRITE_KEPT ? NS_FAILED : NS_UNKNOWN;
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
    size_t len = strlen(url);
    size_t n = pr_encode(pair_id, url, len, NET_URL_MAX, s_blob, sizeof s_blob);
    pr_view_t want = {true, pair_id, (const uint8_t *)url, len}, prev = {s_remote_id != 0, s_remote_id, s_remote_raw, s_remote_len};
    int out = write_record("remote", n, NET_URL_MAX, want, prev);
    if (out == PR_WRITE_DONE) {
        s_remote_id = pair_id;
        s_remote_len = len;
        memcpy(s_remote_raw, url, len);
    }
    unlock();
    return out == PR_WRITE_DONE ? NS_SAVED : out == PR_WRITE_KEPT ? NS_FAILED : NS_UNKNOWN;
}

bool netstore_forget(void)
{
    if (!lock()) {
        return false;
    }
    /* Runs whatever the store state: erase deinitialises an initialised partition first and works on one that never initialised. */
    esp_err_t e = nvs_flash_erase_partition(NET_NVS_PARTITION);
    esp_err_t ie = e == ESP_OK ? nvs_flash_init_partition(NET_NVS_PARTITION) : ESP_ERR_INVALID_STATE;
    s_known = e == ESP_OK && ie == ESP_OK;
    s_cur_id = 0;
    if (e == ESP_OK) {
        s_max_id = 0;
        s_pair_len = 0;
        s_remote_id = 0;
        s_remote_len = 0;
    } else {
        note("not erased:", "pairing", e);
    }
    if (e == ESP_OK && ie != ESP_OK) {
        note("not initialised after erase:", "pairing", ie);
    }
    bool ok = s_known;
    unlock();
    return ok;
}
