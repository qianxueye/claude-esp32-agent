#include <stdio.h>
#include "imu.h"
#include <math.h>
#include <string.h>
#include "app_config.h"
#include "audio.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "motion.h"
#include "orient.h"
#include "transport.h"
#include "ui.h"

static const char *TAG = "imu";

enum { REG_WHO_AM_I = 0x00, REG_CTRL1 = 0x02, REG_CTRL2 = 0x03, REG_CTRL5 = 0x06, REG_CTRL7 = 0x08, REG_STATUS0 = 0x2E, REG_TIMESTAMP_L = 0x30, REG_AX_L = 0x35 };
#define SAMPLE_BLOCK_BYTES 11 /* timestamp(3), temperature(2), accelerometer(6) */
#define FROZEN_SAMPLES (2000 / IMU_PERIOD_MS)

static i2c_master_dev_handle_t s_dev;
static motion_t s_motion;
static volatile int s_dump_req;
static volatile uint32_t s_dump_gen;

#define DUMP_BATCH 5
#define DUMP_MAX_S 600

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, 2, IMU_I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, IMU_I2C_TIMEOUT_MS);
}

static bool probe(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = IMU_I2C_HZ};
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        return false;
    }
    uint8_t id = 0;
    if (reg_read(REG_WHO_AM_I, &id, 1) == ESP_OK && id == IMU_WHO_AM_I) {
        return true;
    }
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return false;
}

static float pick(const float a[3], int src, int sign)
{
    return sign * a[src];
}

void imu_set_dump(int seconds)
{
    seconds = seconds > DUMP_MAX_S ? DUMP_MAX_S : seconds;
    __atomic_store_n(&s_dump_req, seconds > 0 ? seconds * 1000 / IMU_PERIOD_MS : 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_dump_gen, 1, __ATOMIC_RELEASE);
}

/* Logs raw counts five samples per line; a missing index in the sequence marks a lost line or a failed read. */
static void dump_step(const uint8_t raw[6], bool ok, uint32_t *gen, int *left, int *idx, int *n, int *first, int16_t (*batch)[3])
{
    uint32_t g = __atomic_load_n(&s_dump_gen, __ATOMIC_ACQUIRE);
    if (*gen != g) {
        *gen = g;
        *left = __atomic_load_n(&s_dump_req, __ATOMIC_RELAXED);
        *idx = 0;
        *n = 0;
    }
    if (*left <= 0) {
        return;
    }
    if (!ok) {
        *n = 0;
    } else {
        if (*n == 0) {
            *first = *idx;
        }
        for (int i = 0; i < 3; i++) {
            batch[*n][i] = (int16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
        }
        (*n)++;
    }
    (*idx)++;
    (*left)--;
    if (*n == DUMP_BATCH) {
        ESP_LOGI(TAG, "imu_dump i=%d %d,%d,%d;%d,%d,%d;%d,%d,%d;%d,%d,%d;%d,%d,%d", *first, batch[0][0], batch[0][1], batch[0][2], batch[1][0],
                 batch[1][1], batch[1][2], batch[2][0], batch[2][1], batch[2][2], batch[3][0], batch[3][1], batch[3][2], batch[4][0], batch[4][1],
                 batch[4][2]);
        *n = 0;
    }
    if (*left == 0) {
        ESP_LOGI(TAG, "imu_dump done n=%d", *idx);
    }
}

/* Configuration is changed only while the sensor is disabled; the chip keeps running across a reset of this board. */
static bool configure(uint8_t *w)
{
    bool ok = reg_write(REG_CTRL7, 0) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(IMU_DISABLE_SETTLE_MS));
    w[0] = reg_write(REG_CTRL1, IMU_CTRL1_VALUE) == ESP_OK;
    w[1] = reg_write(REG_CTRL2, IMU_CTRL2_VALUE) == ESP_OK;
    w[2] = reg_write(REG_CTRL5, IMU_CTRL5_VALUE) == ESP_OK;
    w[3] = reg_write(REG_CTRL7, IMU_CTRL7_ACCEL_ON) == ESP_OK;
    return ok && w[0] && w[1] && w[2] && w[3];
}

static void imu_task(void *arg)
{
    uint32_t last = now_ms(), last_log = 0, dump_gen = 0;
    int dump_idx = 0, dump_n = 0, dump_first = 0, dump_left = 0;
    int16_t dump_batch[DUMP_BATCH][3];
    uint8_t prev[6] = {0};
    uint32_t ts_last_log = 0, same = 0;
    int warm = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(IMU_PERIOD_MS));
        uint8_t blk[SAMPLE_BLOCK_BYTES], *raw = blk + 5;
        bool ok = reg_read(REG_TIMESTAMP_L, blk, sizeof(blk)) == ESP_OK;
        dump_step(raw, ok, &dump_gen, &dump_left, &dump_idx, &dump_n, &dump_first, dump_batch);
        if (!ok) {
            continue;
        }
        if (warm < IMU_STARTUP_SKIP_SAMPLES) {
            warm++;
            continue;
        }
        uint32_t ts = blk[0] | (blk[1] << 8) | ((uint32_t)blk[2] << 16);
        same = memcmp(prev, raw, 6) == 0 ? same + 1 : 0;
        memcpy(prev, raw, 6);
        if (same == FROZEN_SAMPLES) {
            uint8_t w[4];
            bool cfg = configure(w);
            ESP_LOGW(TAG, "imu frozen: data identical for %u samples, ts=%06x, reconfigured=%d", (unsigned)same, (unsigned)ts, (int)cfg);
            same = 0;
            warm = 0;
            motion_reset(&s_motion);
            continue;
        }
        float a[3];
        for (int i = 0; i < 3; i++) {
            a[i] = (int16_t)(raw[2 * i] | (raw[2 * i + 1] << 8)) * (IMU_RANGE_G / 32768.0f);
        }
        float ux = pick(a, IMU_UP_X_SRC, IMU_UP_X_SIGN), uy = pick(a, IMU_UP_Y_SRC, IMU_UP_Y_SIGN);
        uint32_t t = now_ms();
        orient_feed(ux, uy, t - last);
        last = t;
        int ev = motion_feed(&s_motion, a, t, audio_pa_active());
        if (ev) {
            char extra[40] = "";
            if (ev & MOTION_EV_REST) {
                snprintf(extra, sizeof extra, " spread=%.1fmg from=%s", s_motion.rest_spread * 1000.0f, s_motion.rest_from == MOTION_LIFTED ? "LIFTED" : "MOVING");
            }
            ESP_LOGI(TAG, "motion: ev=%s D=%.1fmg th=%.1f/%.1f/%.1f amp=%d%s", ev & MOTION_EV_PICKUP ? "pickup" : (ev & MOTION_EV_MOVING ? "moving" : "rest"), s_motion.d * 1000.0f, s_motion.cfg.theta_on * 1000.0f,
                     s_motion.cfg.theta_off * 1000.0f, s_motion.cfg.theta_pickup * 1000.0f, (int)audio_pa_active(), extra);
        }
        if (s_motion.state == MOTION_LIFTED) {
            ui_wake(DP_CAUSE_MOTION);
        }
        if (ev & MOTION_EV_SETDOWN) {
            ui_wake(DP_CAUSE_SETDOWN);
        }
        if (t - last_log >= IMU_LOG_PERIOD_MS) {
            last_log = t;
            ESP_LOGI(TAG, "raw g x=%.2f y=%.2f z=%.2f up=(%.2f,%.2f) target=%.1f D=%.2fmg st=%d ts_d=%u same=%u", a[0], a[1], a[2], ux, uy,
                     orient_target(), s_motion.d * 1000.0f, (int)s_motion.state,
                     (unsigned)((ts - ts_last_log) & 0xFFFFFF), (unsigned)same);
            ts_last_log = ts;
        }
    }
}

void imu_start(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!probe(bus, IMU_I2C_ADDR_PRIMARY) && !probe(bus, IMU_I2C_ADDR_SECONDARY)) {
        ESP_LOGW(TAG, "QMI8658 not found");
        return;
    }
    uint8_t w[4];
    configure(w);
    int w1 = !w[0], w2 = !w[1], w5 = !w[2], w7 = !w[3];
    uint8_t rb[8] = {0};
    reg_read(REG_CTRL1, rb, 8);
    ESP_LOGI(TAG, "ctrl write %d/%d/%d/%d readback ctrl1=%02x ctrl2=%02x ctrl5=%02x ctrl7=%02x", w1, w2, w5, w7, rb[0], rb[1],
             rb[4], rb[6]);
    motion_init(&s_motion, NULL);
    xTaskCreate(imu_task, "imu", STACK_IMU, NULL, PRIO_IMU, NULL);
    ESP_LOGI(TAG, "QMI8658 up");
}
