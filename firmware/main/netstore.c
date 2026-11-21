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
static bool s_usable; /* the partition is initialised */
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
    if (!s_usable) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    return nvs_open_from_partition(NET_NVS_PARTITION, NET_NVS_NAMESPACE, mode, h);
}

_Static_assert(PR_ERR_NVS_NO_FREE_PAGES == ESP_ERR_NVS_NO_FREE_PAGES, "nvs.h code");
_Static_assert(PR_ERR_NVS_NEW_VERSION_FOUND == ESP_ERR_NVS_NEW_VERSION_FOUND, "nvs.h code");
_Static_assert(PR_ERR_NVS_NOT_FOUND == ESP_ERR_NVS_NOT_FOUND, "nvs.h code");

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
    s_usable = e == ESP_OK;
    if (!s_usable) {
        ESP_LOGE(TAG, "pairing partition unusable (%s)", esp_err_to_name(e));
    }
    return e;
}

bool netstore_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    return s_mtx && start_partition() == ESP_OK;
}

/* Reads one record into s_blob and classifies it; v points into s_blob. */
static int read_view(nvs_handle_t h, const char *key, size_t max, pr_view_t *v)
{
    size_t n = sizeof s_blob;
    esp_err_t e = nvs_get_blob(h, key, s_blob, &n);
    bool decoded = e == ESP_OK && pr_decode(s_blob, n, max, &v->id, &v->data, &v->len);
    int c = pr_read_class(e, decoded);
    v->present = c == PR_READ_FOUND;
    if (c == PR_READ_FAILED) {
        note(e == ESP_OK ? "malformed record" : "unreadable record", key, e == ESP_OK ? ESP_ERR_INVALID_SIZE : e);
    }
    return c;
}

size_t netstore_load(uint8_t *text, size_t cap, uint32_t *pair_id, char url[NET_URL_MAX + 1], uint32_t *remote_id)
{
    *pair_id = 0;
    *remote_id = 0;
    url[0] = 0;
    if (!lock()) {
        return 0;
    }
    size_t out = 0;
    pr_view_t pv = {false, 0, NULL, 0}, rv = {false, 0, NULL, 0};
    int pc = PR_READ_FAILED, rc = PR_READ_FAILED;
    nvs_handle_t h;
    esp_err_t e = s_usable ? open_rw(NVS_READONLY, &h) : ESP_ERR_NVS_NOT_INITIALIZED;
    if (e == ESP_OK) {
        rc = read_view(h, "remote", NET_URL_MAX, &rv);
        if (rc == PR_READ_FOUND) {
            memcpy(s_remote_raw, rv.data, rv.len);
        }
        pc = read_view(h, "pair", NET_PAIR_MAX_BYTES, &pv);
        nvs_close(h);
    } else if (s_usable) {
        pc = rc = pr_read_class(e, false);
    }
    if (!pr_load_usable(pc, rc) || (pv.present && pv.len > cap)) {
        if (s_usable) {
            note("store unusable this boot:", "load", e);
        }
        s_usable = false;
        unlock();
        return 0;
    }
    s_remote_id = rv.present ? rv.id : 0;
    s_remote_len = rv.present ? rv.len : 0;
    s_cur_id = pv.present ? pv.id : 0;
    s_pair_len = pv.present ? pv.len : 0;
    s_max_id = s_cur_id > s_remote_id ? s_cur_id : s_remote_id;
    if (rv.present && memchr(s_remote_raw, 0, rv.len) == NULL) {
        memcpy(url, s_remote_raw, rv.len);
        url[rv.len] = 0;
        *remote_id = rv.id;
    }
    if (pv.present) {
        memcpy(text, pv.data, pv.len);
        out = pv.len;
        *pair_id = pv.id;
    }
    unlock();
    return out;
}

/* Reads key back after a fresh init of the partition, which is what the next boot loads. */
static bool read_back(const char *key, size_t max, pr_view_t *got)
{
    got->present = false;
    nvs_flash_deinit_partition(NET_NVS_PARTITION);
    if (nvs_flash_init_partition(NET_NVS_PARTITION) != ESP_OK) {
        s_usable = false;
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

/* Writes the record in s_blob; after any error the outcome follows what the store holds, and a broken store is unusable. */
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
    if (!n || !s_usable) {
        note("not saved:", key, e);
        return PR_WRITE_KEPT;
    }
    pr_view_t got = {false, 0, NULL, 0};
    bool read_ok = read_back(key, max, &got);
    int out = pr_write_outcome(false, read_ok, got, want, prev);
    if (out == PR_WRITE_BROKEN) {
        s_usable = false;
    }
    note(out == PR_WRITE_DONE ? "saved despite an error:" : out == PR_WRITE_KEPT ? "not saved:" : "store unusable after a failed write:",
         key, e);
    return out;
}

bool netstore_save_pair(const uint8_t *text, size_t len, uint32_t *pair_id)
{
    if (!lock()) {
        return false;
    }
    uint32_t id = pr_next_id(s_max_id, 0);
    size_t n = pr_encode(id, text, len, NET_PAIR_MAX_BYTES, s_blob, sizeof s_blob);
    pr_view_t want = {true, id, text, len}, prev = {s_cur_id != 0, s_cur_id, NULL, s_pair_len};
    bool done = write_record("pair", n, NET_PAIR_MAX_BYTES, want, prev) == PR_WRITE_DONE;
    if (done) {
        s_cur_id = id;
        s_pair_len = len;
        s_max_id = id;
        *pair_id = id;
    }
    unlock();
    return done;
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
    bool done = write_record("remote", n, NET_URL_MAX, want, prev) == PR_WRITE_DONE;
    if (done) {
        s_remote_id = pair_id;
        s_remote_len = len;
        memcpy(s_remote_raw, url, len);
    }
    unlock();
    return done ? NS_SAVED : NS_FAILED;
}

bool netstore_forget(void)
{
    if (!lock()) {
        return false;
    }
    /* Runs whatever the store state: erase deinitialises an initialised partition first and works on one that never initialised. */
    esp_err_t e = nvs_flash_erase_partition(NET_NVS_PARTITION);
    esp_err_t ie = e == ESP_OK ? nvs_flash_init_partition(NET_NVS_PARTITION) : ESP_ERR_INVALID_STATE;
    s_usable = e == ESP_OK && ie == ESP_OK;
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
    unlock();
    return s_usable;
}
