#include "imu.h"
#include <math.h>
#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orient.h"
#include "transport.h"

static const char *TAG = "imu";

enum { REG_WHO_AM_I = 0x00, REG_CTRL1 = 0x02, REG_CTRL2 = 0x03, REG_CTRL7 = 0x08, REG_STATUS0 = 0x2E, REG_AX_L = 0x35 };

static i2c_master_dev_handle_t s_dev;

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

static void imu_task(void *arg)
{
    uint32_t last = now_ms(), last_log = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(IMU_PERIOD_MS));
        uint8_t raw[6];
        if (reg_read(REG_AX_L, raw, sizeof(raw)) != ESP_OK) {
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
        if (t - last_log >= IMU_LOG_PERIOD_MS) {
            last_log = t;
            ESP_LOGI(TAG, "raw g x=%.2f y=%.2f z=%.2f up=(%.2f,%.2f) target=%.1f", a[0], a[1], a[2], ux, uy, orient_target());
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
    esp_err_t w1 = reg_write(REG_CTRL1, IMU_CTRL1_VALUE), w2 = reg_write(REG_CTRL2, IMU_CTRL2_VALUE),
              w7 = reg_write(REG_CTRL7, IMU_CTRL7_ACCEL_ON);
    uint8_t rb[8] = {0};
    reg_read(REG_CTRL1, rb, 8);
    ESP_LOGI(TAG, "ctrl write %d/%d/%d readback ctrl1=%02x ctrl2=%02x ctrl7=%02x", (int)w1, (int)w2, (int)w7, rb[0], rb[1], rb[6]);
    xTaskCreate(imu_task, "imu", STACK_IMU, NULL, PRIO_IMU, NULL);
    ESP_LOGI(TAG, "QMI8658 up");
}
